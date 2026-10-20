/* AmiStudio Host - Windows platform layer (Winsock 2, Win32 threads/processes).
 * Status: compiles with MinGW-w64 (make win-check); not yet run on Windows. Required before Host 1.0
 * (WinUAE / AmiKit users). See docs/PORTING-WINDOWS.md for what is left. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "plat.h"
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int plat_net_init(void) { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) ? -1 : 0; }
static int mkaddr(const char *addr, int port, struct sockaddr_in *sa) {
    memset(sa, 0, sizeof *sa); sa->sin_family = AF_INET; sa->sin_port = htons((u_short)port);
    return inet_pton(AF_INET, addr && *addr ? addr : "127.0.0.1", &sa->sin_addr) == 1 ? 0 : -1;
}
static sock_t mksock(int type) { sock_t s = WSASocketW(AF_INET, type, 0, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT | WSA_FLAG_OVERLAPPED); return s; }
sock_t plat_listen_tcp(const char *addr, int port, char *err, int en) {
    struct sockaddr_in sa; BOOL ex = TRUE;
    if (mkaddr(addr, port, &sa)) { snprintf(err, en, "bad listen address %s", addr); return SOCK_BAD; }
    sock_t s = mksock(SOCK_STREAM); if (s == SOCK_BAD) { snprintf(err, en, "socket: %d", WSAGetLastError()); return SOCK_BAD; }
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&ex, sizeof ex);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) || listen(s, 16)) { snprintf(err, en, "%s:%d: error %d", addr, port, WSAGetLastError()); closesocket(s); return SOCK_BAD; }
    return s;
}
sock_t plat_bind_udp(const char *addr, int port, char *err, int en) {
    struct sockaddr_in sa; BOOL one = TRUE;
    if (mkaddr(addr, port, &sa)) { snprintf(err, en, "bad address %s", addr); return SOCK_BAD; }
    sock_t s = mksock(SOCK_DGRAM); if (s == SOCK_BAD) { snprintf(err, en, "socket: %d", WSAGetLastError()); return SOCK_BAD; }
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa)) { snprintf(err, en, "udp %s:%d: error %d", addr, port, WSAGetLastError()); closesocket(s); return SOCK_BAD; }
    return s;
}
sock_t plat_accept(sock_t l, char *peer, int pn, int *loopback) {
    struct sockaddr_in sa; int sl = sizeof sa;
    sock_t c = accept(l, (struct sockaddr *)&sa, &sl); if (c == SOCK_BAD) return SOCK_BAD;
    SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
    inet_ntop(AF_INET, &sa.sin_addr, peer, (size_t)pn);
    *loopback = (ntohl(sa.sin_addr.s_addr) >> 24) == 127;
    return c;
}
sock_t plat_connect_tcp(const char *host, int port, int timeout_ms) {
    struct addrinfo hints, *res = NULL, *ai; char ps[16]; sock_t s = SOCK_BAD;
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res)) return SOCK_BAD;
    for (ai = res; ai; ai = ai->ai_next) {
        s = WSASocketW(ai->ai_family, ai->ai_socktype, ai->ai_protocol, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT | WSA_FLAG_OVERLAPPED);
        if (s == SOCK_BAD) continue;
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (r && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wf, ef; struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
            FD_ZERO(&wf); FD_SET(s, &wf); FD_ZERO(&ef); FD_SET(s, &ef);
            r = (select(0, NULL, &wf, &ef, &tv) == 1 && FD_ISSET(s, &wf)) ? 0 : -1;
        }
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        if (!r) break;
        closesocket(s); s = SOCK_BAD;
    }
    freeaddrinfo(res);
    return s;
}
int plat_free_port(void) {
    struct sockaddr_in sa; int sl = sizeof sa, p = 0; sock_t s = socket(AF_INET, SOCK_STREAM, 0); if (s == SOCK_BAD) return 0;
    mkaddr("127.0.0.1", 0, &sa);
    if (!bind(s, (struct sockaddr *)&sa, sizeof sa) && !getsockname(s, (struct sockaddr *)&sa, &sl)) p = ntohs(sa.sin_port);
    closesocket(s); return p;
}
void plat_close(sock_t s) { if (s != SOCK_BAD) closesocket(s); }
void plat_shutdown(sock_t s) { if (s != SOCK_BAD) shutdown(s, SD_BOTH); }
long plat_send(sock_t s, const void *b, size_t n) { return send(s, (const char *)b, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0); }
long plat_recv(sock_t s, void *b, size_t n) { return recv(s, (char *)b, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0); }
int plat_send_all(sock_t s, const void *b, size_t n) { const char *p = b; while (n) { long k = plat_send(s, p, n); if (k <= 0) return -1; p += k; n -= (size_t)k; } return 0; }
int plat_recv_all(sock_t s, void *b, size_t n) { char *p = b; while (n) { long k = plat_recv(s, p, n); if (k < 0) return -1; if (!k) return -2; p += k; n -= (size_t)k; } return 0; }
int plat_set_timeout(sock_t s, int ms) { DWORD t = (DWORD)ms; return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t) | setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t); }
void plat_nodelay(sock_t s) { BOOL one = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one); }
int plat_wait2(sock_t a, sock_t b, int timeout_ms) {
    WSAPOLLFD p[2] = { { a, POLLRDNORM, 0 }, { b, POLLRDNORM, 0 } };
    int r = WSAPoll(p, b != SOCK_BAD ? 2 : 1, timeout_ms);
    if (r < 0) return -1;
    return ((p[0].revents & (POLLRDNORM | POLLHUP | POLLERR)) ? 1 : 0) | ((b != SOCK_BAD && (p[1].revents & (POLLRDNORM | POLLHUP | POLLERR))) ? 2 : 0);
}
int plat_wait_readable(sock_t s, int timeout_ms) { int r = plat_wait2(s, SOCK_BAD, timeout_ms); return r < 0 ? -1 : (r & 1); }
long plat_recvfrom(sock_t s, void *b, size_t n, char *peer, int pn, void *addr, int *alen) {
    struct sockaddr_in sa; int sl = sizeof sa;
    long k = recvfrom(s, (char *)b, (int)n, 0, (struct sockaddr *)&sa, &sl);
    if (k >= 0) { inet_ntop(AF_INET, &sa.sin_addr, peer, (size_t)pn); if (addr && *alen >= sl) { memcpy(addr, &sa, (size_t)sl); *alen = sl; } }
    return k;
}
long plat_sendto(sock_t s, const void *b, size_t n, const void *addr, int alen) { return sendto(s, (const char *)b, (int)n, 0, (const struct sockaddr *)addr, alen); }

struct plat_mutex { CRITICAL_SECTION cs; };
plat_mutex *plat_mutex_new(void) { plat_mutex *m = calloc(1, sizeof *m); if (m) InitializeCriticalSection(&m->cs); return m; }
void plat_lock(plat_mutex *m) { EnterCriticalSection(&m->cs); }
void plat_unlock(plat_mutex *m) { LeaveCriticalSection(&m->cs); }
typedef struct { void (*fn)(void *); void *arg; } thr_arg;
static unsigned __stdcall thr_main(void *p) { thr_arg a = *(thr_arg *)p; free(p); a.fn(a.arg); return 0; }
int plat_thread(void (*fn)(void *), void *arg) {
    thr_arg *a = malloc(sizeof *a); if (!a) return -1; a->fn = fn; a->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 512 * 1024, thr_main, a, 0, NULL);
    if (!h) { free(a); return -1; }
    CloseHandle((HANDLE)h); return 0;
}

/* children run in a kill-on-close job object so Chrome & co. go away with the backend */
int plat_spawn(char *const argv[], const char *log_path, plat_proc *p, char *err, int en) {
    char cmd[8192]; int o = 0;
    for (int i = 0; argv[i] && o < (int)sizeof cmd - 4; i++) {          /* quote per CommandLineToArgvW rules (simple form) */
        int q = !argv[i][0] || strpbrk(argv[i], " \t\"") != NULL;
        if (i) cmd[o++] = ' ';
        if (q) cmd[o++] = '"';
        for (const char *c = argv[i]; *c && o < (int)sizeof cmd - 4; c++) { if (*c == '"') cmd[o++] = '\\'; cmd[o++] = *c; }
        if (q) cmd[o++] = '"';
    }
    cmd[o] = 0;
    STARTUPINFOA si; PROCESS_INFORMATION pi; SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE }; HANDLE lf = INVALID_HANDLE_VALUE;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (log_path) {
        lf = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (lf != INVALID_HANDLE_VALUE) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = lf; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); }
    }
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li); li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li); }
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, lf != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
    if (!ok) { snprintf(err, en, "cannot run %s: error %lu", argv[0], GetLastError()); if (job) CloseHandle(job); return -1; }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    p->pid = (long)pi.dwProcessId; p->h = pi.hProcess;
    p->job = job;
    return 0;
}
int plat_proc_reap(plat_proc *p, int *how) {
    if (!p->h) return -1;
    if (WaitForSingleObject(p->h, 0) == WAIT_TIMEOUT) return 1;
    DWORD code = 0; GetExitCodeProcess(p->h, &code); if (how) *how = (int)code;
    CloseHandle(p->h); p->h = NULL; if (p->job) { CloseHandle(p->job); p->job = NULL; } p->pid = 0; return 0;
}
int plat_proc_alive(plat_proc *p) { return plat_proc_reap(p, NULL) == 1; }
void plat_proc_kill(plat_proc *p) {
    if (!p->h) return;
    if (p->job) TerminateJobObject(p->job, 1); else TerminateProcess(p->h, 1);
    WaitForSingleObject(p->h, 3000);
    CloseHandle(p->h); p->h = NULL; if (p->job) { CloseHandle(p->job); p->job = NULL; } p->pid = 0;
}

/* Windows has no SIGTERM for console-less children: the job is ended as before */
void plat_proc_stop(plat_proc *p, int ms) { (void)ms; plat_proc_kill(p); }
int64_t plat_now_ms(void) { return (int64_t)GetTickCount64(); }
int64_t plat_time(void) { return (int64_t)time(NULL); }
void plat_fmt_time(int64_t t, char *out, int n) { time_t tt = (time_t)t; struct tm tm; localtime_s(&tm, &tt); strftime(out, (size_t)n, "%Y-%m-%d %H:%M", &tm); }
void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }
int plat_random(void *b, size_t n) { return BCryptGenRandom(NULL, (PUCHAR)b, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1; }
int plat_config_dir(char *out, int n) {
    const char *x = getenv("AMISTUDIO_HOST_DIR"); if (x && *x) { snprintf(out, n, "%s", x); return 0; }
    char app[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, app) != S_OK) return -1;
    snprintf(out, n, "%s\\AmiStudio-Host", app); return 0;
}
int plat_setenv(const char *name, const char *value) { return SetEnvironmentVariableA(name, value) ? 0 : -1; }   /* inherited by CreateProcessA(env NULL) */
int plat_data_dir(char *out, int n) {
    char app[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, app) != S_OK) return -1;
    snprintf(out, n, "%s\\AmiStudio-Host", app); return 0;
}
/* %APPDATA% is already private to the user; an explicit owner-only DACL is a TODO (docs/PORTING-WINDOWS.md) */
int plat_mkdir_private(const char *path) {
    char p[512]; snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 3; *q; q++) if (*q == '\\' || *q == '/') { char c = *q; *q = 0; _mkdir(p); *q = c; }
    return (_mkdir(p) == 0 || GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) ? 0 : -1;
}
int plat_write_private(const char *path, const void *b, size_t n) {
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp%lu", path, GetCurrentProcessId());
    FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    if (fwrite(b, 1, n, f) != n) { fclose(f); remove(tmp); return -1; }
    fflush(f); _commit(_fileno(f)); fclose(f);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { remove(tmp); return -1; }
    return 0;
}
const char *plat_os_name(void) { return "windows-x64"; }
int plat_exe_dir(char *out, int n) {
    char p[1024]; DWORD k = GetModuleFileNameA(NULL, p, sizeof p); if (!k || k >= sizeof p) return -1;
    char *s = strrchr(p, '\\'); if (!s) return -1; *s = 0; snprintf(out, n, "%s", p); return 0;
}
int plat_hostname(char *out, int n) { DWORD l = (DWORD)n; if (!GetComputerNameA(out, &l)) { snprintf(out, n, "host"); return -1; } return 0; }
double plat_load(void) { return -1; }
static void (*g_stop)(void);
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; if (g_stop) g_stop(); return TRUE; }
void plat_on_signal(void (*stop)(void)) { g_stop = stop; SetConsoleCtrlHandler(on_ctrl, TRUE); }
#endif
