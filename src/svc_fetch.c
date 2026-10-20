/* AmiStudio Host - fetch v1: HTTPS download proxy for Amigas without AmiSSL or with a slow 68020 TLS handshake
 * (LAUNCHER-PLAN 3.4, FETCH=HOST). The Host does TLS + certificate checks and streams the body over the LAN;
 * the Amiga still checks size and SHA-256 against the manifest.
 *   client: GET  "url[\nexpected size[\nexpected sha256]]"
 *   host:   HEAD u32le size (0xFFFFFFFF unknown), u16le http status
 *           DATA <= 32 KB chunks ...
 *           END  "<sha256 hex>" u32le bytes      or   ERR "<message>" (also after some DATA)
 * Only hosts in fetch_allow (default: GitHub release + raw hosts) can be fetched, so the Host is not an open proxy. */
#include "host.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { conn *c; EVP_MD_CTX *sha; long long got, want_size; int headed, status; unsigned char buf[FETCH_CHUNK]; int bl; int sockerr; char why[100]; } fx;
static int fx_flush(fx *f) { if (f->bl) { if (frame_send(f->c->s, FETCH_DATA, f->buf, (uint32_t)f->bl)) { f->sockerr = 1; return 1; } f->bl = 0; } return 0; }
static int fx_head(void *ud, int status, long long len) {
    fx *f = ud; unsigned char h[6]; f->status = status;
    if (status != 200) return 0;                                    /* error status: reported after the (small) body */
    if (len > g_cfg.fetch_max) { snprintf(f->why, sizeof f->why, "file is larger than this Host allows (fetch_max_mb)"); return 1; }
    if (f->want_size >= 0 && len >= 0 && len != f->want_size) { snprintf(f->why, sizeof f->why, "size on the server (%lld) does not match the manifest (%lld)", len, f->want_size); return 1; }
    put_u32(h, len < 0 ? 0xFFFFFFFFu : (uint32_t)len); put_u16(h + 4, (unsigned)status);
    if (frame_send(f->c->s, FETCH_HEAD, h, 6)) { f->sockerr = 1; return 1; }
    f->headed = 1; return 0;
}
static int fx_body(void *ud, const unsigned char *b, size_t n) {
    fx *f = ud; if (f->status != 200) return 0;
    if (f->got + (long long)n > g_cfg.fetch_max) { snprintf(f->why, sizeof f->why, "file is larger than this Host allows (fetch_max_mb)"); return 1; }
    if (f->want_size >= 0 && f->got + (long long)n > f->want_size) { snprintf(f->why, sizeof f->why, "file is larger than the manifest says"); return 1; }
    EVP_DigestUpdate(f->sha, b, n); f->got += (long long)n;
    while (n) { size_t k = sizeof f->buf - (size_t)f->bl; if (k > n) k = n; memcpy(f->buf + f->bl, b, k); f->bl += (int)k; b += k; n -= k;
        if (f->bl == (int)sizeof f->buf && fx_flush(f)) return 1; }
    return 0;
}
static void ferr(conn *c, const char *m) { frame_send(c->s, FETCH_ERR, m, (uint32_t)strlen(m)); }
static void fe_run(service *sv, conn *c) {
    (void)sv; int op; unsigned char *p; uint32_t n;
    for (;;) {
        plat_set_timeout(c->s, 300000);
        if (frame_recv(c->s, &op, &p, &n, 4096)) { free(p); return; }
        plat_set_timeout(c->s, 60000);
        if (op != FETCH_GET) { free(p); ferr(c, "unknown request"); continue; }
        char *url = (char *)p, *sz = strchr(url, '\n'), *sh = NULL; long long want = -1; char wsha[65] = "";
        if (sz) { *sz++ = 0; sh = strchr(sz, '\n'); if (sh) *sh++ = 0; if (*sz) want = atoll(sz); if (sh) snprintf(wsha, sizeof wsha, "%s", sh); }
        for (char *q = wsha; *q; q++) if (*q >= 'A' && *q <= 'F') *q += 32;
        fx *f = calloc(1, sizeof *f); f->c = c; f->want_size = want; f->sha = EVP_MD_CTX_new(); EVP_DigestInit_ex(f->sha, EVP_sha256(), NULL);
        char err[300]; int64_t t0 = plat_now_ms();
        int st = http_get(url, fetch_host_allowed, fx_head, fx_body, f, err, sizeof err);
        if (f->sockerr) { LOG("%s: fetch %s: client went away", c->peer, url); EVP_MD_CTX_free(f->sha); free(f); free(p); return; }
        if (st < 0 || st != 200) {
            char m[400];
            if (f->why[0]) snprintf(m, sizeof m, "%s", f->why);
            else if (st < 0) snprintf(m, sizeof m, "%s", err);
            else snprintf(m, sizeof m, "HTTP %d from the server", st);
            LOG("%s: fetch %s failed: %s", c->peer, url, m); ferr(c, m);
        } else if (fx_flush(f)) { EVP_MD_CTX_free(f->sha); free(f); free(p); return; }
        else {
            unsigned char d[32]; char hx[65]; unsigned dl = 32; EVP_DigestFinal_ex(f->sha, d, &dl); hex(d, 32, hx);
            if (want >= 0 && f->got != want) ferr(c, "size does not match the manifest");
            else if (wsha[0] && strcmp(wsha, hx)) ferr(c, "SHA-256 does not match the manifest");
            else { unsigned char e[68]; memcpy(e, hx, 64); put_u32(e + 64, (uint32_t)f->got); frame_send(c->s, FETCH_END, e, 68); }
            LOG("%s: fetched %s (%lld bytes, %lld ms)", c->peer, url, f->got, (long long)(plat_now_ms() - t0));
        }
        EVP_MD_CTX_free(f->sha); free(f); free(p);
    }
}
static void fe_status(service *sv, char *out, int n) { (void)sv; snprintf(out, n, "up"); }
service *svc_fetch_new(void) {
    service *sv = calloc(1, sizeof *sv); snprintf(sv->name, sizeof sv->name, "fetch");
    sv->vmin = sv->vmax = 1; sv->need_auth = 1; sv->run = fe_run; sv->status = fe_status; return sv;
}
