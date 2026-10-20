/* AmiStudio Host - LAN discovery (LAUNCHER-PLAN 2.4): the launcher broadcasts "ASH?" to UDP port 9220 and every
 * Host answers "ASH!" followed by "key=value" lines: name, ver, port, pair (1 = pairing needed for this sender),
 * id, os. Roadshow has no mDNS resolver, so a broadcast is the Amiga path. Replies are rate limited. */
#include "host.h"
#include <stdio.h>
#include <string.h>

static sock_t g_us = SOCK_BAD;
static void disc_thread(void *a) {
    (void)a; unsigned char b[512], addr[64]; char peer[64]; int64_t win = 0; int sent = 0;
    while (!g_stop) {
        if (plat_wait_readable(g_us, 500) <= 0) continue;
        int al = sizeof addr;
        long k = plat_recvfrom(g_us, b, sizeof b, peer, sizeof peer, addr, &al);
        if (k < 4 || memcmp(b, ASH_DISC_Q, 4)) continue;
        int64_t now = plat_now_ms(); if (now - win > 1000) { win = now; sent = 0; }
        if (++sent > 20) continue;                                /* at most 20 replies a second */
        char r[400]; int lo = !strncmp(peer, "127.", 4);
        int n = snprintf(r, sizeof r, "%sname=%s\nver=%s\nport=%d\npair=%d\nid=%s\nos=%s\n", ASH_DISC_A, g_cfg.name, HOST_VERSION, g_cfg.port, lo ? 0 : 1, g_cfg.id, plat_os_name());
        plat_sendto(g_us, r, (size_t)n, addr, al);
        DBG("discovery: answered %s", peer);
    }
}
int discovery_start(void) {
    char err[200];
    g_us = plat_bind_udp("0.0.0.0", g_cfg.port, err, sizeof err);
    if (g_us == SOCK_BAD) { LOG("LAN discovery off: %s", err); return -1; }
    LOG("  LAN discovery on UDP %d", g_cfg.port);
    return plat_thread(disc_thread, NULL);
}
