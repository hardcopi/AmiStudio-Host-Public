/* AmiStudio Host - helpers. */
#include "util.h"
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int g_verbose;
static plat_mutex *g_logm;
void logf_(const char *fmt, ...) {
    char t[32]; time_t now = time(NULL); struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S", &tm);
    if (!g_logm) g_logm = plat_mutex_new();
    va_list ap; va_start(ap, fmt);
    plat_lock(g_logm);
    fprintf(stderr, "%s amistudio-hostd: ", t); vfprintf(stderr, fmt, ap); fputc('\n', stderr); fflush(stderr);
    plat_unlock(g_logm);
    va_end(ap);
}
void put_u16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
void put_u32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
unsigned get_u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
uint32_t get_u32(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
int frame_send(sock_t s, int op, const void *p, uint32_t n) {
    unsigned char h[5]; put_u32(h, n + 1); h[4] = (unsigned char)op;
    if (n <= 4096) { unsigned char b[4096 + 5]; memcpy(b, h, 5); if (n) memcpy(b + 5, p, n); return plat_send_all(s, b, n + 5); }
    if (plat_send_all(s, h, 5)) return -1;
    return plat_send_all(s, p, n);
}
int frame_recv(sock_t s, int *op, unsigned char **buf, uint32_t *n, uint32_t max) {
    unsigned char h[5]; int r = plat_recv_all(s, h, 5); *buf = NULL; *n = 0;
    if (r) return r;
    uint32_t len = get_u32(h);
    if (len < 1 || len - 1 > max) return -1;
    *op = h[4]; *n = len - 1;
    *buf = malloc(*n + 1); if (!*buf) return -1;
    if (*n && (r = plat_recv_all(s, *buf, *n))) { free(*buf); *buf = NULL; return r; }
    (*buf)[*n] = 0;
    return 0;
}
void hex(const unsigned char *b, int n, char *out) { static const char d[] = "0123456789abcdef"; for (int i = 0; i < n; i++) { out[2*i] = d[b[i] >> 4]; out[2*i+1] = d[b[i] & 15]; } out[2*n] = 0; }
int ct_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b); unsigned char d = la != lb;
    for (size_t i = 0; i < la && i < lb; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}
void sha256_hex(const void *b, size_t n, char out[65]) { unsigned char d[32]; unsigned dl = 32; EVP_Digest(b, n, d, &dl, EVP_sha256(), NULL); hex(d, 32, out); }
void trim(char *s) { size_t l = strlen(s); while (l && (s[l-1] == '\n' || s[l-1] == '\r' || s[l-1] == ' ' || s[l-1] == '\t')) s[--l] = 0;
    size_t i = 0; while (s[i] == ' ' || s[i] == '\t') i++; if (i) memmove(s, s + i, strlen(s + i) + 1); }
