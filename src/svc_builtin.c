/* AmiStudio Host - built-in services: hostinfo (status for the launcher) and pair (6-digit code -> token). */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- hostinfo v1: one INFO frame of "key value" lines, then the host closes ---- */
static void hi_status(service *sv, char *out, int n) { (void)sv; snprintf(out, n, "up"); }
static void hi_run(service *sv, conn *c) {
    (void)sv; char b[4096]; int o = 0; char up[24] = "";
    plat_lock(g_lock); snprintf(up, sizeof up, "%s", g_cfg.update_ver); plat_unlock(g_lock);
    o += snprintf(b + o, sizeof b - o, "host %s\nash %d\nname %s\nid %s\nos %s\nport %d\nlan %d\npaired %d\nuptime %lld\n",
        HOST_VERSION, ASH_VERSION, g_cfg.name, g_cfg.id, plat_os_name(), g_cfg.port, g_cfg.lan, c->authed, (long long)((plat_now_ms() - g_started_ms) / 1000));
    double l = plat_load(); if (l >= 0) o += snprintf(b + o, sizeof b - o, "load %.2f\n", l);
    if (up[0]) o += snprintf(b + o, sizeof b - o, "update host %s\n", up);
    for (int i = 0; i < g_nsvcs && o < (int)sizeof b - 200; i++) { char st[120] = "up"; if (g_svcs[i]->status) g_svcs[i]->status(g_svcs[i], st, sizeof st);
        o += snprintf(b + o, sizeof b - o, "service %s %u %s\n", g_svcs[i]->name, g_svcs[i]->vmax, st); }
    frame_send(c->s, HI_INFO, b, (uint32_t)o);
}
service *svc_hostinfo_new(void) {
    service *sv = calloc(1, sizeof *sv); snprintf(sv->name, sizeof sv->name, "hostinfo");
    sv->vmin = sv->vmax = 1; sv->need_auth = 0; sv->run = hi_run; sv->status = hi_status; return sv;
}

/* ---- pair v1: PAIR_REQ "123456[\nclient name[\nclient id]]" -> PAIR_OK "<token>" | PAIR_FAIL "<why>" ---- */
static void pr_run(service *sv, conn *c) {
    (void)sv; int op; unsigned char *p; uint32_t n; char code[8] = "", name[ASH_MAX_CLIENT + 1] = "", cid[65] = "", tok[ASH_TOKEN_HEX + 1], msg[200];
    plat_set_timeout(c->s, 120000);
    if (frame_recv(c->s, &op, &p, &n, 512) || op != PAIR_REQ) { free(p); return; }
    int k = 0; uint32_t j = 0;
    for (; j < n && p[j] != '\n'; j++) if (p[j] >= '0' && p[j] <= '9' && k < 7) code[k++] = (char)p[j];
    code[k] = 0;
    if (j < n) { uint32_t a = j + 1, e = a; while (e < n && p[e] != '\n') e++;
        snprintf(name, sizeof name, "%.*s", (int)(e - a), (char *)p + a);
        if (e < n) snprintf(cid, sizeof cid, "%.*s", (int)(n - e - 1), (char *)p + e + 1); }
    if (!name[0]) snprintf(name, sizeof name, "%s", c->client[0] ? c->client : "Amiga");
    snprintf(name + strlen(name), sizeof name - strlen(name), " @%s", c->peer);
    free(p);
    if (auth_pair(code, name, cid, tok, msg, sizeof msg)) {
        auth_fail(c->peer); LOG("%s: pairing failed: %s", c->peer, msg);
        frame_send(c->s, PAIR_FAIL, msg, (uint32_t)strlen(msg)); return;
    }
    LOG("%s: paired (%s) - %s", c->peer, name, msg);
    frame_send(c->s, PAIR_OK, tok, ASH_TOKEN_HEX);
}
/* ---- control v1 (0.1.6): quit-when-unused for the launcher (docs/PROTOCOL.md "control") ----
   QUIT_WHEN_UNUSED [u16 grace s] / QUIT_CANCEL / QUIT_STATUS -> QUIT_STATE u8 state, u16 other sessions, u16 grace s */
static void ctl_reply(conn *c, int st, int others, int grace) {
    unsigned char b[5]; b[0] = (unsigned char)st; put_u16(b + 1, (unsigned)(others > 65535 ? 65535 : others)); put_u16(b + 3, (unsigned)grace);
    frame_send(c->s, CTL_QUIT_STATE, b, 5);
}
static void ctl_run(service *sv, conn *c) {
    (void)sv; plat_set_timeout(c->s, 600000);
    for (;;) {
        int op; unsigned char *p = NULL; uint32_t n;
        if (frame_recv(c->s, &op, &p, &n, 64)) { free(p); return; }
        int others = 0, grace = 0, st;
        if (op == CTL_QUIT_WHEN_UNUSED) {
            int g = n >= 2 ? (int)get_u16(p) : 0;
            st = host_quit_arm(g, &others); host_quit_state(NULL, &grace);
            LOG("%s (%s): quit-when-unused: %s", c->peer, c->client[0] ? c->client : "?",
                st == CTL_STOPPING ? "no other client, stopping now" : st == CTL_ARMED ? "armed" : "refused (allow_quit no / --no-remote-quit)");
            if (st == CTL_ARMED) LOG("  %d other client session%s; the Host stops %d s after the last one ends", others, others == 1 ? "" : "s", grace);
        } else if (op == CTL_QUIT_CANCEL) {
            host_quit_cancel(); st = host_quit_state(&others, &grace);
            LOG("%s (%s): quit-when-unused cancelled%s", c->peer, c->client[0] ? c->client : "?", st == CTL_STOPPING ? " too late (already stopping)" : "");
        } else if (op == CTL_QUIT_STATUS) st = host_quit_state(&others, &grace);
        else { free(p); continue; }   /* unknown ops are ignored (newer clients) */
        free(p);
        ctl_reply(c, st, others, grace);
    }
}
service *svc_control_new(void) {
    service *sv = calloc(1, sizeof *sv); snprintf(sv->name, sizeof sv->name, "control");
    sv->vmin = sv->vmax = 1; sv->need_auth = 1; sv->run = ctl_run; sv->status = hi_status; return sv;
}
service *svc_pair_new(void) {
    service *sv = calloc(1, sizeof *sv); snprintf(sv->name, sizeof sv->name, "pair");
    sv->vmin = sv->vmax = 1; sv->need_auth = 0; sv->run = pr_run; sv->status = hi_status; return sv;
}
