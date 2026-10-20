/* AmiStudio Host - pairing codes, paired tokens and the failed-handshake ban (LAUNCHER-PLAN 2.5).
 * confdir/tokens   : "sha256(token) id created [u=last_used] [c=client_id] client" per line (u=/c= since 0.1.1;
 *                    0.1 files without them still load). Only hashes are stored, so the file can't be used to
 *                    connect. Re-pairing the same Amiga (same client id, or same name from the same address when
 *                    it sends no id) replaces its old token instead of adding another line. The Amiga keeps the token itself in ENVARC:AmiStudio/HostToken.
 * confdir/pair-code: "code expires_unix attempts" - one-time code written by `amistudio-hostd pair` or by the
 *                    daemon at start when nothing is paired yet. */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_TOKENS 64
#define CODE_TRIES 5          /* wrong codes before the current code is thrown away */
#define BAN_FAILS 5           /* failed handshakes within BAN_WINDOW s -> refused for BAN_TIME s (as ASBC) */
#define BAN_WINDOW 60
#define BAN_TIME 300
#define USED_SAVE_S 60        /* last-used time is written to the tokens file at most once a minute per token */
#define CID_MAX 64

typedef struct { char hash[65], id[16], client[ASH_MAX_CLIENT + 1], cid[CID_MAX + 1]; long long created, used; } tok;
static tok g_tok[MAX_TOKENS]; static int g_ntok;
static plat_mutex *g_am;

static void path(char *out, int n, const char *f) { snprintf(out, n, "%s/%s", g_cfg.confdir, f); }
static void load_tokens(void) {
    char p[600], line[300]; FILE *f; path(p, sizeof p, "tokens"); g_ntok = 0;
    if (!(f = fopen(p, "r"))) return;
    while (fgets(line, sizeof line, f) && g_ntok < MAX_TOKENS) {
        tok t; memset(&t, 0, sizeof t); int off = 0;
        if (line[0] == '#' || sscanf(line, "%64s %15s %lld %n", t.hash, t.id, &t.created, &off) < 3 || strlen(t.hash) != 64) continue;
        char *q = line + off;
        for (;;) {                                   /* optional key=value fields before the client name */
            while (*q == ' ') q++;
            int n = 0;
            if (!strncmp(q, "u=", 2) && sscanf(q + 2, "%lld%n", &t.used, &n) == 1) q += 2 + n;
            else if (!strncmp(q, "c=", 2)) { q += 2; while (*q && *q != ' ' && *q != '\n' && n < CID_MAX) t.cid[n++] = *q++; t.cid[n] = 0; while (*q && *q != ' ' && *q != '\n') q++; }
            else break;
        }
        snprintf(t.client, sizeof t.client, "%s", q); trim(t.client);
        g_tok[g_ntok++] = t;
    }
    fclose(f);
}
static int save_tokens(void) {
    char p[600], *b = malloc(MAX_TOKENS * 240 + 240); int o = 0; if (!b) return -1;
    o += snprintf(b + o, 200, "# AmiStudio Host paired clients: sha256(token) id created u=last_used [c=client_id] client. Remove a line to unpair.\n");
    for (int i = 0; i < g_ntok; i++) o += snprintf(b + o, 240, "%s %s %lld u=%lld%s%s %s\n", g_tok[i].hash, g_tok[i].id, g_tok[i].created, g_tok[i].used,
                                                    g_tok[i].cid[0] ? " c=" : "", g_tok[i].cid, g_tok[i].client);
    path(p, sizeof p, "tokens"); int r = plat_write_private(p, b, (size_t)o); free(b); return r;
}
int auth_init(void) { if (!g_am) g_am = plat_mutex_new(); plat_lock(g_am); load_tokens(); plat_unlock(g_am); return 0; }
int auth_has_tokens(void) { plat_lock(g_am); load_tokens(); int n = g_ntok; plat_unlock(g_am); return n > 0; }

int auth_check_token(const char *t, char *id, int idn) {
    char h[65]; int ok = 0;
    if (!t || strlen(t) != ASH_TOKEN_HEX) return 0;
    sha256_hex(t, strlen(t), h);
    plat_lock(g_am); load_tokens();                /* re-read: `revoke` from the CLI takes effect at once */
    for (int i = 0; i < g_ntok; i++) if (ct_equal(h, g_tok[i].hash)) { ok = 1; if (id) snprintf(id, idn, "%s", g_tok[i].id);
        long long now = plat_time(); if (now - g_tok[i].used >= USED_SAVE_S) { g_tok[i].used = now; save_tokens(); } }
    plat_unlock(g_am);
    return ok;
}

static int read_code(char code[8], long long *exp, int *tries) {
    char p[600]; FILE *f; path(p, sizeof p, "pair-code");
    if (!(f = fopen(p, "r"))) return -1;
    int r = fscanf(f, "%7s %lld %d", code, exp, tries); fclose(f);
    return r == 3 ? 0 : -1;
}
static void drop_code(void) { char p[600]; path(p, sizeof p, "pair-code"); remove(p); }
int auth_new_code(char out[7], int minutes) {
    unsigned char r[4]; if (plat_random(r, 4)) return -1;
    unsigned v = ((unsigned)r[0] << 24 | (unsigned)r[1] << 16 | (unsigned)r[2] << 8 | r[3]) % 1000000u;
    snprintf(out, 7, "%06u", v);
    char p[600], b[64]; path(p, sizeof p, "pair-code");
    int n = snprintf(b, sizeof b, "%s %lld 0\n", out, (long long)plat_time() + minutes * 60);
    if (!g_am) g_am = plat_mutex_new();
    plat_lock(g_am); int rc = plat_write_private(p, b, (size_t)n); plat_unlock(g_am);
    return rc;
}
/* the same Amiga pairing again: same client id if it sent one, else the same name (which includes " @address") */
static int same_client(const tok *t, const char *client, const char *cid) {
    if (cid && *cid) return !strcmp(t->cid, cid);
    return !t->cid[0] && !strcmp(t->client, client);
}
int auth_pair(const char *code, const char *client, const char *cid, char *tok_out, char *msg, int mn) {
    char c[8] = ""; long long exp = 0; int tries = 0, rc = -1;
    plat_lock(g_am);
    if (read_code(c, &exp, &tries)) { snprintf(msg, mn, "No pairing code is active. Run 'amistudio-hostd pair' on the PC."); goto out; }
    if (plat_time() > exp) { drop_code(); snprintf(msg, mn, "The pairing code has expired. Run 'amistudio-hostd pair' again."); goto out; }
    if (strlen(code) != 6 || !ct_equal(code, c)) {
        if (++tries >= CODE_TRIES) { drop_code(); snprintf(msg, mn, "Wrong code. Too many tries: make a new code with 'amistudio-hostd pair'."); }
        else { char p[600], b[64]; path(p, sizeof p, "pair-code"); int n = snprintf(b, sizeof b, "%s %lld %d\n", c, exp, tries); plat_write_private(p, b, (size_t)n);
               snprintf(msg, mn, "Wrong code (%d tries left).", CODE_TRIES - tries); }
        goto out;
    }
    load_tokens();
    tok saved[MAX_TOKENS]; int nsaved = g_ntok; memcpy(saved, g_tok, sizeof(tok) * (size_t)g_ntok);
    char cl[ASH_MAX_CLIENT + 1]; snprintf(cl, sizeof cl, "%s", client && *client ? client : "Amiga");
    for (char *q = cl; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
    char ci[CID_MAX + 1] = ""; int cn = 0;
    if (cid) for (const char *q = cid; *q && cn < CID_MAX; q++) if (*q > ' ' && *q < 127) ci[cn++] = *q;
    ci[cn] = 0;
    int j = 0, replaced = 0; char old[160] = "";
    for (int i = 0; i < g_ntok; i++) { if (same_client(&g_tok[i], cl, ci)) { replaced++; if (strlen(old) < 140) snprintf(old + strlen(old), sizeof old - strlen(old), " %s", g_tok[i].id); continue; } g_tok[j++] = g_tok[i]; }
    g_ntok = j;
    if (g_ntok >= MAX_TOKENS) { load_tokens(); snprintf(msg, mn, "Too many paired clients; revoke some with 'amistudio-hostd revoke'."); goto out; }
    unsigned char r[32]; if (plat_random(r, 32)) { load_tokens(); snprintf(msg, mn, "No random numbers"); goto out; }
    hex(r, 32, tok_out);
    tok t; memset(&t, 0, sizeof t); sha256_hex(tok_out, 64, t.hash); snprintf(t.id, sizeof t.id, "%.8s", t.hash);
    t.created = t.used = plat_time(); snprintf(t.client, sizeof t.client, "%s", cl); snprintf(t.cid, sizeof t.cid, "%s", ci);
    g_tok[g_ntok++] = t;
    if (save_tokens()) { memcpy(g_tok, saved, sizeof(tok) * (size_t)nsaved); g_ntok = nsaved; snprintf(msg, mn, "Cannot save the tokens file"); goto out; }
    drop_code();
    if (replaced) snprintf(msg, mn, "Paired as %s (replaces%s)", t.id, old); else snprintf(msg, mn, "Paired as %s", t.id);
    rc = 0;
out:
    plat_unlock(g_am);
    return rc;
}
int auth_list(void) {
    auth_init();
    if (!g_ntok) { printf("No paired clients.\n"); return 0; }
    printf("%-10s %-17s %-17s %s\n", "id", "paired", "last used", "client");
    for (int i = 0; i < g_ntok; i++) { char d[32], u[32] = "never"; plat_fmt_time(g_tok[i].created, d, sizeof d); if (g_tok[i].used > 0) plat_fmt_time(g_tok[i].used, u, sizeof u);
        if (g_tok[i].cid[0]) printf("%-10s %-17s %-17s %s  [client id %s]\n", g_tok[i].id, d, u, g_tok[i].client, g_tok[i].cid);
        else printf("%-10s %-17s %-17s %s\n", g_tok[i].id, d, u, g_tok[i].client); }
    return g_ntok;
}
int auth_revoke(const char *id) {
    int n = 0, j = 0; auth_init(); plat_lock(g_am);
    for (int i = 0; i < g_ntok; i++) { if (!strcmp(id, "all") || (strlen(id) >= 4 && !strncmp(g_tok[i].id, id, strlen(id)))) { n++; continue; } g_tok[j++] = g_tok[i]; }
    g_ntok = j; if (n) save_tokens();
    plat_unlock(g_am); return n;
}

/* ---- failed handshake ban (per peer address, in memory) ---- */
typedef struct { char peer[64]; long long t[BAN_FAILS]; int n; long long until; } ban;
static ban g_ban[64];
static ban *ban_get(const char *peer, int make) {
    ban *free_ = NULL, *old = &g_ban[0];
    for (int i = 0; i < 64; i++) { if (!strcmp(g_ban[i].peer, peer)) return &g_ban[i]; if (!g_ban[i].peer[0] && !free_) free_ = &g_ban[i];
        if (g_ban[i].until < old->until) old = &g_ban[i]; }
    if (!make) return NULL;
    ban *b = free_ ? free_ : old; memset(b, 0, sizeof *b); snprintf(b->peer, sizeof b->peer, "%s", peer); return b;
}
int auth_banned(const char *peer) {
    plat_lock(g_am); ban *b = ban_get(peer, 0); int r = b && b->until > plat_time(); plat_unlock(g_am); return r;
}
void auth_fail(const char *peer) {
    plat_lock(g_am);
    ban *b = ban_get(peer, 1); long long now = plat_time(); int k = 0;
    for (int i = 0; i < b->n; i++) if (now - b->t[i] < BAN_WINDOW) b->t[k++] = b->t[i];
    b->n = k; if (b->n < BAN_FAILS) b->t[b->n++] = now;
    if (b->n >= BAN_FAILS) { b->until = now + BAN_TIME; b->n = 0; LOG("%s: %d failed handshakes in %d s, refused for %d min", peer, BAN_FAILS, BAN_WINDOW, BAN_TIME / 60); }
    plat_unlock(g_am);
}
