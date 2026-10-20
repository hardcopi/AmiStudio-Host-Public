/* amidrive command line (Host 0.1.8), shared by `amistudio-hostd amidrive ...` and `ash-cli amidrive ...`:
 *   status [--watch]   signin [--url|--amisurf]   connect [--auto]   flush   unmount [--force|--later]   signout [--revoke]
 * Talks to the amidrive service over ASH like AmiDrive does (client/ashc.c). Exit status: the RESULT status (0 ok,
 * 1 files open, 2 failed, 3 not signed in, 4 busy, 5 pending uploads, 6 sign-in expired, 7 not empty, 8 drawer
 * copied), 9 = could not talk to the Host, 10 = usage. */
#include "../client/ashc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char *st_names[] = { "unavailable", "signed-out", "signing-in", "ready", "mounting", "mounted", "flushing", "unmounting", "error" };
static const char *fl_names[] = { "fuse", "rclone", "pending", "lowdisk", "expired", "notempty", "copied" };
static const char *rs_names[] = { "ok", "files-open", "failed", "not-signed-in", "busy", "pending", "expired", "not-empty", "copied" };
static uint32_t g32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned long long g64(const unsigned char *p) { return (unsigned long long)g32(p) | (unsigned long long)g32(p + 4) << 32; }
static int gstr(const unsigned char *b, long n, long *o, char *out, int on) {   /* u8 length + bytes */
    if (*o >= n) { out[0] = 0; return -1; }
    int l = b[(*o)++]; if (*o + l > n) l = (int)(n - *o);
    int c = l < on - 1 ? l : on - 1; memcpy(out, b + *o, (size_t)c); out[c] = 0; *o += l; return 0;
}
static void print_state(const unsigned char *b, long n) {
    if (n < 2 + 5 * 4 + 16) { printf("state ? (short frame)\n"); return; }
    long o = 0; int st = b[o++], fl = b[o++];
    uint32_t q = g32(b + o), ip = g32(b + o + 4), kb = g32(b + o + 8), of = g32(b + o + 12), er = g32(b + o + 16); o += 20;
    unsigned long long used = g64(b + o), total = g64(b + o + 8); o += 16;
    char acc[256], mp[256], nonce[256], msg[256];
    gstr(b, n, &o, acc, sizeof acc); gstr(b, n, &o, mp, sizeof mp); gstr(b, n, &o, nonce, sizeof nonce); gstr(b, n, &o, msg, sizeof msg);
    printf("state %s\nflags", st <= 8 ? st_names[st] : "?");
    int any = 0; for (int i = 0; i < 7; i++) if (fl & (1 << i)) { printf("%s%s", any ? "," : " ", fl_names[i]); any = 1; }
    printf("%s\nuploads queued %u in-progress %u pending-kb %u open-files %u errored %u\n", any ? "" : " -", q, ip, kb, of, er);
    if (total || used) printf("quota %llu/%llu\n", used, total);
    printf("account %s\nmount %s\n", acc[0] ? acc : "-", mp);
    if (nonce[0]) printf("nonce %s\n", nonce);
    if (msg[0]) printf("message %s\n", msg);
    fflush(stdout);
}
static int print_result(const unsigned char *b, long n) {
    if (n < 2) return 2;
    long o = 2; char msg[256]; gstr(b, n, &o, msg, sizeof msg); int st = b[1];
    printf("result %s%s%s\n", st <= 8 ? rs_names[st] : "?", msg[0] ? ": " : "", msg);
    if (st == 1 && o + 2 <= n) { int k = b[o] | b[o + 1] << 8; o += 2; for (int i = 0; i < k; i++) { char p[256]; gstr(b, n, &o, p, sizeof p); printf("open %s\n", p); } }
    fflush(stdout); return st;
}
int amidrive_cli(const char *host, int port, const char *token, int argc, char **argv) {
    if (argc < 1) goto usage;
    const char *cmd = argv[0]; int op = 0, arg = 0, watch = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--watch") && !strcmp(cmd, "status")) watch = 1;
        else if (!strcmp(a, "--url") && !strcmp(cmd, "signin")) arg = 1;
        else if (!strcmp(a, "--amisurf") && !strcmp(cmd, "signin")) arg = 2;   /* URL for the Host's browser engine, else the desktop browser */
        else if (!strcmp(a, "--auto") && !strcmp(cmd, "connect")) arg = 1;
        else if (!strcmp(a, "--force") && !strcmp(cmd, "unmount")) arg = 2;
        else if (!strcmp(a, "--later") && !strcmp(cmd, "unmount")) arg = 1;
        else if (!strcmp(a, "--revoke") && !strcmp(cmd, "signout")) arg = 1;
        else { fprintf(stderr, "amidrive %s: unknown option %s\n", cmd, a); goto usage; }
    }
    if (!strcmp(cmd, "status")) op = watch ? 2 : 1;
    else if (!strcmp(cmd, "signin")) op = 3;
    else if (!strcmp(cmd, "connect")) op = 5;
    else if (!strcmp(cmd, "unmount")) op = 6;
    else if (!strcmp(cmd, "flush")) op = 7;
    else if (!strcmp(cmd, "signout")) op = 8;
    else goto usage;
    ashc_init(); ashc_set_timeout(0);
    int st; char msg[300];
    long s = ashc_open(host, port, "amidrive", 1, token, "amidrive-cli", &st, NULL, msg, sizeof msg);
    if (s < 0) { printf("ERR %s (status %d)\n", msg[0] ? msg : "cannot reach the Host", st); return 9; }
    unsigned char a1 = (unsigned char)(op == 2 ? 1 : arg);
    if (ashc_send(s, op, &a1, op == 1 || op == 7 ? 0 : 1)) { printf("ERR send\n"); ashc_close(s); return 9; }
    static unsigned char b[8192]; int rc = -1, auth_done = op != 3;
    for (;;) {
        int fop; long n = ashc_recv(s, &fop, b, sizeof b - 1);
        if (n < 0) { printf("ERR connection closed\n"); rc = 9; break; }
        if (fop == AD_STATE) { print_state(b, n); if (op == 1) { rc = 0; break; } }
        else if (fop == AD_PROGRESS && n >= 8) { printf("progress files %u kb %u\n", g32(b), g32(b + 4)); fflush(stdout); }
        else if (fop == AD_AUTH && n >= 1) {
            long o = 1; char t[1100]; int k = b[0]; if (o < n) { int l = b[o++]; if (l > n - o) l = (int)(n - o); memcpy(t, b + o, (size_t)l); t[l] = 0; } else t[0] = 0;
            if (k == ADA_URL) printf("Open this link in a browser on the Host PC:\n%s\n", t);
            else if (k == ADA_OPENED) printf("%s\n", t[0] ? t : "Sign in in the browser on the Host PC");
            else if (k == ADA_DONE) { printf("signed in\n"); auth_done = 1; rc = 0; }
            else { printf("sign-in failed: %s\n", t); auth_done = 1; rc = 2; }
            fflush(stdout);
            if (auth_done) break;
        }
        else if (fop == AD_RESULT && n >= 2 && b[0] == op) {
            int r = print_result(b, n);
            if (op == 3) { if (r) { rc = r; break; } continue; }
            rc = r; continue;   /* CONNECT/UNMOUNT/FLUSH/SIGNOUT: the STATE that follows is shown too; WATCH keeps going */
        }
        if (rc >= 0 && op >= 5) { if (fop == AD_STATE) break; }
    }
    ashc_close(s);
    return rc < 0 ? 9 : rc;
usage:
    fprintf(stderr, "usage: amidrive status [--watch] | signin [--url|--amisurf] | connect [--auto] | flush | unmount [--force|--later] | signout [--revoke]\n");
    return 10;
}
