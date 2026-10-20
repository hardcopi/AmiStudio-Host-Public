/* amistudio-hostd - the AmiStudio Host daemon (LAUNCHER-PLAN 2, ASH v1 on port 9220).
 * One port; each TCP connection names a service in the handshake and from then on carries that service's own
 * protocol. Built in: hostinfo, pair, fetch. Child-process backends (Browser's asb-chromed, later pdf, drive...)
 * are mounted from host.conf "service" blocks. */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int g_clients; static int64_t g_last_activity;   /* guarded by g_lock */
void host_client(int delta) { plat_lock(g_lock); g_clients += delta; g_last_activity = plat_now_ms(); plat_unlock(g_lock); }
int64_t parse_idle_ms(const char *v) { char *e; double x = strtod(v, &e); if (x < 0) x = 0; return (int64_t)(x * (*e == 's' ? 1000 : 60000)); }
host_cfg g_cfg; plat_mutex *g_lock; service *g_svcs[MAX_SERVICES]; int g_nsvcs; int64_t g_started_ms; volatile int g_stop;

void service_add(service *sv) {
    for (int i = 0; i < g_nsvcs; i++) if (!strcmp(g_svcs[i]->name, sv->name)) { LOG("service %s defined twice; the later one wins", sv->name); g_svcs[i] = sv; return; }
    if (g_nsvcs < MAX_SERVICES) g_svcs[g_nsvcs++] = sv;
}
/* ---- session slots (0.1.4): a dropped client no longer keeps a max_clients slot when it reconnects ---- */
#define MAX_SESS 256
static struct { service *sv; conn *c; } g_sess[MAX_SESS];
static int same_client(const conn *a, const conn *b) {
    if (a->token_id[0] || b->token_id[0]) return !strcmp(a->token_id, b->token_id);     /* paired: same token */
    return a->client[0] && !strcmp(a->client, b->client) && !strcmp(a->peer, b->peer);  /* loopback: id + address */
}
int session_begin(service *sv, conn *c) {
    int kicked = 0, slot = -1;
    plat_lock(g_lock);
    if (sv->max_clients) {
        int live = sv->active;   /* sessions not already replaced (a kicked one still counts in active until it ends) */
        for (int i = 0; i < MAX_SESS; i++) if (g_sess[i].sv == sv && g_sess[i].c->kicked) live--;
        for (int i = 0; i < MAX_SESS && live >= sv->max_clients; i++)
            if (g_sess[i].sv == sv && !g_sess[i].c->kicked && same_client(g_sess[i].c, c)) {
                g_sess[i].c->kicked = 1; plat_shutdown(g_sess[i].c->s); kicked++; live--; }
        if (live >= sv->max_clients) { plat_unlock(g_lock); return 1; }
    }
    for (int i = 0; i < MAX_SESS; i++) if (!g_sess[i].c) { slot = i; break; }
    if (slot >= 0) { g_sess[slot].sv = sv; g_sess[slot].c = c; }
    sv->active++;
    plat_unlock(g_lock);
    if (kicked) LOG("%s (%s): %s reconnect replaces its stale session", c->peer, c->client[0] ? c->client : "?", sv->name);
    return 0;
}
void session_end(service *sv, conn *c) {
    plat_lock(g_lock);
    for (int i = 0; i < MAX_SESS; i++) if (g_sess[i].c == c) { g_sess[i].c = NULL; g_sess[i].sv = NULL; break; }
    sv->active--;
    plat_unlock(g_lock);
}
/* ---- 0.1.8 busy(): a service that keeps the Host up without a client (amidrive with amidrive_keeps_host yes and a
   mount). Asked without g_lock held (the services use their own locks). ---- */
static int svcs_busy(void) { int n = 0; for (int i = 0; i < g_nsvcs; i++) if (g_svcs[i]->busy && g_svcs[i]->busy(g_svcs[i])) n++; return n; }
/* CONFDIR/busy exists while some service is busy: the launcher's run-host.sh watcher counts it as activity */
static void busy_file(int busy) {
    static int last = -1; if (busy == last) return;
    char p[600]; snprintf(p, sizeof p, "%s/busy", g_cfg.confdir);
    if (busy) { char b[200]; int o = 0; for (int i = 0; i < g_nsvcs && o < 150; i++) if (g_svcs[i]->busy && g_svcs[i]->busy(g_svcs[i])) o += snprintf(b + o, sizeof b - o, "%s\n", g_svcs[i]->name);
        if (!o) o = snprintf(b, sizeof b, "busy\n");
        plat_write_private(p, b, (size_t)o); }
    else remove(p);
    if (last != -1) LOG("%s", busy ? "a service keeps the Host up without a client (busy)" : "no service busy any more");
    last = busy;
}
/* ---- quit-when-unused (0.1.6, control service) ---- */
static int g_quit_armed, g_quit_now, g_quit_grace_s = CTL_GRACE_DEFAULT; static int64_t g_quit_zero_since;   /* guarded by g_lock */
static int is_app_service(const service *sv) { return strcmp(sv->name, "hostinfo") && strcmp(sv->name, "pair") && strcmp(sv->name, "control"); }
static int app_sessions_locked(void) { int n = 0; for (int i = 0; i < MAX_SESS; i++) if (g_sess[i].c && !g_sess[i].c->kicked && is_app_service(g_sess[i].sv)) n++; return n; }
int host_app_sessions(void) { plat_lock(g_lock); int n = app_sessions_locked(); plat_unlock(g_lock); return n; }
int host_quit_arm(int grace_s, int *others) {
    if (!g_cfg.allow_quit) { if (others) *others = host_app_sessions(); return CTL_NOT_ARMED; }
    if (grace_s <= 0) grace_s = CTL_GRACE_DEFAULT;
    if (grace_s > CTL_GRACE_MAX) grace_s = CTL_GRACE_MAX;
    int b = svcs_busy();
    plat_lock(g_lock);
    int n = app_sessions_locked() + b; g_quit_armed = 1; g_quit_grace_s = grace_s; g_quit_zero_since = 0;
    if (!n) g_quit_now = 1;
    plat_unlock(g_lock);
    if (others) *others = n;
    return n ? CTL_ARMED : CTL_STOPPING;
}
void host_quit_cancel(void) { plat_lock(g_lock); if (!g_quit_now) { g_quit_armed = 0; g_quit_zero_since = 0; } plat_unlock(g_lock); }
int host_quit_state(int *others, int *grace_s) {
    int b = svcs_busy();
    plat_lock(g_lock); int st = g_quit_now ? CTL_STOPPING : g_quit_armed ? CTL_ARMED : CTL_NOT_ARMED; if (others) *others = app_sessions_locked() + b; if (grace_s) *grace_s = g_quit_grace_s; plat_unlock(g_lock);
    return st;
}
/* main loop, every <= 0.5 s: 1 = stop now */
static int quit_tick(int busy) {
    plat_lock(g_lock);
    int stop = 0, n = app_sessions_locked() + busy; int64_t now = plat_now_ms();
    if (g_quit_now) stop = 1;
    else if (g_quit_armed) {
        if (n) g_quit_zero_since = 0;
        else if (!g_quit_zero_since) g_quit_zero_since = now;
        else if (now - g_quit_zero_since >= (int64_t)g_quit_grace_s * 1000) stop = 2;
    }
    plat_unlock(g_lock);
    if (stop == 1) LOG("quit-when-unused: no other client: stopping");
    if (stop == 2) LOG("quit-when-unused: the last client left %d s ago: stopping", g_quit_grace_s);
    return stop;
}
service *service_find(const char *name) { for (int i = 0; i < g_nsvcs; i++) if (!strcmp(g_svcs[i]->name, name)) return g_svcs[i]; return NULL; }

static void reply(sock_t s, int status, unsigned ver, const char *msg) {
    unsigned char b[4 + 2 + 1 + 2 + 1 + 255]; size_t ml = strlen(msg); if (ml > 255) ml = 255;
    memcpy(b, ASH_MAGIC, 4); put_u16(b + 4, ASH_VERSION); b[6] = (unsigned char)status; put_u16(b + 7, ver); b[9] = (unsigned char)ml;
    memcpy(b + 10, msg, ml); plat_send_all(s, b, 10 + ml);
}
static int rd_str(sock_t s, char *out, int max) {
    unsigned char n; if (plat_recv_all(s, &n, 1)) return -1;
    if (n > max) return -1;
    if (n && plat_recv_all(s, out, n)) return -1;
    out[n] = 0; for (int i = 0; i < n; i++) if ((unsigned char)out[i] < 32) out[i] = '?';
    return n;
}

static void conn_main(void *arg) {
    conn *c = arg; host_client(+1); unsigned char h[6]; char tok[ASH_MAX_TOKEN + 1] = "", msg[256] = ""; service *sv = NULL; int st;
    plat_set_timeout(c->s, 10000);
    if (auth_banned(c->peer)) { reply(c->s, ASH_REFUSED, 0, "Too many failed attempts from this address; try again in a few minutes."); goto done; }
    if (plat_recv_all(c->s, h, 6) || memcmp(h, ASH_MAGIC, 4)) { DBG("%s: not an ASH client", c->peer); goto done; }
    unsigned char vb[2];
    if (rd_str(c->s, c->service, ASH_MAX_NAME) < 1 || plat_recv_all(c->s, vb, 2) || rd_str(c->s, tok, ASH_MAX_TOKEN) < 0 || rd_str(c->s, c->client, ASH_MAX_CLIENT) < 0) {
        DBG("%s: bad handshake", c->peer); goto done; }
    c->want_ver = get_u16(vb);
    if (get_u16(h + 4) != ASH_VERSION) { reply(c->s, get_u16(h + 4) < ASH_VERSION ? ASH_TOO_OLD : ASH_TOO_NEW, 0, "This Host speaks ASH 1."); goto done; }
    if (!(sv = service_find(c->service))) { reply(c->s, ASH_NO_SERVICE, 0, "No such service on this Host."); LOG("%s: unknown service '%s'", c->peer, c->service); goto done; }
    c->authed = c->loopback;
    if (tok[0]) {
        if (auth_check_token(tok, c->token_id, sizeof c->token_id)) c->authed = 1;
        else if (!c->loopback && sv->need_auth) { auth_fail(c->peer); LOG("%s (%s): bad token for %s", c->peer, c->client, c->service);
            reply(c->s, ASH_BAD_TOKEN, 0, "This Amiga is not paired with this Host (or was unpaired). Pair again."); goto done; }
    }
    if (sv->need_auth && !c->authed) { reply(c->s, ASH_BAD_TOKEN, 0, "Pair this Amiga with the Host first (6-digit code)."); goto done; }
    if (c->want_ver < sv->vmin) { snprintf(msg, sizeof msg, "%s needs protocol %u or newer; please update the app.", sv->name, sv->vmin); reply(c->s, ASH_TOO_OLD, sv->vmin, msg); goto done; }
    if (c->want_ver > sv->vmax) { snprintf(msg, sizeof msg, "This Host's %s speaks up to protocol %u; please update the Host.", sv->name, sv->vmax); reply(c->s, ASH_TOO_NEW, sv->vmax, msg); goto done; }
    c->svc_ver = c->want_ver;
    if (session_begin(sv, c)) { reply(c->s, ASH_BUSY, c->svc_ver, "The service is busy with another client."); goto done; }
    st = sv->prepare ? sv->prepare(sv, c, msg, sizeof msg) : ASH_OK;
    plat_lock(g_lock); int kicked = c->kicked; plat_unlock(g_lock);
    if (kicked) { DBG("%s: %s session replaced before it started", c->peer, sv->name); }
    else if (st != ASH_OK) { reply(c->s, st, c->svc_ver, msg[0] ? msg : "Service unavailable."); LOG("%s: %s refused: %s", c->peer, sv->name, msg); }
    else {
        reply(c->s, ASH_OK, c->svc_ver, msg[0] ? msg : "ok");
        DBG("%s (%s%s%s): %s v%u", c->peer, c->client[0] ? c->client : "?", c->token_id[0] ? ", token " : "", c->token_id, sv->name, c->svc_ver);
        plat_set_timeout(c->s, 0);
        sv->run(sv, c);
        plat_lock(g_lock); int k = c->kicked; plat_unlock(g_lock);
        if (k) DBG("%s (%s): %s session closed (the same client reconnected)", c->peer, c->client[0] ? c->client : "?", sv->name);
    }
    session_end(sv, c);
done:
    if (c->backend != SOCK_BAD) plat_close(c->backend);
    plat_close(c->s); free(c);
    host_client(-1);
}

/* ---- the Host's own update check: "component host" (or "app host") in the release manifest ---- */
static int allow_raw(const char *h) { return !strcasecmp(h, "raw.githubusercontent.com") || fetch_host_allowed(h); }
typedef struct { char *b; size_t n, cap; } mbuf;
static int mb_body(void *ud, const unsigned char *b, size_t n) { mbuf *m = ud; if (m->n + n > 65536) return 1;
    if (m->n + n + 1 > m->cap) { m->cap = m->n + n + 4096; m->b = realloc(m->b, m->cap); } memcpy(m->b + m->n, b, n); m->n += n; m->b[m->n] = 0; return 0; }
static int vcmp(const char *a, const char *b) { while (*a || *b) { long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10); if (x != y) return x < y ? -1 : 1; if (*a == '.') a++; if (*b == '.') b++; if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9'))) break; } return 0; }
void host_update_check(void) {
    mbuf m = { 0 }; char err[200];
    int st = http_get(g_cfg.manifest_url, allow_raw, NULL, mb_body, &m, err, sizeof err);
    if (st != 200 || !m.b) { DBG("update check: %s", st < 0 ? err : "no manifest"); free(m.b); return; }
    /* "component host" (0.1.1+: PC parts are component blocks, which Amiga launchers skip); "app host" still read */
    char *p = strstr(m.b, "\ncomponent host\n"), ver[24] = ""; int skip = 16;
    if (!p) { p = strstr(m.b, "\napp host\n"); skip = 10; }
    if (p) { p += skip; while (*p && *p != '\n') { if (!strncmp(p, "status soon", 11)) { ver[0] = 0; break; } if (!strncmp(p, "ver ", 4)) sscanf(p + 4, "%23s", ver);
            p = strchr(p, '\n'); if (!p || p[1] == '\n') break; p++; } }
    plat_lock(g_lock); snprintf(g_cfg.update_ver, sizeof g_cfg.update_ver, "%s", ver[0] && vcmp(ver, HOST_VERSION) > 0 ? ver : ""); plat_unlock(g_lock);
    if (g_cfg.update_ver[0]) LOG("AmiStudio Host %s is available (this is %s)", g_cfg.update_ver, HOST_VERSION);
    free(m.b);
}
static void update_thread(void *a) { (void)a; plat_sleep_ms(5000); while (!g_stop) { host_update_check(); for (int i = 0; i < 24 * 3600 && !g_stop; i++) plat_sleep_ms(1000); } }

/* ---- CLI ---- */
static void on_stop(void) { g_stop = 1; }
static void usage(void) {
    fprintf(stderr,
        "AmiStudio Host %s - serves AmiStudio apps on the Amiga (ASH v%d, port %d)\n\n"
        "usage: amistudio-hostd [run] [options]     run the Host\n"
        "       amistudio-hostd pair [--minutes N]  make a 6-digit pairing code (default 10 minutes)\n"
        "       amistudio-hostd tokens              list paired Amigas\n"
        "       amistudio-hostd revoke ID|all       unpair\n"
        "       amistudio-hostd status [HOST[:PORT]] ask a running Host for its status\n"
        "       amistudio-hostd amidrive status|signin [--url|--amisurf]|connect|flush|unmount [--force|--later]|signout [--revoke]\n"
        "                                           Google Drive on the running Host (options: --port N, --host H)\n\n"
        "options: --lan               listen on all interfaces (LAN clients must pair); default 127.0.0.1 only\n"
        "         --listen ADDR       listen on one address\n"
        "         --port N            ASH port (default %d)\n"
        "         --config FILE       default: CONFDIR/host.conf if it exists\n"
        "         --confdir DIR       default: ~/.config/amistudio-host (or $AMISTUDIO_HOST_DIR)\n"
        "         --data-dir DIR      {state} for services, e.g. AmiDiscord's Chrome profile (default ~/.local/share/amistudio-host)\n"
        "         --browser PATH      mount AmiStudio Browser's asb-chromed as the 'browser' service\n"
        "         --browser-idle MIN  bundled engine quits after MIN minutes without a client (default 30, 0 = never)\n"
        "         --exit-idle MIN     stop the Host after MIN minutes with no client connection (0 = never, default;\n"
        "                             any connection counts, e.g. the launcher's status checks; '45s' = seconds)\n"
        "         --no-bundled-browser  do not mount the engine/ folder next to amistudio-hostd as 'browser'\n"
        "         --amidiscord-idle MIN  the AmiDiscord engine (Chrome, logged in) quits MIN minutes after the last\n"
        "                             AmiDiscord client left (default 30, 0 = never)\n"
        "         --no-bundled-amidiscord  do not mount the engine in app mode as 'amidiscord'\n"
        "         --no-remote-quit    ignore the launcher's quit-when-unused request (control service)\n"
        "         --name NAME         name shown on the Amiga (default: this computer's name)\n"
        "         --no-discovery      do not answer LAN discovery broadcasts\n"
        "         --no-update-check   do not look for a newer Host in AmiStudio-Releases\n"
        "         -v                  verbose log\n\n"
        "Never forward the port to the internet; use a VPN or SSH tunnel for remote use.\n",
        HOST_VERSION, ASH_VERSION, ASH_PORT, ASH_PORT);
}
int ash_status_main(const char *hp);   /* status.c */
typedef struct { service *sv; } stop_arg;
static int g_stopping_n;   /* guarded by g_lock */
static void stop_thread(void *a) { service *sv = ((stop_arg *)a)->sv; free(a); sv->stop(sv); plat_lock(g_lock); g_stopping_n--; plat_unlock(g_lock); }
/* 0.1.8: every service stops at the same time, so amidrive's flush/unmount (up to amidrive_stop_timeout) and the
   engines' stop_timeout do not add up */
static void stop_all(void) {
    for (int k = 0; k < g_nsvcs; k++) if (g_svcs[k]->stop) {
        stop_arg *a = malloc(sizeof *a); a->sv = g_svcs[k];
        plat_lock(g_lock); g_stopping_n++; plat_unlock(g_lock);
        if (plat_thread(stop_thread, a)) { plat_lock(g_lock); g_stopping_n--; plat_unlock(g_lock); g_svcs[k]->stop(g_svcs[k]); free(a); }
    }
    for (;;) { plat_lock(g_lock); int n = g_stopping_n; plat_unlock(g_lock); if (!n) break; plat_sleep_ms(50); }
}
/* the release tarball's defaults (INSTALLDIR/host-defaults.conf: the AmiStudio OAuth client for amidrive), read before
   CONFDIR/host.conf, whose settings win */
static void load_defaults(void) {
    char dir[700], p[800], err[300]; FILE *f;
    if (plat_exe_dir(dir, sizeof dir)) return;
    snprintf(p, sizeof p, "%s/host-defaults.conf", dir);
    if (!(f = fopen(p, "r"))) return;
    fclose(f); if (config_load(p, err, sizeof err)) LOG("%s", err);
}
static void set_defaults(void) {
    g_cfg.amidrive = 1; g_cfg.amidrive_automount = 1; g_cfg.amidrive_keeps_host = 0; g_cfg.amidrive_stop_s = 60;
    g_cfg.amidrive_retry_s = 60; g_cfg.amidrive_auth_s = 600; g_cfg.amidrive_auth_check = 1;
}

int main(int argc, char **argv) {
    const char *cli_exitidle = NULL, *cli_data = NULL;
    const char *cmd = "run", *config = NULL, *browser = NULL, *cli_listen = NULL, *cli_name = NULL; int cli_lan = -1, cli_port = 0, cli_disc = -1, cli_upd = -1, minutes = 10, cli_bidle = -1, cli_bundled = -1, cli_didle = -1, cli_dbundled = -1, cli_noquit = 0;
    int i = 1;
    if (argc > 1 && argv[1][0] != '-') { cmd = argv[1]; i = 2; }
    memset(&g_cfg, 0, sizeof g_cfg); set_defaults();
    g_cfg.port = ASH_PORT; g_cfg.bundled_browser = 1; g_cfg.browser_idle = 30; g_cfg.bundled_amidiscord = 1; g_cfg.amidiscord_idle = 30; g_cfg.allow_quit = 1; g_cfg.discovery = 1; g_cfg.update_check = 1; g_cfg.fetch_max = 64L * 1048576L;
    snprintf(g_cfg.listen, sizeof g_cfg.listen, "127.0.0.1");
    snprintf(g_cfg.fetch_allow, sizeof g_cfg.fetch_allow, "github.com raw.githubusercontent.com objects.githubusercontent.com release-assets.githubusercontent.com codeload.github.com");
    snprintf(g_cfg.manifest_url, sizeof g_cfg.manifest_url, "https://raw.githubusercontent.com/hardcopi/AmiStudio-Releases/main/manifest.txt");
    plat_config_dir(g_cfg.confdir, sizeof g_cfg.confdir);
    plat_data_dir(g_cfg.data_dir, sizeof g_cfg.data_dir);
    if (!strcmp(cmd, "amidrive")) {   /* client of the running Host's amidrive service (tools/amidrive_cli.c) */
        plat_net_init(); g_lock = plat_mutex_new();
        const char *h = "127.0.0.1"; int pt = 0; char *rest[16]; int nr = 0;
        for (int k = 2; k < argc; k++) {
            if (!strcmp(argv[k], "--port") && k + 1 < argc) pt = atoi(argv[++k]);
            else if (!strcmp(argv[k], "--host") && k + 1 < argc) h = argv[++k];
            else if (!strcmp(argv[k], "--confdir") && k + 1 < argc) snprintf(g_cfg.confdir, sizeof g_cfg.confdir, "%s", argv[++k]);
            else if (nr < 16) rest[nr++] = argv[k];
        }
        if (!pt) { char cp[600], err[300]; snprintf(cp, sizeof cp, "%s/host.conf", g_cfg.confdir); FILE *f = fopen(cp, "r");
            if (f) { fclose(f); g_verbose = 0; config_load(cp, err, sizeof err); } pt = g_cfg.port; }
        return amidrive_cli(h, pt, getenv("ASH_TOKEN"), nr, rest);
    }
    const char *positional = NULL;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--lan")) cli_lan = 1;
        else if (!strcmp(a, "--listen") && i + 1 < argc) cli_listen = argv[++i];
        else if (!strcmp(a, "--port") && i + 1 < argc) cli_port = atoi(argv[++i]);
        else if (!strcmp(a, "--config") && i + 1 < argc) config = argv[++i];
        else if (!strcmp(a, "--confdir") && i + 1 < argc) snprintf(g_cfg.confdir, sizeof g_cfg.confdir, "%s", argv[++i]);
        else if (!strcmp(a, "--browser") && i + 1 < argc) browser = argv[++i];
        else if (!strcmp(a, "--data-dir") && i + 1 < argc) cli_data = argv[++i];
        else if (!strcmp(a, "--name") && i + 1 < argc) cli_name = argv[++i];
        else if (!strcmp(a, "--minutes") && i + 1 < argc) minutes = atoi(argv[++i]);
        else if (!strcmp(a, "--no-discovery")) cli_disc = 0;
        else if (!strcmp(a, "--browser-idle") && i + 1 < argc) cli_bidle = atoi(argv[++i]);
        else if (!strcmp(a, "--no-bundled-browser")) cli_bundled = 0;
        else if (!strcmp(a, "--amidiscord-idle") && i + 1 < argc) cli_didle = atoi(argv[++i]);
        else if (!strcmp(a, "--no-bundled-amidiscord")) cli_dbundled = 0;
        else if (!strcmp(a, "--no-remote-quit")) cli_noquit = 1;
        else if (!strcmp(a, "--exit-idle") && i + 1 < argc) cli_exitidle = argv[++i];
        else if (!strcmp(a, "--no-update-check")) cli_upd = 0;
        else if (!strcmp(a, "-v")) g_verbose = 1;
        else if (!strcmp(a, "--version")) { printf("AmiStudio Host %s (ASH %d, %s)\n", HOST_VERSION, ASH_VERSION, plat_os_name()); return 0; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] != '-' && !positional) positional = a;
        else { fprintf(stderr, "amistudio-hostd: unknown option %s\n\n", a); usage(); return 2; }
    }
    plat_net_init();
    g_lock = plat_mutex_new();
    if (plat_mkdir_private(g_cfg.confdir)) { fprintf(stderr, "amistudio-hostd: cannot create %s\n", g_cfg.confdir); return 1; }
    auth_init();
    if (!strcmp(cmd, "pair")) {
        char code[7]; if (minutes < 1 || minutes > 1440) minutes = 10;
        if (auth_new_code(code, minutes)) { fprintf(stderr, "amistudio-hostd: cannot write the pairing code\n"); return 1; }
        printf("Pairing code: %c%c%c %c%c%c\nEnter it in AmiStudio on the Amiga (Hosts... > Pair) within %d minutes. It works once.\n",
               code[0], code[1], code[2], code[3], code[4], code[5], minutes);
        return 0;
    }
    if (!strcmp(cmd, "tokens")) { auth_list(); return 0; }
    if (!strcmp(cmd, "revoke")) { if (!positional) { fprintf(stderr, "usage: amistudio-hostd revoke ID|all\n"); return 2; }
        int n = auth_revoke(positional); printf("%d client%s unpaired\n", n, n == 1 ? "" : "s"); return n ? 0 : 1; }
    if (!strcmp(cmd, "status")) return ash_status_main(positional);
    if (strcmp(cmd, "run")) { fprintf(stderr, "amistudio-hostd: unknown command %s\n\n", cmd); usage(); return 2; }

    /* run */
    char err[300], path[600];
    service_add(svc_hostinfo_new()); service_add(svc_pair_new()); service_add(svc_fetch_new()); service_add(svc_control_new());
    load_defaults();
    if (!config) { snprintf(path, sizeof path, "%s/host.conf", g_cfg.confdir); FILE *f = fopen(path, "r"); if (f) { fclose(f); config = path; } }
    if (config && config_load(config, err, sizeof err)) { LOG("%s", err); return 1; }
    if (g_cfg.amidrive && !service_find("amidrive")) service_add(svc_amidrive_new());
    if (browser) {
        const char *kv[] = { "proto", "2", "exec", NULL, "handshake", "asbc2", "compat_port", "9223", "log", "browser.log", NULL };
        static char ex[700]; snprintf(ex, sizeof ex, "%s -port {port} -token {token} -idle 0", browser); kv[3] = ex;
        service *sv = svc_proxy_new("browser", kv, err, sizeof err); if (!sv) { LOG("%s", err); return 1; } service_add(sv);
    }
    if (cli_data) snprintf(g_cfg.data_dir, sizeof g_cfg.data_dir, "%s", cli_data);
    /* 0.1.5: services see the Host's data folder ({state}) as AMISTUDIO_HOST_DATA, so an engine whose exec line does
       not set ASB_STATE still keeps its data (AmiDiscord's downloads too) in <data>/<app id>, --data-dir included */
    if (g_cfg.data_dir[0]) plat_setenv("AMISTUDIO_HOST_DATA", g_cfg.data_dir);
    if (cli_bidle >= 0) g_cfg.browser_idle = cli_bidle;
    if (cli_exitidle) g_cfg.exit_idle_ms = parse_idle_ms(cli_exitidle);
    if (cli_bundled >= 0) g_cfg.bundled_browser = cli_bundled;
    if (cli_didle >= 0) g_cfg.amidiscord_idle = cli_didle;
    if (cli_dbundled >= 0) g_cfg.bundled_amidiscord = cli_dbundled;
    if (cli_noquit) g_cfg.allow_quit = 0;
    /* the release tarball ships AmiSurf's engine (asb-chromed + Chromium) in engine/ next to the binary: mount it as
       "browser" unless host.conf or --browser already did */
    if (g_cfg.bundled_browser && !service_find("browser")) {
        char dir[700], es[800]; FILE *t;
        if (!plat_exe_dir(dir, sizeof dir)) {
            snprintf(es, sizeof es, "%s/engine/engine.sh", dir);
            if ((t = fopen(es, "r"))) { fclose(t);
                static char ex[1000], idle[16]; snprintf(idle, sizeof idle, "%d", g_cfg.browser_idle < 0 ? 0 : g_cfg.browser_idle);
                snprintf(ex, sizeof ex, "sh \"%s\" -port {port} -token {token} -cdp {port2} -idle %s", es, idle);
                const char *kv[] = { "proto", "2", "exec", ex, "handshake", "asbc2", "ready_timeout", "40", "log", "browser.log", NULL };
                service *sv = svc_proxy_new("browser", kv, err, sizeof err); if (!sv) { LOG("%s", err); return 1; }
                service_add(sv); LOG("bundled browser engine: %s (stops after %s min idle, restarts on demand)", es, idle); } }
    }
    /* 0.1.4: the same engine in app mode serves AmiDiscord (discord.com/app, own Chrome profile and config, no tabs,
       external links go to the Amiga). Chrome stays up amidiscord_idle minutes after the last client, so Discord
       stays logged in and keeps notifying across short disconnects. */
    if (g_cfg.bundled_amidiscord && !service_find("amidiscord")) {
        char dir[700], es[800]; FILE *t;
        if (!plat_exe_dir(dir, sizeof dir)) {
            snprintf(es, sizeof es, "%s/engine/engine.sh", dir);
            if ((t = fopen(es, "r"))) { fclose(t);
                static char ex[1400], idle[16]; snprintf(idle, sizeof idle, "%d", g_cfg.amidiscord_idle < 0 ? 0 : g_cfg.amidiscord_idle);
                snprintf(ex, sizeof ex, "env ASB_APP=amidiscord ASB_APP_URL=https://discord.com/app ASB_APP_TITLE=AmiDiscord ASB_APP_EXTERNAL=client "
                         "ASB_STATE={state}/amidiscord ASB_CONFIG={state}/amidiscord/config "
                         "sh \"%s\" -port {port} -token {token} -cdp {port2} -idle %s", es, idle);
                const char *kv[] = { "proto", "2", "exec", ex, "handshake", "asbc2", "ready_timeout", "40", "max_clients", "1", "log", "amidiscord.log", "stop_timeout", "20", NULL };
                service *sv = svc_proxy_new("amidiscord", kv, err, sizeof err); if (!sv) { LOG("%s", err); return 1; }
                service_add(sv); LOG("bundled AmiDiscord engine: app mode, discord.com/app (Chrome stays up %s min after the last client)", idle); } }
    }
    if (cli_lan == 1) { g_cfg.lan = 1; snprintf(g_cfg.listen, sizeof g_cfg.listen, "0.0.0.0"); }
    if (cli_listen) { snprintf(g_cfg.listen, sizeof g_cfg.listen, "%s", cli_listen); g_cfg.lan = strncmp(cli_listen, "127.", 4) != 0; }
    if (cli_port) g_cfg.port = cli_port;
    if (cli_disc >= 0) g_cfg.discovery = cli_disc;
    if (cli_upd >= 0) g_cfg.update_check = cli_upd;
    if (cli_name) snprintf(g_cfg.name, sizeof g_cfg.name, "%s", cli_name);
    if (!g_cfg.name[0]) plat_hostname(g_cfg.name, sizeof g_cfg.name);
    { snprintf(path, sizeof path, "%s/host-id", g_cfg.confdir); FILE *f = fopen(path, "r");
      if (f) { if (fscanf(f, "%16s", g_cfg.id) != 1) g_cfg.id[0] = 0; fclose(f); }
      if (strlen(g_cfg.id) != 16) { unsigned char r[8]; plat_random(r, 8); hex(r, 8, g_cfg.id); char b[24]; int n = snprintf(b, sizeof b, "%s\n", g_cfg.id); plat_write_private(path, b, (size_t)n); } }
    sock_t ls = plat_listen_tcp(g_cfg.listen, g_cfg.port, err, sizeof err);
    if (ls == SOCK_BAD) { LOG("cannot listen on %s", err); return 1; }
    { char bp[600]; snprintf(bp, sizeof bp, "%s/busy", g_cfg.confdir); remove(bp); }   /* a killed Host's flag */
    for (int k = 0; k < g_nsvcs; k++) if (g_svcs[k]->init) g_svcs[k]->init(g_svcs[k]);
    g_started_ms = plat_now_ms();
    plat_on_signal(on_stop);
    LOG("AmiStudio Host %s (%s) on %s:%d, config %s%s", HOST_VERSION, plat_os_name(), g_cfg.listen, g_cfg.port, g_cfg.confdir, g_cfg.lan ? ", LAN clients must pair" : ", this computer only");
    for (int k = 0; k < g_nsvcs; k++) { char pv[24]; service *v = g_svcs[k];
        if (v->vmin == v->vmax) snprintf(pv, sizeof pv, "%u", v->vmax); else snprintf(pv, sizeof pv, "%u-%u", v->vmin, v->vmax);
        LOG("  service %s (protocol %s)%s", v->name, pv, v->need_auth ? "" : ", no pairing needed"); }
    if (svc_proxy_compat_listeners()) return 1;
    if (g_cfg.lan && g_cfg.discovery) discovery_start();
    if (g_cfg.lan && !auth_has_tokens()) {
        char code[7]; if (!auth_new_code(code, 30)) LOG("no Amiga paired yet: pairing code %s (valid 30 minutes; new one: amistudio-hostd pair)", code); }
    if (g_cfg.update_check) plat_thread(update_thread, NULL);
    g_last_activity = plat_now_ms();
    if (g_cfg.exit_idle_ms) LOG("idle shutdown: the Host stops after %lld s with no client connection", (long long)(g_cfg.exit_idle_ms / 1000));
    int was_busy = 0;
    while (!g_stop) {
        int busy = svcs_busy();
        busy_file(busy > 0);
        if (was_busy && !busy) { plat_lock(g_lock); g_last_activity = plat_now_ms(); plat_unlock(g_lock); }   /* idle counts from now */
        was_busy = busy;
        if (quit_tick(busy)) break;
        if (g_cfg.exit_idle_ms) {
            plat_lock(g_lock); int n = g_clients; int64_t quiet = plat_now_ms() - g_last_activity; plat_unlock(g_lock);
            if (!n && !busy && quiet >= g_cfg.exit_idle_ms) { LOG("no client for %lld s: stopping (idle shutdown)", (long long)(quiet / 1000)); break; }
        }
        if (plat_wait_readable(ls, 500) <= 0) continue;
        conn *c = calloc(1, sizeof *c); if (!c) continue;
        c->backend = SOCK_BAD;
        c->s = plat_accept(ls, c->peer, sizeof c->peer, &c->loopback);
        if (c->s == SOCK_BAD) { free(c); continue; }
        plat_nodelay(c->s);
        if (plat_thread(conn_main, c)) { plat_close(c->s); free(c); }
    }
    LOG("stopping");
    g_stop = 1;
    plat_close(ls);   /* no new clients while the services stop */
    stop_all();
    busy_file(0);
    return 0;
}
