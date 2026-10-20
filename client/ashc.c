/* ashc - ASH v1 client (see ashc.h). */
#include "ashc.h"
#include <stdio.h>
#include <string.h>
#ifdef ASHC_AMIGA
#include <proto/socket.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define CLOSESOCK(s) CloseSocket(s)
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#define CLOSESOCK(s) close(s)
#endif

int ashc_init(void) {
#ifdef _WIN32
    WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) ? -1 : 0;
#else
    return 0;
#endif
}
static int g_tmo;   /* ashc_set_timeout: connect/read/write timeout in ms, 0 = none (Amiga builds: ignored) */
void ashc_set_timeout(int ms) { g_tmo = ms > 0 ? ms : 0; }
#if !defined(ASHC_AMIGA) && !defined(_WIN32)
static int tmo_connect(long s, const struct sockaddr *sa, int sl) {
    if (!g_tmo) return connect(s, sa, sl);
    int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, sa, sl);
    if (r && errno == EINPROGRESS) {
        fd_set w; struct timeval tv = { g_tmo / 1000, (g_tmo % 1000) * 1000 }; FD_ZERO(&w); FD_SET(s, &w);
        if (select((int)s + 1, NULL, &w, NULL, &tv) == 1) { int e = 0; socklen_t el = sizeof e; getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el); r = e ? -1 : 0; if (e) errno = e; }
        else { r = -1; errno = ETIMEDOUT; }
    }
    fcntl(s, F_SETFL, fl);
    if (!r) { struct timeval tv = { g_tmo / 1000, (g_tmo % 1000) * 1000 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); }
    return r;
}
#define TIMED_OUT() (errno == EAGAIN || errno == EWOULDBLOCK)
#elif defined(_WIN32)
static int tmo_connect(long s, const struct sockaddr *sa, int sl) {
    int r = connect(s, sa, sl);
    if (!r && g_tmo) { DWORD t = (DWORD)g_tmo; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t); }
    return r;
}
#define TIMED_OUT() (WSAGetLastError() == WSAETIMEDOUT)
#else
#define tmo_connect(s, sa, sl) connect(s, sa, sl)
#define TIMED_OUT() 0
#endif
static int wr(long s, const void *b, unsigned long n) { const char *p = b; while (n) { long k = send(s, p, n, 0); if (k <= 0) return -1; p += k; n -= (unsigned long)k; } return 0; }
static int rd(long s, void *b, unsigned long n) { char *p = b; while (n) { long k = recv(s, p, n, 0); if (k < 0) return -1; if (!k) return -2; p += k; n -= (unsigned long)k; } return 0; }
#if defined(ASH_HOSTD)
int plat_resolve4(const char *host, struct in_addr *out);   /* inside amistudio-hostd: plat_posix.c */
#endif
static void u16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

long ashc_open(const char *host, int port, const char *service, unsigned ver, const char *token, const char *client,
               int *status, unsigned *ver_out, char *msg, int mn) {
    struct sockaddr_in sa; struct hostent *he; long s; unsigned char b[512]; int o = 0; size_t l;
    *status = -1; msg[0] = 0;
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port);
#if defined(ASH_HOSTD)
    /* inside amistudio-hostd (static glibc): its NSS-free lookup (plat_posix.c), never gethostbyname */
    (void)he; if (plat_resolve4(host, &sa.sin_addr)) { snprintf(msg, mn, "unknown host %s", host); return -1; }
#else
    he = gethostbyname(host);
    if (!he) { snprintf(msg, mn, "unknown host %s", host); return -1; }
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
#endif
    s = (long)socket(AF_INET, SOCK_STREAM, 0); if (s < 0) { snprintf(msg, mn, "no socket"); return -1; }
    if (tmo_connect(s, (struct sockaddr *)&sa, sizeof sa)) { snprintf(msg, mn, "cannot connect to %s:%d (nothing listening, or no answer%s)", host, port, g_tmo ? " in time" : ""); CLOSESOCK(s); return -1; }
#if !defined(ASHC_AMIGA) && !defined(_WIN32)
    { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
#endif
    memcpy(b, ASH_MAGIC, 4); u16(b + 4, ASH_VERSION); o = 6;
    l = strlen(service); if (l > ASH_MAX_NAME) l = ASH_MAX_NAME; b[o++] = (unsigned char)l; memcpy(b + o, service, l); o += (int)l;
    u16(b + o, ver); o += 2;
    l = token ? strlen(token) : 0; if (l > ASH_MAX_TOKEN) l = ASH_MAX_TOKEN; b[o++] = (unsigned char)l; if (l) memcpy(b + o, token, l); o += (int)l;
    l = client ? strlen(client) : 0; if (l > ASH_MAX_CLIENT) l = ASH_MAX_CLIENT; b[o++] = (unsigned char)l; if (l) memcpy(b + o, client, l); o += (int)l;
    { int r = wr(s, b, (unsigned long)o); if (!r) r = rd(s, b, 10);
      if (r == -1 && TIMED_OUT()) { snprintf(msg, mn, "%s:%d is in use by a program that is not an AmiStudio Host (no ASH answer within %d.%d s)", host, port, g_tmo / 1000, g_tmo % 1000 / 100); CLOSESOCK(s); return -1; }
      if (r || memcmp(b, ASH_MAGIC, 4)) { snprintf(msg, mn, "%s:%d is in use by a program that is not an AmiStudio Host (%s)", host, port, r == -2 ? "it closed the connection" : r ? "connection error" : "it answered something else"); CLOSESOCK(s); return -1; } }
    *status = b[6]; if (ver_out) *ver_out = b[7] | (unsigned)b[8] << 8;
    l = b[9]; { char m[256]; if (l && rd(s, m, (unsigned long)l)) { CLOSESOCK(s); return -1; } m[l] = 0; snprintf(msg, mn, "%s", m); }
    if (*status != ASH_OK) { CLOSESOCK(s); return -1; }
    return s;
}
int ashc_send(long s, int op, const void *p, unsigned long n) {
    unsigned char h[5]; unsigned long t = n + 1; h[0] = (unsigned char)t; h[1] = (unsigned char)(t >> 8); h[2] = (unsigned char)(t >> 16); h[3] = (unsigned char)(t >> 24); h[4] = (unsigned char)op;
    if (wr(s, h, 5)) return -1;
    return n ? wr(s, p, n) : 0;
}
long ashc_recv(long s, int *op, void *buf, unsigned long cap) {
    unsigned char h[5]; unsigned long n; int r = rd(s, h, 5); if (r) return r;
    n = (h[0] | (unsigned long)h[1] << 8 | (unsigned long)h[2] << 16 | (unsigned long)h[3] << 24);
    if (n < 1) return -1;
    n--; *op = h[4];
    if (n > cap) return -3;
    if (n && (r = rd(s, buf, n))) return r;
    return (long)n;
}
void ashc_close(long s) { if (s >= 0) CLOSESOCK(s); }

int ashc_hostinfo(const char *host, int port, const char *token, char *out, int on, char *msg, int mn) {
    int st, op; long n; long s = ashc_open(host, port, "hostinfo", 1, token, "ashc", &st, NULL, msg, mn);
    if (s < 0) return -1;
    n = ashc_recv(s, &op, out, (unsigned long)on - 1); ashc_close(s);
    if (n < 0 || op != HI_INFO) { snprintf(msg, mn, "no status from the Host%s", n == -1 && TIMED_OUT() ? " (timed out)" : ""); return -1; }
    out[n] = 0; return 0;
}
int ashc_pair(const char *host, int port, const char *code, const char *name, char *tok, char *msg, int mn) {
    int st, op; char b[300]; long n; long s = ashc_open(host, port, "pair", 1, NULL, name, &st, NULL, msg, mn);
    if (s < 0) return -1;
    n = snprintf(b, sizeof b, "%s\n%s", code, name ? name : "");
    if (ashc_send(s, PAIR_REQ, b, (unsigned long)n)) { ashc_close(s); snprintf(msg, mn, "send failed"); return -1; }
    n = ashc_recv(s, &op, b, sizeof b - 1); ashc_close(s);
    if (n < 0) { snprintf(msg, mn, "no answer from the Host"); return -1; }
    b[n] = 0;
    if (op == PAIR_OK && n == ASH_TOKEN_HEX) { memcpy(tok, b, ASH_TOKEN_HEX); tok[ASH_TOKEN_HEX] = 0; snprintf(msg, mn, "paired"); return 0; }
    snprintf(msg, mn, "%s", b); return -1;
}
int ashc_discover(const char *addr, int port, int wait_ms, void (*cb)(const char *addr, const char *text, void *ud), void *ud) {
    struct sockaddr_in sa, from; long s; int one = 1, n = 0; char b[600];
    s = (long)socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port); sa.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (addr && *addr) sa.sin_addr.s_addr = inet_addr(addr);
    sendto(s, ASH_DISC_Q, 4, 0, (struct sockaddr *)&sa, sizeof sa);
    for (;;) {
        fd_set rf; struct timeval tv; tv.tv_sec = wait_ms / 1000; tv.tv_usec = (wait_ms % 1000) * 1000;
        FD_ZERO(&rf); FD_SET(s, &rf);
        if (select((int)s + 1, &rf, NULL, NULL, &tv) <= 0) break;
        socklen_t fl = sizeof from; long k = recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
        if (k < 4 || memcmp(b, ASH_DISC_A, 4)) continue;
        b[k] = 0; cb(inet_ntoa(from.sin_addr), b + 4, ud); n++;
    }
    CLOSESOCK(s); return n;
}
