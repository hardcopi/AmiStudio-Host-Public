/* AmiStudio Host - child-process service backends ("service" blocks in host.conf).
 *
 * This is how existing backends are mounted without merging their code into the Host (LAUNCHER-PLAN 2.2):
 * the Host starts the backend as a child listening on a random loopback port with a random internal token,
 * does the backend's own handshake for the client, then passes bytes through untouched. AmiStudio Browser's
 * asb-chromed fits as-is:
 *
 *   service browser
 *   proto 2                      # ASBC v2; clients ask for "browser" protocol 2
 *   exec /opt/amistudio/asb-chromed -port {port} -token {token} -idle 0
 *   handshake asbc2              # Host sends "ASBC" v2 + token to the child; the client skips its own ASBC handshake
 *   compat_port 9223             # optional: plain ASBC listener for Browser 0.79-0.81 (token = a paired Host token)
 *   start ondemand               # or "boot"
 *   ready_timeout 30
 *   log browser.log              # child stdout/stderr, relative to the config dir
 *
 * Placeholders in exec: {port} {token} {confdir} {name}, {port2} (a second free loopback port, e.g. Chrome's
 * private debug port), {state} (the Host's data folder, ~/.local/share/amistudio-host or data_dir) and {config}
 * (= {confdir}), quoted. handshake none = raw passthrough right after ASH OK.
 * stop_timeout N (seconds, default 15): on Host stop the backend gets SIGTERM alone and N seconds to finish (asb-chromed
 * in app mode closes Chrome cleanly so logins persist), then its whole process group is ended.
 *
 * A reaper thread checks every backend once a second (0.1.1): a backend that exits is reaped at once (no zombie),
 * the exit is logged, hostinfo shows "idle exited <how>" and the next client starts it again. "start boot"
 * backends are restarted by the reaper itself, at most once every 5 s. */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    plat_mutex *m;
    char exec[1024], handshake[16], log[512];
    int boot, ready_ms, compat_port, stop_ms;
    plat_proc proc; int port, port2; char token[33]; int64_t started;
    char last_err[160];
    int exits; int64_t last_exit, next_try;   /* reaper bookkeeping */
} proxy;

static int split_args(const char *line, char **argv, int max, char *store, int sn) {
    int n = 0, o = 0; const char *p = line;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[n++] = store + o;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) { if (*p == '"') { q = !q; p++; continue; } if (o < sn - 1) store[o++] = *p; p++; }
        store[o++] = 0; if (o >= sn) return -1;
    }
    argv[n] = NULL; return n;
}
static void subst(const char *in, char *out, int n, proxy *px, const char *name) {
    int o = 0; char v[600];
    while (*in && o < n - 1) {
        const char *rep = NULL;
        if (!strncmp(in, "{port}", 6)) { snprintf(v, sizeof v, "%d", px->port); rep = v; in += 6; }
        else if (!strncmp(in, "{token}", 7)) { rep = px->token; in += 7; }
        else if (!strncmp(in, "{confdir}", 9)) { snprintf(v, sizeof v, "\"%s\"", g_cfg.confdir); rep = v; in += 9; }
        else if (!strncmp(in, "{name}", 6)) { rep = name; in += 6; }
        else if (!strncmp(in, "{port2}", 7)) { snprintf(v, sizeof v, "%d", px->port2); rep = v; in += 7; }
        else if (!strncmp(in, "{state}", 7)) { snprintf(v, sizeof v, "\"%s\"", g_cfg.data_dir); rep = v; in += 7; }
        else if (!strncmp(in, "{config}", 8)) { snprintf(v, sizeof v, "\"%s\"", g_cfg.confdir); rep = v; in += 8; }
        if (rep) { int l = (int)strlen(rep); if (o + l >= n) break; memcpy(out + o, rep, (size_t)l); o += l; }
        else out[o++] = *in++;
    }
    out[o] = 0;
}
/* start the child if it is not running; caller holds px->m */
static void note_exit(service *sv, proxy *px, int how) {
    px->exits++; px->last_exit = plat_now_ms();
    if (how < 0) snprintf(px->last_err, sizeof px->last_err, "exited signal %d", -how);
    else snprintf(px->last_err, sizeof px->last_err, "exited status %d", how);
    LOG("%s: backend %s after %lld s; %s", sv->name, px->last_err, (long long)((px->last_exit - px->started) / 1000),
        px->boot ? "restarting" : "it starts again for the next client");
}
static int ensure(service *sv, proxy *px, char *msg, int mn) {
    int how = 0, rr = plat_proc_reap(&px->proc, &how);
    if (rr == 1) return 0;
    if (rr == 0) note_exit(sv, px, how);
    if (px->started) LOG("%s: starting backend again", sv->name);
    unsigned char r[16]; plat_random(r, 16); hex(r, 16, px->token);
    px->port = plat_free_port();
    if (!px->port) { snprintf(msg, mn, "%s: no free port", sv->name); return -1; }
    px->port2 = 0;
    if (strstr(px->exec, "{port2}")) for (int t = 0; t < 10 && (!px->port2 || px->port2 == px->port); t++) px->port2 = plat_free_port();
    char cmd[2048], store[2048], *argv[64], lp[700], err[200];
    subst(px->exec, cmd, sizeof cmd, px, sv->name);
    if (split_args(cmd, argv, 64, store, sizeof store) < 1) { snprintf(msg, mn, "%s: bad exec line", sv->name); return -1; }
    snprintf(lp, sizeof lp, "%s/%s", g_cfg.confdir, px->log);
    if (plat_spawn(argv, px->log[0] ? lp : NULL, &px->proc, err, sizeof err)) {
        snprintf(msg, mn, "%s backend did not start: %s", sv->name, err); snprintf(px->last_err, sizeof px->last_err, "%s", err); LOG("%s", msg); return -1; }
    px->started = plat_now_ms(); px->last_err[0] = 0;
    LOG("%s: started backend pid %ld on 127.0.0.1:%d (log %s)", sv->name, px->proc.pid, px->port, px->log[0] ? lp : "-");
    for (int waited = 0; waited < px->ready_ms; waited += 200) {
        sock_t t = plat_connect_tcp("127.0.0.1", px->port, 500);
        if (t != SOCK_BAD) {
            /* probe connection: for ASBC send a bad-token handshake so the backend logs nothing worse than a reject;
               simplest is to close at once, which asb-chromed treats as "bad handshake" */
            plat_close(t); return 0; }
        if (plat_proc_reap(&px->proc, NULL) != 1) { snprintf(msg, mn, "%s backend exited during start (see %s)", sv->name, lp); snprintf(px->last_err, sizeof px->last_err, "exited during start"); LOG("%s", msg); return -1; }
        plat_sleep_ms(200);
    }
    snprintf(msg, mn, "%s backend did not open its port within %d s", sv->name, px->ready_ms / 1000);
    snprintf(px->last_err, sizeof px->last_err, "port did not open"); LOG("%s", msg);
    plat_proc_kill(&px->proc);
    return -1;
}
/* connect to the running child and do its handshake. Returns the socket or SOCK_BAD. ver: ASBC version to ask for */
static sock_t backend_open(service *sv, proxy *px, unsigned ver, int do_hs, char *msg, int mn) {
    plat_lock(px->m);
    int r = ensure(sv, px, msg, mn); int port = px->port; char tok[33]; snprintf(tok, sizeof tok, "%s", px->token);
    plat_unlock(px->m);
    if (r) return SOCK_BAD;
    sock_t b = plat_connect_tcp("127.0.0.1", port, 3000);
    if (b == SOCK_BAD) { snprintf(msg, mn, "%s backend is not answering", sv->name); return SOCK_BAD; }
    plat_nodelay(b);
    if (do_hs && !strcasecmp(px->handshake, "asbc2")) {
        unsigned char h[7 + 32], a[7]; memcpy(h, "ASBC", 4); put_u16(h + 4, ver); h[6] = 32; memcpy(h + 7, tok, 32);
        plat_set_timeout(b, 10000);
        if (plat_send_all(b, h, sizeof h) || plat_recv_all(b, a, 7) || memcmp(a, "ASBC", 4) || a[6] != 0) {
            snprintf(msg, mn, "%s backend refused the handshake", sv->name); plat_close(b); return SOCK_BAD; }
        plat_set_timeout(b, 0);
    }
    return b;
}
static void splice(sock_t a, sock_t b) {
    static __thread unsigned char buf[65536];
    for (;;) {
        int w = plat_wait2(a, b, 60000);
        if (w < 0) return;
        if (w & 1) { long k = plat_recv(a, buf, sizeof buf); if (k <= 0 || plat_send_all(b, buf, (size_t)k)) return; }
        if (w & 2) { long k = plat_recv(b, buf, sizeof buf); if (k <= 0 || plat_send_all(a, buf, (size_t)k)) return; }
    }
}
/* 0.1.6: for asbc2 backends the Host's first frame is ASBC CMD (op 18) "asbhost client KEY": KEY is the same
   client identity as the session-slot takeover rule (paired token id, or a hash of client id + address for loopback),
   so asb-chromed in app mode keeps per-client view state (zoom) for the same client and resets it for a new one.
   Older engines ignore the unknown command. */
static void send_client_key(sock_t b, const conn *c) {
    char key[80], h[65], id[ASH_MAX_CLIENT + 70];
    if (c->token_id[0]) snprintf(key, sizeof key, "t%s", c->token_id);
    else { snprintf(id, sizeof id, "%s|%s", c->client, c->peer); sha256_hex(id, strlen(id), h); snprintf(key, sizeof key, "l%.16s", h); }
    char cmd[120]; int n = snprintf(cmd, sizeof cmd, "asbhost client %s", key);
    unsigned char f[5 + 120]; put_u32(f, (uint32_t)n + 1); f[4] = 18; memcpy(f + 5, cmd, (size_t)n);
    plat_send_all(b, f, (size_t)n + 5);
}
static int px_prepare(service *sv, conn *c, char *msg, int mn) {
    proxy *px = sv->priv;
    c->backend = backend_open(sv, px, c->want_ver, 1, msg, mn);
    if (c->backend != SOCK_BAD && !strcasecmp(px->handshake, "asbc2")) send_client_key(c->backend, c);
    return c->backend == SOCK_BAD ? ASH_REFUSED : ASH_OK;
}
static void px_run(service *sv, conn *c) { (void)sv; splice(c->s, c->backend); }
static void px_status(service *sv, char *out, int n) {
    proxy *px = sv->priv; plat_lock(px->m);
    int how = 0, r = plat_proc_reap(&px->proc, &how);
    if (r == 0) note_exit(sv, px, how);
    if (r == 1) snprintf(out, n, "up");
    else if (!strncmp(px->last_err, "exited signal", 13) || !strncmp(px->last_err, "exited status", 13)) snprintf(out, n, "idle %s", px->last_err);   /* ran, then quit: restarts on demand */
    else if (px->last_err[0]) snprintf(out, n, "down %s", px->last_err);
    else snprintf(out, n, "idle");
    plat_unlock(px->m);
}
static void px_stop(service *sv) { proxy *px = sv->priv; plat_lock(px->m); if (plat_proc_alive(&px->proc)) { LOG("%s: stopping backend", sv->name); plat_proc_stop(&px->proc, px->stop_ms); } plat_unlock(px->m); }

service *svc_proxy_new(const char *name, const char *const *kv, char *err, int en) {
    service *sv = calloc(1, sizeof *sv); proxy *px = calloc(1, sizeof *px);
    snprintf(sv->name, sizeof sv->name, "%s", name);
    sv->vmin = sv->vmax = 1; sv->need_auth = 1; sv->priv = px;
    sv->prepare = px_prepare; sv->run = px_run; sv->status = px_status; sv->stop = px_stop;
    px->m = plat_mutex_new(); px->ready_ms = 30000; px->stop_ms = 15000; snprintf(px->handshake, sizeof px->handshake, "none"); snprintf(px->log, sizeof px->log, "%s.log", name);
    for (int i = 0; kv[i] && kv[i + 1]; i += 2) {
        const char *k = kv[i], *v = kv[i + 1];
        if (!strcmp(k, "proto")) { unsigned a = 1, b = 0; int n = sscanf(v, "%u %u", &a, &b); sv->vmin = a; sv->vmax = n == 2 ? b : a; }
        else if (!strcmp(k, "exec")) snprintf(px->exec, sizeof px->exec, "%s", v);
        else if (!strcmp(k, "handshake")) snprintf(px->handshake, sizeof px->handshake, "%s", v);
        else if (!strcmp(k, "start")) px->boot = !strcasecmp(v, "boot");
        else if (!strcmp(k, "ready_timeout")) px->ready_ms = atoi(v) * 1000;
        else if (!strcmp(k, "compat_port")) px->compat_port = atoi(v);
        else if (!strcmp(k, "max_clients")) sv->max_clients = atoi(v);
        else if (!strcmp(k, "stop_timeout")) px->stop_ms = atoi(v) * 1000;
        else if (!strcmp(k, "log")) snprintf(px->log, sizeof px->log, "%s", strcmp(v, "none") ? v : "");
        else if (!strcmp(k, "auth")) sv->need_auth = strcasecmp(v, "no") != 0;
        else LOG("service %s: unknown key '%s' ignored", name, k);
    }
    if (!px->exec[0]) { snprintf(err, en, "service %s: 'exec' is required", name); free(px); free(sv); return NULL; }
    if (strcasecmp(px->handshake, "asbc2") && strcasecmp(px->handshake, "none")) { snprintf(err, en, "service %s: handshake must be asbc2 or none", name); free(px); free(sv); return NULL; }
    if (px->compat_port && strcasecmp(px->handshake, "asbc2")) { snprintf(err, en, "service %s: compat_port needs handshake asbc2", name); free(px); free(sv); return NULL; }
    if (sv->vmin > sv->vmax) { snprintf(err, en, "service %s: bad proto range", name); free(px); free(sv); return NULL; }
    return sv;
}

/* ---- plain ASBC listeners (compat_port) for Browser clients that do not speak ASH yet ----
 * The client's own ASBC handshake is checked here (loopback, or a paired Host token as TOKEN=), then replaced with
 * the child's internal token, and the rest is passed through. */
typedef struct { service *sv; sock_t ls; } compat_l;
typedef struct { service *sv; conn c; } compat_c;
static void compat_conn(void *a) {
    host_client(+1);
    compat_c *cc = a; service *sv = cc->sv; proxy *px = sv->priv; conn *c = &cc->c; unsigned char h[7]; char tok[65] = "", msg[200];
    plat_set_timeout(c->s, 10000);
    if (auth_banned(c->peer)) goto done;
    if (plat_recv_all(c->s, h, 7) || memcmp(h, "ASBC", 4) || h[6] > 64 || (h[6] && plat_recv_all(c->s, tok, h[6]))) goto done;
    tok[h[6]] = 0;
    c->authed = c->loopback || auth_check_token(tok, c->token_id, sizeof c->token_id);
    if (!c->authed) { unsigned char no[7]; memcpy(no, "ASBC", 4); put_u16(no + 4, get_u16(h + 4)); no[6] = 1; plat_send_all(c->s, no, 7);
        auth_fail(c->peer); LOG("%s: %s compat port: bad token", c->peer, sv->name); goto done; }
    if (session_begin(sv, c)) goto done;
    c->backend = backend_open(sv, px, get_u16(h + 4), 0, msg, sizeof msg);
    if (c->backend != SOCK_BAD) {
        unsigned char f[7 + 32]; memcpy(f, h, 6); f[6] = 32; memcpy(f + 7, px->token, 32);
        if (!plat_send_all(c->backend, f, sizeof f)) { plat_set_timeout(c->s, 0); DBG("%s: %s via compat port %d", c->peer, sv->name, px->compat_port); splice(c->s, c->backend); }
        plat_close(c->backend);
    } else LOG("%s: %s compat: %s", c->peer, sv->name, msg);
    session_end(sv, c);
done:
    plat_close(c->s); free(cc);
    host_client(-1);
}
static void compat_accept(void *a) {
    compat_l *l = a;
    while (!g_stop) {
        if (plat_wait_readable(l->ls, 500) <= 0) continue;
        compat_c *cc = calloc(1, sizeof *cc); cc->sv = l->sv; cc->c.backend = SOCK_BAD;
        cc->c.s = plat_accept(l->ls, cc->c.peer, sizeof cc->c.peer, &cc->c.loopback);
        if (cc->c.s == SOCK_BAD) { free(cc); continue; }
        plat_nodelay(cc->c.s);
        if (plat_thread(compat_conn, cc)) { plat_close(cc->c.s); free(cc); }
    }
}
/* reaper: notices exited backends within a second instead of at the next connection */
static void reaper(void *a) {
    (void)a;
    while (!g_stop) {
        plat_sleep_ms(1000);
        for (int i = 0; i < g_nsvcs && !g_stop; i++) {
            service *sv = g_svcs[i]; if (sv->prepare != px_prepare) continue;
            proxy *px = sv->priv; int how = 0;
            plat_lock(px->m);
            if (plat_proc_reap(&px->proc, &how) == 0) { note_exit(sv, px, how); px->next_try = plat_now_ms() + (plat_now_ms() - px->started < 5000 ? 5000 : 0); }
            if (px->boot && !g_stop && px->started && px->proc.pid <= 0 && plat_now_ms() >= px->next_try) {
                char msg[200]; if (ensure(sv, px, msg, sizeof msg)) px->next_try = plat_now_ms() + 5000; }
            plat_unlock(px->m);
        }
    }
}
int svc_proxy_compat_listeners(void) {
    int any = 0;
    for (int i = 0; i < g_nsvcs; i++) if (g_svcs[i]->prepare == px_prepare) any = 1;
    if (any) plat_thread(reaper, NULL);
    for (int i = 0; i < g_nsvcs; i++) {
        service *sv = g_svcs[i]; if (sv->prepare != px_prepare) continue;
        proxy *px = sv->priv; char err[200];
        if (px->boot) { char msg[200]; plat_lock(px->m); ensure(sv, px, msg, sizeof msg); plat_unlock(px->m); }
        if (!px->compat_port) continue;
        compat_l *l = calloc(1, sizeof *l); l->sv = sv;
        l->ls = plat_listen_tcp(g_cfg.listen, px->compat_port, err, sizeof err);
        if (l->ls == SOCK_BAD) { LOG("%s: compat listener: %s (is the old asb-chromed still running?)", sv->name, err); free(l); return -1; }
        LOG("  %s: plain ASBC on %s:%d for older Browser clients", sv->name, g_cfg.listen, px->compat_port);
        plat_thread(compat_accept, l);
    }
    return 0;
}
