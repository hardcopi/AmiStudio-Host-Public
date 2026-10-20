/* ash-cli - test client for the AmiStudio Host (what the Amiga side will do), built on client/ashc.c.
 *   ash-cli [-H host] [-p port] [-t token] info
 *   ash-cli ... pair CODE [NAME]              prints the token
 *   ash-cli ... fetch URL OUT [SIZE [SHA256]] download through the Host's fetch service
 *   ash-cli ... discover [ADDR]               LAN discovery (default broadcast)
 *   ash-cli ... hs SERVICE VER                handshake only: prints status and message
 *   ash-cli ... amidrive status|signin|connect|flush|unmount [--force|--later]|signout   (tools/amidrive_cli.c) */
#include "../client/ashc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int amidrive_cli(const char *host, int port, const char *token, int argc, char **argv);
static void disc_cb(const char *a, const char *t, void *ud) { (void)ud; printf("host %s\n%s\n", a, t); }
int main(int argc, char **argv) {
    const char *host = "127.0.0.1", *tok = getenv("ASH_TOKEN"); int port = ASH_PORT, i = 1; char msg[300];
    ashc_init();
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-H") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) tok = argv[++i];
    }
    if (i >= argc) { fprintf(stderr, "usage: ash-cli [-H host] [-p port] [-t token] info|pair CODE [NAME]|fetch URL OUT [SIZE [SHA]]|discover [ADDR]|hs SERVICE VER|amidrive CMD\n"); return 2; }
    const char *cmd = argv[i++];
    if (!strcmp(cmd, "amidrive")) return amidrive_cli(host, port, tok, argc - i, argv + i);
    if (!strcmp(cmd, "info")) { ashc_set_timeout(5000); char out[4096]; if (ashc_hostinfo(host, port, tok, out, sizeof out, msg, sizeof msg)) { printf("ERR %s\n", msg); return 1; } fputs(out, stdout); return 0; }
    if (!strcmp(cmd, "pair") && i < argc) { char t[ASH_TOKEN_HEX + 1]; if (ashc_pair(host, port, argv[i], i + 1 < argc ? argv[i + 1] : "ash-cli", t, msg, sizeof msg)) { printf("ERR %s\n", msg); return 1; } printf("%s\n", t); return 0; }
    if (!strcmp(cmd, "discover")) { int n = ashc_discover(i < argc ? argv[i] : NULL, port, 1500, disc_cb, NULL); printf("%d host(s)\n", n); return n > 0 ? 0 : 1; }
    if (!strcmp(cmd, "hs") && i + 1 < argc) { int st; unsigned v = 0; long s = ashc_open(host, port, argv[i], (unsigned)atoi(argv[i + 1]), tok, "ash-cli", &st, &v, msg, sizeof msg);
        printf("status %d ver %u msg %s\n", st, v, msg); ashc_close(s); return st; }
    if (!strcmp(cmd, "fetch") && i + 1 < argc) {
        int st; long s = ashc_open(host, port, "fetch", 1, tok, "ash-cli", &st, NULL, msg, sizeof msg);
        if (s < 0) { printf("ERR handshake %d %s\n", st, msg); return 1; }
        char req[2400]; int n = snprintf(req, sizeof req, "%s%s%s%s%s", argv[i], i + 2 < argc ? "\n" : "", i + 2 < argc ? argv[i + 2] : "", i + 3 < argc ? "\n" : "", i + 3 < argc ? argv[i + 3] : "");
        ashc_send(s, FETCH_GET, req, (unsigned long)n);
        FILE *f = fopen(argv[i + 1], "wb"); static unsigned char b[FETCH_CHUNK + 16]; long total = 0; int op;
        for (;;) { long k = ashc_recv(s, &op, b, sizeof b - 1); if (k < 0) { printf("ERR connection %ld\n", k); return 1; }
            if (op == FETCH_HEAD) printf("HEAD size %lu http %u\n", (unsigned long)(b[0] | b[1] << 8 | b[2] << 16 | (unsigned long)b[3] << 24), b[4] | b[5] << 8);
            else if (op == FETCH_DATA) { fwrite(b, 1, (size_t)k, f); total += k; }
            else if (op == FETCH_END) { b[64] = b[64]; printf("END sha256 %.64s bytes %ld\n", (char *)b, total); break; }
            else if (op == FETCH_ERR) { b[k] = 0; printf("ERR %s\n", (char *)b); fclose(f); ashc_close(s); return 1; } }
        fclose(f); ashc_close(s); return 0;
    }
    fprintf(stderr, "unknown command %s\n", cmd); return 2;
}
