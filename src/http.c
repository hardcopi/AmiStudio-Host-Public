/* AmiStudio Host - minimal HTTPS/1.1 GET client (OpenSSL): certificate + host name check, SNI, redirects
 * (each hop must pass the allow() check), Content-Length / chunked / until-close bodies streamed to a callback. */
#include "host.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static int has_word(const char *s, const char *w) { size_t n = strlen(w); for (; *s; s++) if (!strncasecmp(s, w, n)) return 1; return 0; }
static SSL_CTX *g_ctx; static plat_mutex *g_hm;
static SSL_CTX *ctx(char *err, int en) {
    if (!g_hm) g_hm = plat_mutex_new();
    plat_lock(g_hm);
    if (!g_ctx) {
        SSL_CTX *c = SSL_CTX_new(TLS_client_method());
        if (c) {
            SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
            SSL_CTX_set_verify(c, SSL_VERIFY_PEER, NULL);
            SSL_CTX_set_default_verify_paths(c);
            /* the static Linux build carries Debian's OPENSSLDIR; also load the CA bundles other distros use */
            static const char *const bundles[] = { "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", NULL };
#ifndef _WIN32
            for (int i = 0; bundles[i]; i++) { FILE *t = fopen(bundles[i], "r"); if (t) { fclose(t); SSL_CTX_load_verify_locations(c, bundles[i], NULL); } }
#else
            (void)bundles;   /* Windows: TODO load the system store (CryptoAPI) - docs/PORTING-WINDOWS.md */
#endif
            if (g_cfg.cafile[0] && !SSL_CTX_load_verify_locations(c, g_cfg.cafile, NULL)) LOG("cafile %s could not be loaded", g_cfg.cafile);
        }
        g_ctx = c;
    }
    plat_unlock(g_hm);
    if (!g_ctx) snprintf(err, en, "TLS init failed");
    return g_ctx;
}

typedef struct { SSL *ssl; sock_t s; unsigned char buf[16384]; int pos, len; } hconn;
static int hc_fill(hconn *h) { h->pos = 0; h->len = SSL_read(h->ssl, h->buf, sizeof h->buf); if (h->len <= 0) { h->len = 0; return -1; } return 0; }
static int hc_getc(hconn *h) { if (h->pos >= h->len && hc_fill(h)) return -1; return h->buf[h->pos++]; }
static int hc_line(hconn *h, char *out, int n) {
    int k = 0, c; while ((c = hc_getc(h)) >= 0) { if (c == '\n') break; if (c != '\r' && k < n - 1) out[k++] = (char)c; } out[k] = 0;
    return (c < 0 && !k) ? -1 : k;
}
static int parse_url(const char *url, char *host, int hn, int *port, char *path, int pn) {
    if (strncasecmp(url, "https://", 8)) return -1;
    const char *p = url + 8, *e = p; while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#') e++;
    if (e == p || e - p >= hn) return -1;
    memcpy(host, p, (size_t)(e - p)); host[e - p] = 0; *port = 443;
    if (*e == ':') { *port = atoi(e + 1); while (*e && *e != '/' && *e != '?') e++; if (*port <= 0 || *port > 65535) return -1; }
    snprintf(path, pn, "%s%s", *e == '/' ? "" : "/", e);
    char *h = strchr(path, '#'); if (h) *h = 0;
    for (char *q = host; *q; q++) { *q = (char)tolower((unsigned char)*q); if (!isalnum((unsigned char)*q) && *q != '.' && *q != '-') return -1; }
    for (const char *q = path; *q; q++) if ((unsigned char)*q <= ' ') return -1;    /* no header injection */
    return 0;
}

/* 0.1.10: a shorter connect + read timeout for this thread's next requests (0 = the defaults, 15 s / 30 s) */
static __thread int t_tmo;
void http_set_timeout(int ms) { t_tmo = ms > 0 ? ms : 0; }
int http_get(const char *url0, int (*allow)(const char *host), http_head_cb hc, http_body_cb bc, void *ud, char *err, int en) {
    return http_req("GET", url0, NULL, NULL, allow, hc, bc, ud, err, en);
}
int http_req(const char *method, const char *url0, const char *headers, const char *body, int (*allow)(const char *host),
             http_head_cb hc, http_body_cb bc, void *ud, char *err, int en) {
    char url[2048], host[256], path[1800]; int port, hops;
    snprintf(url, sizeof url, "%s", url0);
    SSL_CTX *c = ctx(err, en); if (!c) return -1;
    for (hops = 0; hops < 6; hops++) {
        if (parse_url(url, host, sizeof host, &port, path, sizeof path)) { snprintf(err, en, "only https:// URLs can be fetched (%.80s)", url); return -1; }
        if (allow && !allow(host)) { snprintf(err, en, "%s is not on this Host's fetch list (fetch_allow in host.conf)", host); return -1; }
        hconn h; memset(&h, 0, sizeof h);
        h.s = plat_connect_tcp(host, port, t_tmo ? t_tmo : 15000);
        if (h.s == SOCK_BAD) { snprintf(err, en, "cannot connect to %s:%d", host, port); return -1; }
        plat_set_timeout(h.s, t_tmo ? t_tmo : 30000);
        h.ssl = SSL_new(c); SSL_set_fd(h.ssl, (int)h.s); SSL_set_tlsext_host_name(h.ssl, host); SSL_set1_host(h.ssl, host);
        int status = -1; long long clen = -1; int chunked = 0; char loc[2048] = "", line[4096];
        if (SSL_connect(h.ssl) != 1) {
            long v = SSL_get_verify_result(h.ssl);
            snprintf(err, en, "TLS to %s failed%s%s", host, v != X509_V_OK ? ": " : "", v != X509_V_OK ? X509_verify_cert_error_string(v) : "");
            goto fail;
        }
        { char req[8192]; size_t bl = body ? strlen(body) : 0; char cl[48] = "";
          if (body) snprintf(cl, sizeof cl, "Content-Length: %zu\r\n", bl);
          int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AmiStudio-Host/%s\r\nAccept: */*\r\nAccept-Encoding: identity\r\n%s%sConnection: close\r\n\r\n",
                           method, path, host, HOST_VERSION, headers ? headers : "", cl);
          if (n <= 0 || n >= (int)sizeof req) { snprintf(err, en, "request too long"); goto fail; }
          if (SSL_write(h.ssl, req, n) != n || (bl && SSL_write(h.ssl, body, (int)bl) != (int)bl)) { snprintf(err, en, "send to %s failed", host); goto fail; } }
        if (hc_line(&h, line, sizeof line) < 0 || sscanf(line, "HTTP/%*s %d", &status) != 1) { snprintf(err, en, "no HTTP reply from %s", host); goto fail; }
        while (hc_line(&h, line, sizeof line) > 0) {
            if (!strncasecmp(line, "content-length:", 15)) clen = atoll(line + 15);
            else if (!strncasecmp(line, "transfer-encoding:", 18) && has_word(line + 18, "chunked")) chunked = 1;
            else if (!strncasecmp(line, "location:", 9)) { char *v = line + 9; while (*v == ' ') v++; snprintf(loc, sizeof loc, "%s", v); }
        }
        if (status >= 300 && status < 400 && status != 304 && loc[0] && !body) {
            if (loc[0] == '/' && port != 443) snprintf(url, sizeof url, "https://%s:%d%.1800s", host, port, loc);
            else if (loc[0] == '/') snprintf(url, sizeof url, "https://%s%.1800s", host, loc);
            else snprintf(url, sizeof url, "%s", loc);
            SSL_free(h.ssl); plat_close(h.s); continue;
        }
        if (hc && hc(ud, status, chunked ? -1 : clen)) { snprintf(err, en, "aborted"); goto fail; }
        /* body */
        if (chunked) {
            for (;;) {
                if (hc_line(&h, line, sizeof line) < 0) { snprintf(err, en, "connection lost"); goto fail; }
                long long cs = strtoll(line, NULL, 16); if (cs <= 0) break;
                while (cs > 0) { if (h.pos >= h.len && hc_fill(&h)) { snprintf(err, en, "connection lost"); goto fail; }
                    int k = h.len - h.pos; if (k > cs) k = (int)cs;
                    if (bc && bc(ud, h.buf + h.pos, (size_t)k)) { snprintf(err, en, "aborted"); goto fail; }
                    h.pos += k; cs -= k; }
                hc_line(&h, line, sizeof line);
            }
        } else {
            long long left = clen;
            while (left != 0) {
                if (h.pos >= h.len && hc_fill(&h)) { if (clen < 0) break; snprintf(err, en, "connection lost after %lld of %lld bytes", clen - left, clen); goto fail; }
                int k = h.len - h.pos; if (left > 0 && k > left) k = (int)left;
                if (bc && bc(ud, h.buf + h.pos, (size_t)k)) { snprintf(err, en, "aborted"); goto fail; }
                h.pos += k; if (left > 0) left -= k;
            }
        }
        SSL_shutdown(h.ssl); SSL_free(h.ssl); plat_close(h.s);
        return status;
    fail:
        SSL_free(h.ssl); plat_close(h.s);
        return -1;
    }
    snprintf(err, en, "too many redirects");
    return -1;
}

int fetch_host_allowed(const char *host) {
    const char *p = g_cfg.fetch_allow; size_t hl = strlen(host);
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p; while (*e && *e != ' ' && *e != ',') e++;
        size_t n = (size_t)(e - p);
        if (n == 1 && *p == '*') return 1;
        if (n && n == hl && !strncasecmp(p, host, n)) return 1;
        if (n > 1 && *p == '.' && hl > n && !strncasecmp(host + hl - n, p, n)) return 1;   /* ".example.com" = any subdomain */
        p = e;
    }
    return 0;
}
