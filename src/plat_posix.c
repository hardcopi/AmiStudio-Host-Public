/* AmiStudio Host - POSIX platform layer (Linux; should also build on macOS/BSD). */
#define _GNU_SOURCE
#include "plat.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif

int plat_net_init(void) { signal(SIGPIPE, SIG_IGN); return 0; }

static int mkaddr(const char *addr, int port, struct sockaddr_in *sa) {
    memset(sa, 0, sizeof *sa); sa->sin_family = AF_INET; sa->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, addr && *addr ? addr : "127.0.0.1", &sa->sin_addr) == 1 ? 0 : -1;
}
sock_t plat_listen_tcp(const char *addr, int port, char *err, int en) {
    struct sockaddr_in sa; int one = 1;
    if (mkaddr(addr, port, &sa)) { snprintf(err, en, "bad listen address %s", addr); return SOCK_BAD; }
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { snprintf(err, en, "socket: %s", strerror(errno)); return SOCK_BAD; }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) || listen(s, 16)) {
        snprintf(err, en, "%s:%d: %s", addr, port, strerror(errno)); close(s); return SOCK_BAD; }
    return s;
}
sock_t plat_bind_udp(const char *addr, int port, char *err, int en) {
    struct sockaddr_in sa; int one = 1;
    if (mkaddr(addr, port, &sa)) { snprintf(err, en, "bad address %s", addr); return SOCK_BAD; }
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) { snprintf(err, en, "socket: %s", strerror(errno)); return SOCK_BAD; }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa)) { snprintf(err, en, "udp %s:%d: %s", addr, port, strerror(errno)); close(s); return SOCK_BAD; }
    return s;
}
sock_t plat_accept(sock_t l, char *peer, int pn, int *loopback) {
    struct sockaddr_in sa; socklen_t sl = sizeof sa;
    int c = accept4(l, (struct sockaddr *)&sa, &sl, SOCK_CLOEXEC);
    if (c < 0) return SOCK_BAD;
    inet_ntop(AF_INET, &sa.sin_addr, peer, (socklen_t)pn);
    *loopback = (ntohl(sa.sin_addr.s_addr) >> 24) == 127;
    return c;
}
/* Host 0.1.10: name lookups never load glibc's NSS modules into this process. The release binary is linked
   statically; for a name that is not in /etc/hosts, getaddrinfo dlopen()s the system's NSS modules (Fedora/Nobara:
   libnss_myhostname, libnss_resolve, mdns), and those pull in the system's own libc.so.6. A libc from another glibc
   version than the one the Host was linked with crashes in __libc_early_init (SIGFPE, divide by zero) and takes the
   whole Host with it: on Nobara the first lookup was the amidrive sign-in check (accounts.google.com). So names go
   to the system's own `getent ahosts` (a normal dynamic program: the system's NSS with the system's libc, in its own
   process); numeric addresses are parsed with inet_pton. The static build links none of glibc's NSS lookups
   (Makefile: -Wl,--wrap to src/nss_off.c, test: run_tests.py "static binary: no NSS symbols"). */
#define RES_MAX 8
typedef struct { struct sockaddr_storage a; socklen_t l; } res_addr;
static int res_numeric(const char *h, int port, res_addr *r) {   /* inet_pton only: never NSS */
    memset(r, 0, sizeof *r);
    struct sockaddr_in *a4 = (struct sockaddr_in *)&r->a; struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&r->a;
    if (inet_pton(AF_INET, h, &a4->sin_addr) == 1) { a4->sin_family = AF_INET; a4->sin_port = htons((uint16_t)port); r->l = sizeof *a4; return 1; }
    char t[64]; snprintf(t, sizeof t, "%s", h); if (t[0] == '[') { memmove(t, t + 1, strlen(t)); char *e = strchr(t, ']'); if (e) *e = 0; }
    char *pc = strchr(t, '%'); if (pc) *pc = 0;   /* no zone ids */
    if (inet_pton(AF_INET6, t, &a6->sin6_addr) == 1) { a6->sin6_family = AF_INET6; a6->sin6_port = htons((uint16_t)port); r->l = sizeof *a6; return 1; }
    return 0;
}
static int res_name_ok(const char *h) {
    if (!*h || *h == '-' || *h == '.' || strlen(h) > 253) return 0;
    for (; *h; h++) if (!isalnum((unsigned char)*h) && *h != '-' && *h != '.' && *h != '_') return 0;
    return 1;
}
extern char **environ;
/* -1 = getent could not be run, else the number of addresses (0 = unknown name) */
static int res_getent(const char *host, int port, res_addr *out, int max, int timeout_ms) {
    int p[2]; if (pipe2(p, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at; posix_spawnattr_init(&at);
    sigset_t def; sigemptyset(&def); sigaddset(&def, SIGPIPE); sigaddset(&def, SIGCHLD);
    posix_spawnattr_setsigdefault(&at, &def); posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF);
    char *argv[] = { "getent", "ahosts", (char *)host, NULL };
    /* the system's getent with the system's libraries: no LD_LIBRARY_PATH / LD_PRELOAD an emulator or app bundle
       passed on (Amiberry's host-run runs us in its environment) */
    char *envp[256]; int ne = 0;
    for (char **e = environ; e && *e && ne < 255; e++) if (strncmp(*e, "LD_LIBRARY_PATH=", 16) && strncmp(*e, "LD_PRELOAD=", 11)) envp[ne++] = *e;
    envp[ne] = NULL;
    pid_t pid = -1; int r = posix_spawnp(&pid, "getent", &fa, &at, argv, envp);
    static const char *fixed[] = { "/usr/bin/getent", "/bin/getent", "/usr/sbin/getent", NULL };
    for (int i = 0; r && fixed[i]; i++) r = posix_spawn(&pid, fixed[i], &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at); close(p[1]);
    if (r) { close(p[0]); return -1; }
    char buf[8192]; int o = 0; int64_t end = plat_now_ms() + (timeout_ms > 0 && timeout_ms < 15000 ? timeout_ms : 15000);
    for (;;) {
        int left = (int)(end - plat_now_ms()); if (left <= 0) break;
        struct pollfd pf = { p[0], POLLIN, 0 }; int k = poll(&pf, 1, left);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        ssize_t n = read(p[0], buf + o, sizeof buf - 1 - (size_t)o);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (o += (int)n) >= (int)sizeof buf - 1) break;
    }
    buf[o] = 0; close(p[0]);
    int st = 0; pid_t w;
    do w = waitpid(pid, &st, WNOHANG); while (w < 0 && errno == EINTR);
    if (w == 0) { kill(pid, SIGKILL); while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} }
    /* "ADDRESS STREAM|DGRAM|RAW [name]" lines; keep the STREAM ones, in getent's (RFC 6724) order */
    int n = 0;
    for (char *l = buf, *nl; l && *l && n < max; l = nl) {
        nl = strchr(l, '\n'); if (nl) *nl++ = 0;
        char ip[64], kind[16]; if (sscanf(l, "%63s %15s", ip, kind) != 2 || strcmp(kind, "STREAM")) continue;
        res_addr a; if (!res_numeric(ip, port, &a)) continue;
        int dup = 0; for (int i = 0; i < n; i++) if (out[i].l == a.l && !memcmp(&out[i].a, &a.a, a.l)) dup = 1;
        if (!dup) out[n++] = a;
    }
    return n;
}
static int plat_resolve(const char *host, int port, res_addr *out, int max, int timeout_ms) {
    if (res_numeric(host, port, &out[0])) return 1;
    if (!strcasecmp(host, "localhost") || (strlen(host) > 10 && !strcasecmp(host + strlen(host) - 10, ".localhost"))) {
        int n = 0; if (res_numeric("127.0.0.1", port, &out[n])) n++; if (n < max && res_numeric("::1", port, &out[n])) n++; return n;
    }
    if (!res_name_ok(host)) return 0;
    int n = res_getent(host, port, out, max, timeout_ms);
    return n > 0 ? n : 0;   /* no getent at all: unknown (never glibc's in-process lookup) */
}
int plat_resolve4(const char *host, struct in_addr *out) {   /* first IPv4 address of host (ashc), 0 = found */
    res_addr r[RES_MAX]; int n = plat_resolve(host, 0, r, RES_MAX, 10000);
    for (int i = 0; i < n; i++) if (r[i].a.ss_family == AF_INET) { *out = ((struct sockaddr_in *)&r[i].a)->sin_addr; return 0; }
    return -1;
}
sock_t plat_connect_tcp(const char *host, int port, int timeout_ms) {
    res_addr ra[RES_MAX]; int na = plat_resolve(host, port, ra, RES_MAX, timeout_ms), s = -1;
    for (int i = 0; i < na; i++) {
        s = socket(ra[i].a.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) continue;
        int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int r = connect(s, (struct sockaddr *)&ra[i].a, ra[i].l);
        if (r && errno == EINPROGRESS) {
            struct pollfd p = { s, POLLOUT, 0 };
            if (poll(&p, 1, timeout_ms) == 1) { int e = 0; socklen_t el = sizeof e; getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el); r = e ? -1 : 0; }
            else r = -1;
        }
        fcntl(s, F_SETFL, fl);
        if (!r) break;
        close(s); s = -1;
    }
    return s;
}
int plat_free_port(void) {
    struct sockaddr_in sa; socklen_t sl = sizeof sa; int p = 0;
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return 0;
    mkaddr("127.0.0.1", 0, &sa);
    if (!bind(s, (struct sockaddr *)&sa, sizeof sa) && !getsockname(s, (struct sockaddr *)&sa, &sl)) p = ntohs(sa.sin_port);
    close(s); return p;
}
void plat_close(sock_t s) { if (s >= 0) close(s); }
void plat_shutdown(sock_t s) { if (s >= 0) shutdown(s, SHUT_RDWR); }
long plat_send(sock_t s, const void *b, size_t n) { for (;;) { long k = send(s, b, n, MSG_NOSIGNAL); if (k < 0 && errno == EINTR) continue; return k; } }
long plat_recv(sock_t s, void *b, size_t n) { for (;;) { long k = recv(s, b, n, 0); if (k < 0 && errno == EINTR) continue; return k; } }
int plat_send_all(sock_t s, const void *b, size_t n) {
    const char *p = b; while (n) { long k = plat_send(s, p, n); if (k <= 0) return -1; p += k; n -= (size_t)k; } return 0;
}
int plat_recv_all(sock_t s, void *b, size_t n) {
    char *p = b; while (n) { long k = plat_recv(s, p, n); if (k < 0) return -1; if (!k) return -2; p += k; n -= (size_t)k; } return 0;
}
int plat_set_timeout(sock_t s, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) | setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
void plat_nodelay(sock_t s) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
int plat_wait2(sock_t a, sock_t b, int timeout_ms) {
    struct pollfd p[2] = { { a, POLLIN, 0 }, { b, POLLIN, 0 } };
    int r = poll(p, b >= 0 ? 2 : 1, timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    return ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0) | ((b >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) ? 2 : 0);
}
int plat_wait_readable(sock_t s, int timeout_ms) { int r = plat_wait2(s, SOCK_BAD, timeout_ms); return r < 0 ? -1 : (r & 1); }
long plat_recvfrom(sock_t s, void *b, size_t n, char *peer, int pn, void *addr, int *alen) {
    struct sockaddr_in sa; socklen_t sl = sizeof sa;
    long k = recvfrom(s, b, n, 0, (struct sockaddr *)&sa, &sl);
    if (k >= 0) { inet_ntop(AF_INET, &sa.sin_addr, peer, (socklen_t)pn); if (addr && *alen >= (int)sl) { memcpy(addr, &sa, sl); *alen = (int)sl; } }
    return k;
}
long plat_sendto(sock_t s, const void *b, size_t n, const void *addr, int alen) { return sendto(s, b, n, 0, (const struct sockaddr *)addr, (socklen_t)alen); }

struct plat_mutex { pthread_mutex_t m; };
plat_mutex *plat_mutex_new(void) { plat_mutex *m = calloc(1, sizeof *m); if (m) pthread_mutex_init(&m->m, NULL); return m; }
void plat_lock(plat_mutex *m) { pthread_mutex_lock(&m->m); }
void plat_unlock(plat_mutex *m) { pthread_mutex_unlock(&m->m); }
typedef struct { void (*fn)(void *); void *arg; } thr_arg;
static void *thr_main(void *p) { thr_arg a = *(thr_arg *)p; free(p); a.fn(a.arg); return NULL; }
int plat_thread(void (*fn)(void *), void *arg) {
    pthread_t t; pthread_attr_t at; thr_arg *a = malloc(sizeof *a); if (!a) return -1;
    a->fn = fn; a->arg = arg;
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&at, 512 * 1024);
    int r = pthread_create(&t, &at, thr_main, a); pthread_attr_destroy(&at);
    if (r) { free(a); return -1; }
    return 0;
}

int plat_spawn(char *const argv[], const char *log_path, plat_proc *p, char *err, int en) {
    int ep[2]; if (pipe2(ep, O_CLOEXEC)) { snprintf(err, en, "pipe: %s", strerror(errno)); return -1; }
    pid_t pid = fork();
    if (pid < 0) { snprintf(err, en, "fork: %s", strerror(errno)); close(ep[0]); close(ep[1]); return -1; }
    if (!pid) {
        setpgid(0, 0);                    /* own process group: kill() takes Chrome & co. with it */
        signal(SIGPIPE, SIG_DFL);
        int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, 0); close(nul); }
        if (log_path) { int lf = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600); if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); close(lf); } }
        execvp(argv[0], argv);
        int e = errno; (void)!write(ep[1], &e, sizeof e); _exit(127);
    }
    close(ep[1]);
    int e = 0; long k = read(ep[0], &e, sizeof e); close(ep[0]);
    if (k == (long)sizeof e) { waitpid(pid, NULL, 0); snprintf(err, en, "cannot run %s: %s", argv[0], strerror(e)); return -1; }
    p->pid = pid; p->h = NULL;
    return 0;
}
int plat_proc_reap(plat_proc *p, int *how) {
    if (p->pid <= 0) return -1;
    int st = 0; pid_t r;
    do r = waitpid((pid_t)p->pid, &st, WNOHANG); while (r < 0 && errno == EINTR);
    if (r == 0) return 1;
    if (how) *how = r < 0 ? 0 : WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st);
    p->pid = 0; return 0;
}
int plat_proc_alive(plat_proc *p) { return plat_proc_reap(p, NULL) == 1; }
void plat_proc_kill(plat_proc *p) {
    if (p->pid <= 0) return;
    kill(-(pid_t)p->pid, SIGTERM); kill((pid_t)p->pid, SIGTERM);
    for (int i = 0; i < 30; i++) { if (waitpid((pid_t)p->pid, NULL, WNOHANG) == (pid_t)p->pid) { p->pid = 0; break; } usleep(100000); }
    if (p->pid > 0) { kill(-(pid_t)p->pid, SIGKILL); kill((pid_t)p->pid, SIGKILL); waitpid((pid_t)p->pid, NULL, 0); p->pid = 0; }
}

void plat_proc_stop(plat_proc *p, int ms) {
    if (p->pid <= 0) return;
    pid_t grp = (pid_t)p->pid;                 /* the child made itself a process group leader (plat_spawn) */
    kill(grp, SIGTERM);
    for (int i = 0; i < ms / 100; i++) { if (waitpid((pid_t)p->pid, NULL, WNOHANG) == (pid_t)p->pid) { p->pid = 0; break; } usleep(100000); }
    if (p->pid > 0) { plat_proc_kill(p); return; }
    /* the backend has exited; whatever it left in its group (a browser an older backend did not stop) goes too */
    if (kill(-grp, SIGTERM) == 0) {
        for (int i = 0; i < 20 && kill(-grp, 0) == 0; i++) usleep(100000);
        kill(-grp, SIGKILL);
    }
}
int64_t plat_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int64_t plat_time(void) { return (int64_t)time(NULL); }
void plat_fmt_time(int64_t t, char *out, int n) { time_t tt = (time_t)t; struct tm tm; localtime_r(&tt, &tm); strftime(out, (size_t)n, "%Y-%m-%d %H:%M", &tm); }
void plat_sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; while (nanosleep(&t, &t) && errno == EINTR) {} }
int plat_random(void *b, size_t n) {
    unsigned char *p = b;
#ifdef __linux__
    while (n) { ssize_t k = getrandom(p, n, 0); if (k < 0) { if (errno == EINTR) continue; break; } p += k; n -= (size_t)k; }
    if (!n) return 0;
#endif
    int f = open("/dev/urandom", O_RDONLY | O_CLOEXEC); if (f < 0) return -1;
    while (n) { ssize_t k = read(f, p, n); if (k <= 0) { close(f); return -1; } p += k; n -= (size_t)k; }
    close(f); return 0;
}
int plat_config_dir(char *out, int n) {
    const char *x = getenv("AMISTUDIO_HOST_DIR");
    if (x && *x) { snprintf(out, n, "%s", x); return 0; }
    x = getenv("XDG_CONFIG_HOME");
    if (x && *x) { snprintf(out, n, "%s/amistudio-host", x); return 0; }
    x = getenv("HOME"); if (!x) return -1;
    snprintf(out, n, "%s/.config/amistudio-host", x); return 0;
}
int plat_setenv(const char *name, const char *value) { return setenv(name, value, 1); }
int plat_data_dir(char *out, int n) {
    const char *x = getenv("XDG_DATA_HOME");
    if (x && *x) { snprintf(out, n, "%s/amistudio-host", x); return 0; }
    x = getenv("HOME"); if (!x) return -1;
    snprintf(out, n, "%s/.local/share/amistudio-host", x); return 0;
}
int plat_mkdir_private(const char *path) {
    char p[512]; snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0700); *q = '/'; }
    if (mkdir(p, 0700) && errno != EEXIST) return -1;
    return chmod(p, 0700);
}
int plat_write_private(const char *path, const void *b, size_t n) {
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); if (f < 0) return -1;
    const char *p = b; size_t left = n;
    while (left) { ssize_t k = write(f, p, left); if (k <= 0) { close(f); unlink(tmp); return -1; } p += k; left -= (size_t)k; }
    fsync(f); close(f);
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
const char *plat_os_name(void) {
    static char s[64]; if (!s[0]) { struct utsname u; if (!uname(&u)) { char *q; snprintf(s, sizeof s, "%s-%s", u.sysname, u.machine); for (q = s; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32; } else snprintf(s, sizeof s, "posix"); }
    return s;
}
int plat_exe_dir(char *out, int n) {
    char p[1024]; ssize_t k = readlink("/proc/self/exe", p, sizeof p - 1);
    if (k <= 0) return -1;
    p[k] = 0; char *s = strrchr(p, '/'); if (!s) return -1; *s = 0;
    snprintf(out, n, "%s", p[0] ? p : "/"); return 0;
}
int plat_hostname(char *out, int n) { if (gethostname(out, (size_t)n)) { snprintf(out, n, "host"); return -1; } out[n - 1] = 0; return 0; }
double plat_load(void) { double l[1]; return getloadavg(l, 1) == 1 ? l[0] : -1; }
static void (*g_stop)(void);
static void on_sig(int s) { (void)s; if (g_stop) g_stop(); }
void plat_on_signal(void (*stop)(void)) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig; g_stop = stop;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
}
