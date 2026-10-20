/* `amistudio-hostd status [HOST[:PORT]]` - asks a running Host for its hostinfo (uses client/ashc.c). */
#include "host.h"
#include "../client/ashc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int ash_status_main(const char *hp) {
    char host[256] = "127.0.0.1", out[4096], msg[256]; int port = ASH_PORT;
    if (hp) { snprintf(host, sizeof host, "%s", hp); char *c = strrchr(host, ':'); if (c) { *c = 0; port = atoi(c + 1); } }
    const char *tok = getenv("ASH_TOKEN"), *t = getenv("ASH_TIMEOUT_MS");
    ashc_set_timeout(t && atoi(t) > 0 ? atoi(t) : 2000);   /* never hang on a port held by something else */
    if (ashc_hostinfo(host, port, tok, out, sizeof out, msg, sizeof msg)) { fprintf(stderr, "No AmiStudio Host at %s:%d (%s)\n", host, port, msg); return 1; }
    fputs(out, stdout);
    return 0;
}
