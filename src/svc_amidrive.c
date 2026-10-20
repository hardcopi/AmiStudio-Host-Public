/* AmiStudio Host - amidrive v1 (0.1.8): Google Drive mounted on the Host PC with rclone, so Amiberry shows it as a
 * normal drawer (AMIDRIVE-HOST-REQUEST.md; wire format in docs/PROTOCOL.md "amidrive v1", files and lifecycle in
 * docs/SERVICES.md "amidrive").
 *
 *  {state}/amidrive/           rclone.conf (0600, remote "gdrive"), cache/ (--cache-dir), drawer.path, Drive.mount,
 *                              rclone.pid, amidrive.log, rclone.log, account, signin.expired, Google Drive.info (parked)
 *  amidrive_mountpoint         the mount (launcher: <AmiStudio drawer>/Google Drive; default {state}/amidrive/Google Drive)
 *
 * Lifecycle (Rich, 19.10.2026): mounted only while AmiStudio or AmiDrive is in use. CONNECT flag 1 (sent when AmiStudio
 * or AmiDrive starts) mounts when amidrive_automount is on; nothing mounts at Host boot. When the Host stops (the
 * launcher's quit-when-unused, idle exit, SIGTERM) amidrive flushes and unmounts first. busy() keeps the Host up only
 * with amidrive_keeps_host yes. A token is never used from a drawer that was copied or moved (drawer.path check).
 * rclone runs with RCLONE_CONFIG/RCLONE_CONFIG_DIR/RCLONE_CACHE_DIR in {state}/amidrive, never ~/.config or ~/.cache.
 * Tokens and the client secret never go to a client or a log, and never into a command line. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "host.h"
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* Windows has no FUSE/rclone mount path yet (docs/PORTING-WINDOWS.md): the service answers "unavailable" */
static void adw_status(service *sv, char *out, int n) { (void)sv; snprintf(out, n, "down no-fuse"); }
static void adw_run(service *sv, conn *c) {
    (void)sv; unsigned char st[96]; memset(st, 0, sizeof st); int o = 2 + 5 * 4 + 2 * 8; o += 3;
    const char *m = "Google Drive needs the Linux Host"; st[o++] = (unsigned char)strlen(m); memcpy(st + o, m, strlen(m)); o += (int)strlen(m);
    for (;;) { int op; unsigned char *p; uint32_t n; if (frame_recv(c->s, &op, &p, &n, 4096)) { free(p); return; }
        if (op == AD_STATUS) frame_send(c->s, AD_STATE, st, (uint32_t)o);
        if (op >= 1 && op <= 8) { unsigned char r[3] = { (unsigned char)op, op <= 2 || op == 4 ? 0 : 2, 0 }; frame_send(c->s, AD_RESULT, r, 3); }
        free(p); }
}
service *svc_amidrive_new(void) {
    service *sv = calloc(1, sizeof *sv); snprintf(sv->name, sizeof sv->name, "amidrive");
    sv->vmin = sv->vmax = 1; sv->need_auth = 1; sv->max_clients = 4; sv->run = adw_run; sv->status = adw_status; return sv;
}
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <openssl/evp.h>
extern char **environ;

#define MAXPATHS 20
typedef struct sess sess;
struct sess {
    conn *c; plat_mutex *wm; int dead;                       /* dead: the connection ended, sends are dropped (wm) */
    int watch; int64_t last_push; unsigned last_hash;        /* A.m */
    int ref, worker, auth_attached;                          /* A.m */
    int auth_want, auth_url_sent;                            /* A.m: AUTH_START method of this session; AUTH 0 already sent to it */
    int op, arg;
    sess *next;
};
static struct {
    plat_mutex *m; pthread_mutex_t opm;                      /* m: fields; opm: one mount/unmount/flush/signout at a time */
    char dir[600], conf[700], cache[700], mp[700], icon_park[700], icon_live[720], marker[700], pidf[700], drawerf[700],
         logf[700], rlogf[700], expiredf[700], accountf[700];
    char rclone[700], fusermount[300], mpname[200], icon_ship[700];
    int fuse_ok, rclone_ok; char down[40];
    int busy_state;                                          /* ADS_MOUNTING/FLUSHING/UNMOUNTING while an op runs, else 0 */
    int error; char msg[256];
    pid_t rpid, drain_pid; int expect_exit;   /* drain_pid: rclone still serving files left open after a lazy unmount */ int rc_port; char rc_auth[96]; char nonce[20]; int64_t mounted_at;
    int mounted;                                             /* our rclone is serving the mount point */
    uint32_t q_queued, q_inprog, pending_kb, open_files, errored; uint64_t quota_used, quota_total;
    uint32_t cache_dirty_n, cache_dirty_kb;                  /* pending uploads found in cache/ while unmounted */
    int lowdisk, notempty, copied, expired;
    char account[256];
    int sessions; sess *list;
    int64_t retry_at; int crash_retry; int abandon_ok;
    int auth_running; pid_t apid; int auth_cancel; sess *auth_cancel_by; int auth_step; char auth_text[1100]; char auth_url[1024]; int auth_url_flags; int auth_status; char auth_msg[256];
    volatile int busy; volatile int stopping;
    long rlog_off; int rc_fail;
} A;

/* ---------- small helpers ---------- */
static int isdir(const char *p);
static void ad_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ad_log(const char *fmt, ...) {
    char b[1200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    LOG("amidrive: %s", b);
    if (!A.logf[0] || !isdir(A.dir)) return;   /* {state}/amidrive is only created on first use */
    struct stat st; if (!stat(A.logf, &st) && st.st_size > 1024 * 1024) { char o[720]; snprintf(o, sizeof o, "%s.old", A.logf); rename(A.logf, o); }
    int f = open(A.logf, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600); if (f < 0) return;
    char t[32]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S ", &tm);
    char line[1300]; int n = snprintf(line, sizeof line, "%s%s\n", t, b);
    if (n > 0) (void)!write(f, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
    close(f);
}
static void setmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void setmsg(const char *fmt, ...) { va_list ap; va_start(ap, fmt); plat_lock(A.m); vsnprintf(A.msg, sizeof A.msg, fmt, ap); plat_unlock(A.m); va_end(ap); }
static int exists(const char *p) { struct stat st; return !lstat(p, &st); }
static int isdir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int dir_count(const char *p) {   /* entries in a folder, -1 = cannot read */
    DIR *d = opendir(p); if (!d) return -1; int n = 0; struct dirent *e;
    while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d); return n;
}
static char *read_small(const char *p, long max) {
    FILE *f = fopen(p, "r"); if (!f) return NULL;
    char *b = malloc((size_t)max + 1); size_t n = fread(b, 1, (size_t)max, f); fclose(f); b[n] = 0; return b;
}
static void read_line(const char *p, char *out, int n) {
    char *b = read_small(p, 4096); out[0] = 0; if (!b) return;
    char *nl = strchr(b, '\n'); if (nl) *nl = 0;
    snprintf(out, n, "%s", b); free(b); trim(out);
}
static int rm_rf(const char *p) {
    struct stat st; if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    DIR *d = opendir(p); if (!d) return -1; struct dirent *e; int rc = 0;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char q[1400]; snprintf(q, sizeof q, "%s/%s", p, e->d_name); if (rm_rf(q)) rc = -1; }
    closedir(d); if (rmdir(p)) rc = -1; return rc;
}
static int mkdirs(const char *p, mode_t m) {
    char t[700]; snprintf(t, sizeof t, "%s", p);
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, m); *q = '/'; }
    return mkdir(t, m) && errno != EEXIST ? -1 : 0;
}
static int find_exe(const char *name, char *out, int n) {   /* PATH search; 0 found */
    if (strchr(name, '/')) { snprintf(out, n, "%s", name); return access(name, X_OK); }
    const char *path = getenv("PATH"); if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char d[4096]; snprintf(d, sizeof d, "%s:/usr/bin:/bin:/usr/sbin:/sbin", path); char *save = NULL;
    for (char *s = strtok_r(d, ":", &save); s; s = strtok_r(NULL, ":", &save)) { snprintf(out, n, "%s/%s", s, name); if (!access(out, X_OK)) return 0; }
    return -1;
}
static void unoct(char *s) {   /* /proc/self/mountinfo escapes: \040 = space */
    char *w = s;
    for (char *r = s; *r; ) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') { *w++ = (char)((r[1]-'0') * 64 + (r[2]-'0') * 8 + (r[3]-'0')); r += 4; }
        else *w++ = *r++;
    }
    *w = 0;
}
static int is_mounted(const char *mp) {
    FILE *f = fopen("/proc/self/mountinfo", "r"); if (!f) return 0;
    char line[4096]; int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *save = NULL, *tok = strtok_r(line, " ", &save); int i = 0;
        while (tok && i < 4) { tok = strtok_r(NULL, " ", &save); i++; }
        if (!tok) continue;
        unoct(tok);
        if (!strcmp(tok, mp)) { found = 1; break; }
    }
    fclose(f); return found;
}
/* ---------- processes ---------- */
static char **env_make(const char *const *set) {   /* environ with "K=V" entries replaced/added and "-K" entries removed */
    int n = 0, m = 0; while (environ[n]) n++; while (set && set[m]) m++;
    char **e = calloc((size_t)(n + m + 1), sizeof *e); int k = 0;
    for (int i = 0; i < n; i++) {
        int drop = 0;
        for (int j = 0; j < m; j++) { const char *s = set[j]; const char *key = s[0] == '-' ? s + 1 : s; size_t kl = strcspn(key, "=");
            if (!strncmp(environ[i], key, kl) && environ[i][kl] == '=') { drop = 1; break; } }
        if (!drop) e[k++] = strdup(environ[i]);
    }
    for (int j = 0; j < m; j++) if (set[j][0] != '-') e[k++] = strdup(set[j]);
    e[k] = NULL; return e;
}
static void env_free(char **e) { for (int i = 0; e && e[i]; i++) { memset(e[i], 0, strlen(e[i])); free(e[i]); } free(e); }
/* fork + exec with envp; stdout/stderr to out_fd (-1 = /dev/null); own process group. Returns pid or -1 */
static pid_t spawn(char *const argv[], char **envp, int out_fd, int newpg) {
    pid_t pid = fork(); if (pid < 0) return -1;
    if (!pid) {
        if (newpg) setpgid(0, 0);
        sigset_t ss; sigemptyset(&ss); sigprocmask(SIG_SETMASK, &ss, NULL); signal(SIGPIPE, SIG_DFL);
        int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); if (out_fd < 0) { dup2(nul, 1); dup2(nul, 2); } }
        if (out_fd >= 0) { dup2(out_fd, 1); dup2(out_fd, 2); }
        for (int fd = 3; fd < 1024; fd++) close(fd);   /* no client socket may live on in rclone */
        if (strchr(argv[0], '/')) execve(argv[0], argv, envp ? envp : environ); else execvpe(argv[0], argv, envp ? envp : environ);
        _exit(127);
    }
    return pid;
}
static int wait_pid(pid_t pid, int ms) {   /* 1 exited (reaped), 0 still running */
    for (int t = 0; ; t += 50) { int st; pid_t r = waitpid(pid, &st, WNOHANG); if (r == pid || (r < 0 && errno == ECHILD)) return 1; if (t >= ms) return 0; plat_sleep_ms(50); }
}
static int run_capture(char *const argv[], char **envp, char *out, int on, int ms) {   /* exit status, -1 failed/timeout */
    int p[2]; if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = spawn(argv, envp, p[1], 1); close(p[1]); if (pid < 0) { close(p[0]); return -1; }
    int o = 0; if (out && on) out[0] = 0; int64_t end = plat_now_ms() + ms;
    for (;;) {
        struct pollfd pf = { p[0], POLLIN, 0 }; int r = poll(&pf, 1, 100);
        if (r > 0) { char b[512]; ssize_t k = read(p[0], b, sizeof b); if (k <= 0) break;
            if (out && o < on - 1) { int c = (int)k; if (c > on - 1 - o) c = on - 1 - o; memcpy(out + o, b, (size_t)c); o += c; out[o] = 0; } }
        if (plat_now_ms() > end) { kill(-pid, SIGKILL); kill(pid, SIGKILL); break; }
    }
    close(p[0]);
    int st = 0; for (int t = 0; t < 3000; t += 50) { pid_t r = waitpid(pid, &st, WNOHANG); if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1; plat_sleep_ms(50); }
    kill(-pid, SIGKILL); waitpid(pid, &st, 0); return -1;
}
static char **rclone_env(void) {
    char a[800], b[800], c[800];
    snprintf(a, sizeof a, "RCLONE_CONFIG=%s", A.conf); snprintf(b, sizeof b, "RCLONE_CONFIG_DIR=%s", A.dir); snprintf(c, sizeof c, "RCLONE_CACHE_DIR=%s", A.cache);
    const char *set[] = { a, b, c, "-RCLONE_DRIVE_CLIENT_ID", "-RCLONE_DRIVE_CLIENT_SECRET", "-RCLONE_PASSWORD_COMMAND", "-RCLONE_CONFIG_PASS", NULL };
    return env_make(set);
}
/* ---------- unmount (fusermount3, falling back to umount: some systems have no /etc/mtab) ---------- */
static int umount_mp(int lazy) {   /* 0 = not mounted afterwards */
    if (!is_mounted(A.mp)) return 0;
    char out[400]; char *a1[] = { A.fusermount, lazy ? "-uz" : "-u", A.mp, NULL };
    int r = run_capture(a1, NULL, out, sizeof out, 15000);
    if (!is_mounted(A.mp)) return 0;
    trim(out); ad_log("%s %s failed (%d%s%s); trying umount%s", A.fusermount, lazy ? "-uz" : "-u", r, out[0] ? ": " : "", out, lazy ? " -l" : "");
    char *a2[] = { "umount", lazy ? "-l" : A.mp, lazy ? A.mp : NULL, NULL };
    r = run_capture(a2, NULL, out, sizeof out, 15000);
    if (!is_mounted(A.mp)) return 0;
    trim(out); ad_log("umount%s \"%s\" failed (%d%s%s)", lazy ? " -l" : "", A.mp, r, out[0] ? ": " : "", out);
    return -1;
}
/* the folder and icon after an unmount: marker gone, empty mount point removed, icon parked in {state}/amidrive */
static void cleanup_after(void) {
    unlink(A.marker); unlink(A.pidf);
    if (!is_mounted(A.mp) && isdir(A.mp) && dir_count(A.mp) == 0) rmdir(A.mp);
    if (strcmp(A.icon_live, A.icon_park) && exists(A.icon_live) && !exists(A.mp)) {
        if (rename(A.icon_live, A.icon_park)) ad_log("cannot park %s: %s", A.icon_live, strerror(errno)); }
    plat_lock(A.m); A.mounted = 0; A.rc_port = 0; A.nonce[0] = 0; A.q_queued = A.q_inprog = A.pending_kb = A.open_files = A.errored = 0; plat_unlock(A.m);
}
/* a leftover rclone from a Host that was killed (pidfile, checked against its command line) */
static void kill_leftover(void) {
    char t[32]; read_line(A.pidf, t, sizeof t); long pid = atol(t);
    if (pid <= 1 || pid == (long)A.rpid || pid == (long)A.drain_pid) return;
    char p[64], cmd[4096]; snprintf(p, sizeof p, "/proc/%ld/cmdline", pid);
    int f = open(p, O_RDONLY | O_CLOEXEC); if (f < 0) { unlink(A.pidf); return; }
    ssize_t n = read(f, cmd, sizeof cmd - 1); close(f); if (n <= 0) { unlink(A.pidf); return; } cmd[n] = 0;
    int is_rc = strstr(cmd, "rclone") != NULL, is_mount = 0, has_mp = 0;
    for (char *q = cmd; q < cmd + n; q += strlen(q) + 1) { if (!strcmp(q, "mount")) is_mount = 1; if (!strcmp(q, A.mp)) has_mp = 1; }
    if (!is_rc || !is_mount || !has_mp) { ad_log("rclone.pid names pid %ld, which is not our rclone mount: left alone", pid); unlink(A.pidf); return; }
    ad_log("stopping a leftover rclone (pid %ld) from an earlier Host", pid);
    kill((pid_t)pid, SIGTERM);
    for (int i = 0; i < 50 && !kill((pid_t)pid, 0); i++) plat_sleep_ms(100);
    if (!kill((pid_t)pid, 0)) { kill((pid_t)pid, SIGKILL); for (int i = 0; i < 20 && !kill((pid_t)pid, 0); i++) plat_sleep_ms(100); }
    unlink(A.pidf);
}
static void stale_clear(const char *why) {
    kill_leftover();
    if (is_mounted(A.mp)) { ad_log("%s: clearing a stale mount at \"%s\"", why, A.mp); umount_mp(1); }
    if (exists(A.marker) || (strcmp(A.icon_live, A.icon_park) && exists(A.icon_live) && !exists(A.mp)) || (isdir(A.mp) && dir_count(A.mp) == 0)) cleanup_after();
}
/* stop our rclone: it normally exits by itself once the mount is gone */
static void stop_rclone(int ms) {
    pid_t p; plat_lock(A.m); p = A.rpid; A.expect_exit = 1; plat_unlock(A.m);
    if (p > 0 && !wait_pid(p, ms)) { kill(p, SIGTERM); if (!wait_pid(p, 5000)) { kill(-p, SIGKILL); kill(p, SIGKILL); wait_pid(p, 3000); } }
    plat_lock(A.m); A.rpid = 0; plat_unlock(A.m);
}
/* ---------- rclone remote control (loopback HTTP, basic auth) ---------- */
static int rc_call(const char *method, const char *body, char *out, int on) {   /* HTTP status or -1 */
    int port; char auth[96]; plat_lock(A.m); port = A.rc_port; snprintf(auth, sizeof auth, "%s", A.rc_auth); plat_unlock(A.m);
    if (out && on) out[0] = 0;
    if (!port) return -1;
    sock_t s = plat_connect_tcp("127.0.0.1", port, 1500); if (s == SOCK_BAD) return -1;
    plat_set_timeout(s, 5000);
    char req[1200]; int bl = body ? (int)strlen(body) : 2;
    int n = snprintf(req, sizeof req, "POST /%s HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                     method, auth, bl, body ? body : "{}");
    if (plat_send_all(s, req, (size_t)n)) { plat_close(s); return -1; }
    const int cap = 262144; char *buf = malloc((size_t)cap); int o = 0;   /* heap, not TLS: thread stacks are 512 KB */
    if (!buf) { plat_close(s); return -1; }
    for (;;) { long k = plat_recv(s, buf + o, (size_t)(cap - 1 - o)); if (k <= 0) break; o += (int)k; if (o >= cap - 1) break; }
    plat_close(s); buf[o] = 0;
    int st = -1; if (sscanf(buf, "HTTP/%*s %d", &st) != 1) { free(buf); return -1; }
    char *b = strstr(buf, "\r\n\r\n"); if (b && out) snprintf(out, on, "%s", b + 4);
    free(buf); return st;
}
static int js_num(const char *js, const char *key, double *v) {
    char k[64]; snprintf(k, sizeof k, "\"%s\"", key); const char *p = strstr(js, k); if (!p) return -1;
    p += strlen(k); while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
    if (!strncmp(p, "true", 4)) { *v = 1; return 0; } if (!strncmp(p, "false", 5)) { *v = 0; return 0; }
    char *e; *v = strtod(p, &e); return e == p ? -1 : 0;
}
static int js_str(const char *js, const char *key, char *out, int n) {
    char k[64]; snprintf(k, sizeof k, "\"%s\"", key); const char *p = strstr(js, k); out[0] = 0; if (!p) return -1;
    p += strlen(k); while (*p == ' ' || *p == ':' || *p == '\t') p++; if (*p != '"') return -1; p++;
    int o = 0; while (*p && *p != '"' && o < n - 1) { if (*p == '\\' && p[1]) p++; out[o++] = *p++; } out[o] = 0; return 0;
}
typedef struct { int n; long long bytes; int ids[512]; int nids; } queue_info;
static int rc_queue(queue_info *q) {
    const int cap = 262144; char *js = malloc((size_t)cap); memset(q, 0, sizeof *q);
    if (!js) return -1;
    if (rc_call("vfs/queue", "{}", js, cap) != 200) { free(js); return -1; }
    const char *p = strstr(js, "\"queue\""); if (!p) { free(js); return 0; }
    while ((p = strstr(p, "\"name\""))) {
        const char *nx = strstr(p + 6, "\"name\""); char seg[2048]; int l = nx ? (int)(nx - p) : (int)strlen(p); if (l >= (int)sizeof seg) l = sizeof seg - 1;
        memcpy(seg, p, (size_t)l); seg[l] = 0; double id = 0, size = 0, up = 0, ex = 0;
        js_num(seg, "id", &id); js_num(seg, "size", &size); js_num(seg, "uploading", &up); js_num(seg, "expiry", &ex);
        q->n++; q->bytes += (long long)size;
        if (!up && ex > 0 && q->nids < 512) q->ids[q->nids++] = (int)id;
        p += 6;
    }
    free(js); return 0;
}
static long long rc_bytes_done(void) { char js[8192]; double b = 0; if (rc_call("core/stats", "{}", js, sizeof js) != 200) return -1; js_num(js, "bytes", &b); return (long long)b; }
/* ---------- state ---------- */
static int signed_in(void) { char *b = read_small(A.conf, 65536); int r = b && strstr(b, "[gdrive]") != NULL; if (b) { memset(b, 0, strlen(b)); free(b); } return r; }
static int state_locked(int si) {
    if (!A.fuse_ok || !A.rclone_ok) return ADS_UNAVAILABLE;
    if (A.busy_state) return A.busy_state;
    if (A.auth_running) return ADS_SIGNING_IN;
    if (A.mounted) return ADS_MOUNTED;
    if (A.error) return ADS_ERROR;
    if (!si) return ADS_SIGNED_OUT;
    return ADS_READY;
}
static int flags_locked(void) {
    int f = 0;
    if (A.fuse_ok) f |= ADF_FUSE;
    if (A.rclone_ok) f |= ADF_RCLONE;
    if ((A.mounted && (A.q_queued || A.q_inprog)) || (!A.mounted && A.cache_dirty_n)) f |= ADF_PENDING;
    if (A.lowdisk) f |= ADF_LOWDISK;
    if (A.expired) f |= ADF_EXPIRED;
    if (A.notempty) f |= ADF_NOTEMPTY;
    if (A.copied) f |= ADF_COPIED;
    return f;
}
/* every Host -> client frame stays under 8 KB (AmiDrive's receive buffer): the largest is a RESULT 1 with 20 paths of
   at most 255 bytes, 3 + 256 + 2 + 20 * 256 = 5381 bytes */
#define AD_MAX_FRAME 8000
typedef struct { unsigned char b[AD_MAX_FRAME]; int n; } fbuf;
static void fb_u8(fbuf *f, unsigned v) { if (f->n < (int)sizeof f->b) f->b[f->n++] = (unsigned char)v; }
static void fb_u16(fbuf *f, unsigned v) { fb_u8(f, v & 255); fb_u8(f, (v >> 8) & 255); }
static void fb_u32(fbuf *f, uint32_t v) { for (int i = 0; i < 4; i++) fb_u8(f, (v >> (8 * i)) & 255); }
static void fb_u64(fbuf *f, uint64_t v) { for (int i = 0; i < 8; i++) fb_u8(f, (unsigned)((v >> (8 * i)) & 255)); }
static void fb_str(fbuf *f, const char *s) {   /* u8 length + bytes, cut to 255 without splitting a UTF-8 sequence */
    size_t l = strlen(s); if (l > 255) { l = 255; while (l && (((unsigned char)s[l] & 0xC0) == 0x80)) l--; }
    fb_u8(f, (unsigned)l); for (size_t i = 0; i < l; i++) fb_u8(f, (unsigned char)s[i]); }
static void fb_str_end(fbuf *f, const char *s) {   /* paths: cut to 255 bytes keeping the end, with a leading "…" */
    size_t l = strlen(s); if (l <= 255) { fb_str(f, s); return; }
    const char *t = s + l - 252; while (((unsigned char)*t & 0xC0) == 0x80) t++;
    size_t tl = strlen(t); fb_u8(f, (unsigned)(tl + 3)); fb_u8(f, 0xE2); fb_u8(f, 0x80); fb_u8(f, 0xA6); for (size_t i = 0; i < tl; i++) fb_u8(f, (unsigned char)t[i]);
}
static void state_frame(fbuf *f) {
    int si = signed_in();
    plat_lock(A.m);
    fb_u8(f, (unsigned)state_locked(si)); fb_u8(f, (unsigned)flags_locked());
    fb_u32(f, A.mounted ? A.q_queued : A.cache_dirty_n); fb_u32(f, A.mounted ? A.q_inprog : 0); fb_u32(f, A.mounted ? A.pending_kb : A.cache_dirty_kb);
    fb_u32(f, A.open_files); fb_u32(f, A.errored); fb_u64(f, A.quota_used); fb_u64(f, A.quota_total);
    fb_str(f, si ? A.account : ""); fb_str(f, A.mp); fb_str(f, A.mounted ? A.nonce : "");
    const char *m = A.msg;
    if (!A.rclone_ok) m = "rclone is missing from the Host (engine/rclone/rclone)";
    else if (!A.fuse_ok) m = "FUSE is missing on the Host PC: install fuse3";
    fb_str(f, m);
    plat_unlock(A.m);
}
static unsigned hash_bytes(const unsigned char *b, int n) { unsigned h = 2166136261u; for (int i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }
/* ---------- sessions ---------- */
static void s_unref(sess *s) { plat_lock(A.m); int r = --s->ref; plat_unlock(A.m); if (!r) free(s); }   /* s->wm is kept: plat has no mutex free (one per session) */
static int s_send(sess *s, int op, const void *p, uint32_t n) { plat_lock(s->wm); int r = s->dead ? -1 : frame_send(s->c->s, op, p, n); plat_unlock(s->wm); return r; }
static void s_result(sess *s, int op, int st, const char *msg, char (*paths)[1024], int np) {
    fbuf f; f.n = 0; fb_u8(&f, (unsigned)op); fb_u8(&f, (unsigned)st); fb_str(&f, msg ? msg : "");
    if (st == ADR_FILES_OPEN) { if (np > MAXPATHS) np = MAXPATHS; fb_u16(&f, (unsigned)np); for (int i = 0; i < np; i++) fb_str_end(&f, paths[i]); }
    s_send(s, AD_RESULT, f.b, (uint32_t)f.n);
}
static void s_state(sess *s) {
    fbuf f; f.n = 0; state_frame(&f);
    if (!s_send(s, AD_STATE, f.b, (uint32_t)f.n)) { plat_lock(A.m); s->last_hash = hash_bytes(f.b, f.n); s->last_push = plat_now_ms(); plat_unlock(A.m); }
}
/* PROGRESS goes to the session that asked (FLUSH/UNMOUNT/SIGNOUT; none for the Host's own stop) and to every watcher */
static void s_progress(sess *s, uint32_t files, uint32_t kb) {
    unsigned char b[8]; put_u32(b, files); put_u32(b + 4, kb);
    sess *v[17]; int n = 0;
    plat_lock(A.m);
    if (s) { s->ref++; v[n++] = s; }
    for (sess *q = A.list; q && n < 17; q = q->next) if (q->watch && q != s) { q->ref++; v[n++] = q; }
    plat_unlock(A.m);
    for (int i = 0; i < n; i++) { s_send(v[i], AD_PROGRESS, b, 8); s_unref(v[i]); }
}
/* every session that asked for this sign-in gets the AUTH steps */
/* AUTH frames. AUTH 0 (URL) carries a trailing u8 (0.1.9+): ADAU_ENGINE = this Host has a browser
   engine (AmiSurf through this Host opens the URL on this PC), ADAU_NO_DESKTOP = sent because the desktop browser of a
   method 0 sign-in could not open. Older clients read step + string and ignore the byte. */
#define ADAU_ENGINE 1
#define ADAU_NO_DESKTOP 2
static void auth_frame(fbuf *f, int step, const char *text, int flags) { f->n = 0; fb_u8(f, (unsigned)step); fb_str(f, text); if (step == ADA_URL) fb_u8(f, (unsigned)flags); }
/* who: 0 every session in the sign-in; AUTH_TO_URL those that asked for the URL (method 1/2) and have not had it;
   AUTH_TO_DESKTOP those that asked for the desktop browser (method 0) */
enum { AUTH_TO_ALL = 0, AUTH_TO_URL = 1, AUTH_TO_DESKTOP = 2 };
static void auth_send(int step, const char *text, int flags, int who) {
    sess *v[16]; int n = 0;
    plat_lock(A.m);
    if (who != AUTH_TO_URL) { A.auth_step = step; snprintf(A.auth_text, sizeof A.auth_text, "%s", text); }
    for (sess *s = A.list; s && n < 16; s = s->next) {
        if (!s->auth_attached) continue;
        if (who == AUTH_TO_URL && (!s->auth_want || s->auth_url_sent)) continue;
        if (who == AUTH_TO_DESKTOP && (s->auth_want || s->auth_url_sent)) continue;
        if (step == ADA_URL) s->auth_url_sent = 1;
        s->ref++; v[n++] = s;
    }
    plat_unlock(A.m);
    fbuf f; auth_frame(&f, step, text, flags);
    for (int i = 0; i < n; i++) { s_send(v[i], AD_AUTH, f.b, (uint32_t)f.n); s_unref(v[i]); }
}
static void auth_broadcast(int step, const char *text) { auth_send(step, text, 0, AUTH_TO_ALL); }
/* ---------- checks ---------- */
static int drawer_check(void) {   /* 0 same drawer (or nothing stored), 1 copied (stored path exists), 2 moved */
    char stored[700], cur[700]; read_line(A.drawerf, stored, sizeof stored);
    if (!stored[0]) return 0;
    if (!realpath(A.dir, cur)) snprintf(cur, sizeof cur, "%s", A.dir);
    if (!strcmp(stored, cur)) return 0;
    struct stat a, b; if (!stat(stored, &a) && !stat(cur, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino) return 0;
    return isdir(stored) ? 1 : 2;
}
static void write_drawer_path(void) { char cur[700]; if (!realpath(A.dir, cur)) snprintf(cur, sizeof cur, "%s", A.dir); char b[720]; int n = snprintf(b, sizeof b, "%s\n", cur); plat_write_private(A.drawerf, b, (size_t)n); }
static void set_cache_aside(void) {
    if (!isdir(A.cache) || dir_count(A.cache) <= 0) return;
    char t[32], to[760]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(t, sizeof t, "%Y%m%d-%H%M%S", &tm);
    snprintf(to, sizeof to, "%s/cache.copied-%s", A.dir, t);
    if (rename(A.cache, to)) ad_log("cannot set cache/ aside: %s", strerror(errno));
    else ad_log("copied drawer: its cache/ (with any pending uploads of the original) is set aside as %s and never uploaded", to);
}
static void scan_dirty(const char *d, uint32_t *n, uint64_t *bytes, int depth) {
    if (depth > 64) return;
    DIR *dh = opendir(d); if (!dh) return;
    struct dirent *e;
    while ((e = readdir(dh))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", d, e->d_name); struct stat st; if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { scan_dirty(p, n, bytes, depth + 1); continue; }
        if (!S_ISREG(st.st_mode) || st.st_size > 65536) continue;
        char *b = read_small(p, 65536); if (!b) continue;
        if (strstr(b, "\"Dirty\": true") || strstr(b, "\"Dirty\":true")) { double sz = 0; js_num(b, "Size", &sz); (*n)++; *bytes += (uint64_t)sz; }
        free(b);
    }
    closedir(dh);
}
static void cache_pending(uint32_t *n, uint32_t *kb) { uint64_t by = 0; char m[760]; snprintf(m, sizeof m, "%s/vfsMeta", A.cache); *n = 0; scan_dirty(m, n, &by, 0); *kb = (uint32_t)((by + 1023) / 1024); }
/* open files under the mount point: /proc/<pid>/fd links and working directories ("dir/"), each path counted once
   however many processes hold it. Returns the number of distinct paths; the first max go to paths. */
typedef struct { char **v; int n, cap; } pathset;
static int ps_add(pathset *p, const char *s) {
    for (int i = 0; i < p->n; i++) if (!strcmp(p->v[i], s)) return 0;
    if (p->n >= 4096) return 0;
    if (p->n == p->cap) { int c = p->cap ? p->cap * 2 : 64; char **nv = realloc(p->v, sizeof *nv * (size_t)c); if (!nv) return 0; p->v = nv; p->cap = c; }
    p->v[p->n++] = strdup(s); return 1;
}
static int open_files(char (*paths)[1024], int max) {
    DIR *d = opendir("/proc"); if (!d) return 0; struct dirent *e; size_t ml = strlen(A.mp); pathset ps = { 0 };
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char fdd[300], l[4096], q[600]; ssize_t k;
        snprintf(q, sizeof q, "/proc/%s/cwd", e->d_name);
        if ((k = readlink(q, l, sizeof l - 1)) > 0) { l[k] = 0;
            if (!strncmp(l, A.mp, ml) && (l[ml] == '/' || !l[ml])) { char t[4100]; snprintf(t, sizeof t, "%s/", l[ml] ? l + ml + 1 : ""); ps_add(&ps, t); } }
        snprintf(fdd, sizeof fdd, "/proc/%s/fd", e->d_name);
        DIR *fd = opendir(fdd); if (!fd) continue; struct dirent *f;
        while ((f = readdir(fd))) {
            if (f->d_name[0] == '.') continue;
            snprintf(q, sizeof q, "%s/%s", fdd, f->d_name); if ((k = readlink(q, l, sizeof l - 1)) <= 0) continue; l[k] = 0;
            if (strncmp(l, A.mp, ml) || (l[ml] != '/' && l[ml])) continue;
            ps_add(&ps, l[ml] ? l + ml + 1 : "/");
        }
        closedir(fd);
    }
    closedir(d);
    for (int i = 0; i < ps.n; i++) { if (paths && i < max) snprintf(paths[i], 1024, "%s", ps.v[i]); free(ps.v[i]); }
    free(ps.v); return ps.n;
}
static const char *net_words[] = { "dial tcp", "no such host", "i/o timeout", "connection refused", "network is unreachable", "TLS handshake timeout",
                                   "context deadline exceeded", "connection reset", "temporary failure in name resolution", NULL };
/* the last ERROR/CRITICAL text in rclone.log since offset off (timestamps cut), and whether it was invalid_grant */
static void log_since(long off, char *out, int n, int *grant, int *net) {
    FILE *f = fopen(A.rlogf, "r"); out[0] = 0; *grant = 0; *net = 0; if (!f) return;
    fseek(f, off, SEEK_SET); char line[2048];
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "invalid_grant")) *grant = 1;
        if (strstr(line, "CRITICAL") || strstr(line, "ERROR") || strstr(line, "Fatal")) {
            for (int i = 0; net_words[i]; i++) if (strcasestr(line, net_words[i])) *net = 1;
            trim(line); char *m = strstr(line, ": "); snprintf(out, n, "%s", m ? m + 2 : line); }
    }
    fclose(f);
}
static long file_size(const char *p) { struct stat st; return stat(p, &st) ? 0 : (long)st.st_size; }
static void set_expired(int on) {
    plat_lock(A.m); A.expired = on; plat_unlock(A.m);
    if (on) plat_write_private(A.expiredf, "invalid_grant\n", 14); else unlink(A.expiredf);
}
/* ---------- connect ---------- */
static void add_arg(char **v, int *n, const char *s) { if (*n < 120) v[(*n)++] = strdup(s); }
/* amidrive_flags: split on spaces with "double quotes"; flags that must never be used are dropped */
static void add_user_flags(char **v, int *n) {
    const char *p = g_cfg.amidrive_flags; char tok[512];
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int o = 0, q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) { if (*p == '"') { q = !q; p++; continue; } if (o < (int)sizeof tok - 1) tok[o++] = *p; p++; }
        tok[o] = 0;
        static const char *banned[] = { "--allow-non-empty", "--allow-other", "--allow-root", "--daemon", "--drive-use-trash", "--config", "--cache-dir", "--rc-addr", "--rc-user", "--rc-pass", "--log-file", NULL };
        int bad = 0; for (int i = 0; banned[i]; i++) { size_t l = strlen(banned[i]); if (!strncmp(tok, banned[i], l) && (!tok[l] || tok[l] == '=')) bad = 1; }
        if (bad) { ad_log("amidrive_flags: %s ignored (not allowed)", tok); continue; }
        add_arg(v, n, tok);
    }
}
static void ensure_dir(void) { if (!isdir(A.dir)) { mkdirs(g_cfg.data_dir, 0700); plat_mkdir_private(A.dir); } }
/* {state}/amidrive/amidrive.lock (flock, held for the Host's life): a second Host on the same data folder (a test Host,
   a second port) never clears or mounts this Drive. A killed Host's lock goes with it; rclone does not inherit it. */
static int g_lockfd = -1;
static int take_lock(int create) {   /* 1 = this Host owns {state}/amidrive */
    if (g_lockfd >= 0) return 1;
    if (!isdir(A.dir)) { if (!create) return 0; ensure_dir(); }
    char p[720]; snprintf(p, sizeof p, "%s/amidrive.lock", A.dir);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600); if (fd < 0) return 0;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return 0; }
    g_lockfd = fd; return 1;
}
static int connect_locked_op(int automatic, char *msg, int mn);
static int do_connect(int automatic, char *msg, int mn) {
    pthread_mutex_lock(&A.opm); int r = connect_locked_op(automatic, msg, mn); pthread_mutex_unlock(&A.opm); return r;
}
static int connect_locked_op(int automatic, char *msg, int mn) {
    msg[0] = 0;
    if (A.stopping) { snprintf(msg, mn, "The Host is stopping"); return ADR_FAILED; }
    if (!A.rclone_ok) { snprintf(msg, mn, "rclone is missing from the Host"); return ADR_FAILED; }
    if (!A.fuse_ok) { snprintf(msg, mn, "FUSE is missing on the Host PC: install fuse3"); return ADR_FAILED; }
    if (automatic && !g_cfg.amidrive_automount) { msg[0] = 0; return ADR_OK; }   /* amidrive_automount no: nothing to do */
    plat_lock(A.m); int mounted = A.mounted, auth = A.auth_running; plat_unlock(A.m);
    if (mounted && is_mounted(A.mp)) { snprintf(msg, mn, "Google Drive is already mounted"); return ADR_OK; }
    if (auth) { snprintf(msg, mn, "A sign-in is in progress"); return ADR_BUSY; }
    if (A.drain_pid) {
        int st, gone = 0; pid_t dp = A.drain_pid;
        /* a draining rclone whose last file was just closed takes a moment to exit: wait up to 3 s for it */
        for (int i = 0; i < 30 && !gone; i++) {
            pid_t w = waitpid(dp, &st, WNOHANG);
            if (w == dp || (w < 0 && errno == ECHILD && kill(dp, 0) && errno == ESRCH)) gone = 1;
            else { plat_lock(A.m); if (!A.drain_pid) gone = 1; plat_unlock(A.m); }   /* the monitor reaped it */
            if (!gone) usleep(100000);
        }
        if (gone) { plat_lock(A.m); A.drain_pid = 0; plat_unlock(A.m); unlink(A.pidf); }
        else { snprintf(msg, mn, "files from the last mount are still open; close them first"); return ADR_BUSY; }
    }
    if (!signed_in()) { snprintf(msg, mn, "Sign in to Google Drive first"); return ADR_NOT_SIGNED_IN; }
    if (A.expired) { snprintf(msg, mn, "The Google sign-in has expired: sign in again"); return ADR_EXPIRED; }
    if (!take_lock(1)) { snprintf(msg, mn, "another AmiStudio Host is using this Google Drive (%s)", A.dir); return ADR_BUSY; }
    int dc = drawer_check();
    if (dc) {
        if (dc == 1) set_cache_aside();
        plat_lock(A.m); A.copied = 1; A.retry_at = 0; plat_unlock(A.m);
        ad_log("drawer %s (drawer.path does not match): token not used", dc == 1 ? "copied" : "moved");
        snprintf(msg, mn, "This drawer was %s: sign in again", dc == 1 ? "copied" : "moved"); setmsg("%s", msg); return ADR_COPIED;
    }
    plat_lock(A.m); A.copied = 0; A.busy_state = ADS_MOUNTING; A.error = 0; plat_unlock(A.m);
    stale_clear("before mounting");
    int r = ADR_FAILED;
    if (!exists(A.mp)) {
        if (mkdirs(A.mp, 0755) && !isdir(A.mp)) { snprintf(msg, mn, "cannot create \"%s\": %s", A.mp, strerror(errno)); goto out; }
        chmod(A.mp, 0755);   /* 0755 whatever the umask */
    }
    int cnt = dir_count(A.mp);
    if (cnt != 0) {
        plat_lock(A.m); A.notempty = 1; A.retry_at = 0; plat_unlock(A.m);
        if (cnt < 0) snprintf(msg, mn, "%s cannot be read: %s", A.mpname, strerror(errno));
        else snprintf(msg, mn, "%s has %d file%s in it; move them away", A.mpname, cnt, cnt == 1 ? "" : "s");
        ad_log("%s", msg); r = ADR_NOT_EMPTY; goto out;
    }
    plat_lock(A.m); A.notempty = 0; plat_unlock(A.m);
    mkdirs(A.cache, 0700);
    if (file_size(A.rlogf) > 4 * 1024 * 1024) { char o[720]; snprintf(o, sizeof o, "%s.old", A.rlogf); rename(A.rlogf, o); }
    unsigned char rnd[24]; plat_random(rnd, sizeof rnd); char pass[33]; hex(rnd, 16, pass);
    char nonce[17]; hex(rnd + 16, 8, nonce);
    char up[64]; snprintf(up, sizeof up, "amidrive:%s", pass);
    unsigned char b64[96]; EVP_EncodeBlock(b64, (const unsigned char *)up, (int)strlen(up));
    int port = plat_free_port();
    char addr[40]; snprintf(addr, sizeof addr, "127.0.0.1:%d", port);
    char *v[128]; int n = 0;
    const char *defs[] = { A.rclone, "mount", "gdrive:", A.mp, "--cache-dir", A.cache, "--vfs-cache-mode", "full", "--vfs-write-back", "10s",
        "--vfs-cache-max-size", "2G", "--vfs-cache-max-age", "24h", "--vfs-cache-min-free-space", "1G", "--dir-cache-time", "5m",
        "--poll-interval", "1m", "--drive-skip-gdocs", "--transfers", "2", "--tpslimit", "8", NULL };
    for (int i = 0; defs[i]; i++) add_arg(v, &n, defs[i]);
    add_user_flags(v, &n);
    const char *tail[] = { "--drive-use-trash=true", "--rc", "--rc-addr", addr, "--rc-user", "amidrive", "--rc-pass", pass, "--log-file", A.rlogf, NULL };
    for (int i = 0; tail[i]; i++) add_arg(v, &n, tail[i]);
    v[n] = NULL;
    long logoff = file_size(A.rlogf);
    char **env = rclone_env();
    /* "--rc-pass" is on rclone's command line (readable by this user only through /proc of the same uid): it only
       guards the loopback rc port against other local users, never a Google credential */
    pid_t pid = spawn(v, env, -1, 1);
    env_free(env); for (int i = 0; i < n; i++) free(v[i]);
    if (pid < 0) { snprintf(msg, mn, "cannot start rclone: %s", strerror(errno)); goto out; }
    { char pb[32]; int k = snprintf(pb, sizeof pb, "%ld\n", (long)pid); plat_write_private(A.pidf, pb, (size_t)k); }
    plat_lock(A.m); A.rpid = pid; A.expect_exit = 0; A.rc_port = port; snprintf(A.rc_auth, sizeof A.rc_auth, "%s", (char *)b64);
    snprintf(A.nonce, sizeof A.nonce, "%s", nonce); A.rlog_off = logoff; A.rc_fail = 0; plat_unlock(A.m);
    ad_log("mounting gdrive: at \"%s\" (rclone pid %ld)", A.mp, (long)pid);
    int ok = 0, exited = 0;
    for (int t = 0; t < 30000 && !A.stopping; t += 100) {
        if (is_mounted(A.mp)) { ok = 1; break; }
        int st; if (waitpid(pid, &st, WNOHANG) == pid) { exited = 1; break; }
        plat_sleep_ms(100);
    }
    if (!ok) {
        if (!exited) { kill(pid, SIGTERM); if (!wait_pid(pid, 3000)) { kill(-pid, SIGKILL); kill(pid, SIGKILL); wait_pid(pid, 2000); } }
        plat_lock(A.m); A.rpid = 0; A.rc_port = 0; plat_unlock(A.m);
        if (is_mounted(A.mp)) umount_mp(1);
        cleanup_after();
        char lm[400]; int grant, net; log_since(logoff, lm, sizeof lm, &grant, &net);
        if (grant) { set_expired(1); snprintf(msg, mn, "The Google sign-in has expired: sign in again"); r = ADR_EXPIRED; }
        else {
            snprintf(msg, mn, "Mounting Google Drive failed%s%s", lm[0] ? ": " : "", lm);
            if (automatic && net) { plat_lock(A.m); A.retry_at = plat_now_ms() + (int64_t)g_cfg.amidrive_retry_s * 1000; plat_unlock(A.m); }
            plat_lock(A.m); A.error = 1; plat_unlock(A.m);
        }
        ad_log("%s", msg); goto out;
    }
    for (int t = 0; t < 5000; t += 100) { char js[512]; if (rc_call("core/version", "{}", js, sizeof js) == 200) break; plat_sleep_ms(100); }
    if (strcmp(A.icon_live, A.icon_park) && !exists(A.icon_park) && !exists(A.icon_live) && A.icon_ship[0] && exists(A.icon_ship)) {
        /* first mount: the drawer icon shipped in the Host tarball (services/amidrive/Google Drive.info, copied
           byte-for-byte from the AmiDrive release); after that it lives parked in {state}/amidrive */
        char *ib = read_small(A.icon_ship, 256 * 1024); struct stat ist;
        if (ib && !stat(A.icon_ship, &ist)) { FILE *f = fopen(A.icon_park, "wb"); if (f) { fwrite(ib, 1, (size_t)ist.st_size, f); fclose(f); } }
        free(ib);
    }
    if (strcmp(A.icon_live, A.icon_park) && exists(A.icon_park) && !exists(A.icon_live)) {
        if (rename(A.icon_park, A.icon_live)) ad_log("cannot place the drawer icon: %s", strerror(errno)); }
    char acc[256]; read_line(A.accountf, acc, sizeof acc);
    plat_lock(A.m); snprintf(A.account, sizeof A.account, "%s", acc); A.mounted = 1; A.mounted_at = plat_now_ms(); A.crash_retry = 0; A.retry_at = 0;
    A.abandon_ok = 0; A.cache_dirty_n = A.cache_dirty_kb = 0; A.msg[0] = 0; plat_unlock(A.m);
    /* tab-separated, the Linux path last (it has a space): "amidrive\t1\t<nonce>\t<account or ->\t<linux path>\n" */
    { char mk[1200]; int k = snprintf(mk, sizeof mk, "amidrive\t1\t%s\t%s\t%s\n", nonce, acc[0] ? acc : "-", A.mp); plat_write_private(A.marker, mk, (size_t)(k < (int)sizeof mk ? k : (int)sizeof mk - 1)); }
    ad_log("mounted at \"%s\"", A.mp);
    snprintf(msg, mn, "Google Drive mounted"); r = ADR_OK;
out:
    plat_lock(A.m); A.busy_state = 0; if (r != ADR_OK && r != ADR_FAILED) A.error = 0; if (r != ADR_OK) snprintf(A.msg, sizeof A.msg, "%s", msg); plat_unlock(A.m);
    return r;
}
/* ---------- flush / unmount / sign out ---------- */
/* upload everything pending now; PROGRESS to s about once a second. 0 = queue empty, -1 = stalled/failed/deadline */
static int flush_wait(sess *s, int64_t deadline, char *msg, int mn) {
    int64_t last_change = plat_now_ms(); long long last_left = -1; int last_n = -1; int64_t last_prog = 0;
    for (;;) {
        queue_info q; if (rc_queue(&q)) { snprintf(msg, mn, "rclone on the Host PC is not answering"); return -1; }
        for (int i = 0; i < q.nids; i++) { char b[80]; snprintf(b, sizeof b, "{\"id\":%d,\"expiry\":0}", q.ids[i]); rc_call("vfs/queue-set-expiry", b, NULL, 0); }
        plat_lock(A.m); A.q_queued = (uint32_t)q.n; A.pending_kb = (uint32_t)((q.bytes + 1023) / 1024); plat_unlock(A.m);
        int64_t now = plat_now_ms();
        if (now - last_prog >= 1000 || q.n == 0) { s_progress(s, (uint32_t)q.n, (uint32_t)((q.bytes + 1023) / 1024)); last_prog = now; }
        if (q.n == 0) return 0;
        long long done = rc_bytes_done();
        long long left = q.bytes * 1000 + q.n - done;   /* any change (bytes sent, files done) counts as progress */
        if (left != last_left || q.n != last_n) { last_left = left; last_n = q.n; last_change = now; }
        if (now - last_change > 60000) { snprintf(msg, mn, "Uploads are not making progress (%d file%s left)", q.n, q.n == 1 ? "" : "s"); return -1; }
        if (deadline && now > deadline) { snprintf(msg, mn, "Out of time with %d upload%s pending", q.n, q.n == 1 ? "" : "s"); return -1; }
        if (A.stopping && !deadline) { snprintf(msg, mn, "The Host is stopping"); return -1; }
        plat_sleep_ms(250);
    }
}
static void note_cache_pending(const char *why) {
    uint32_t n, kb; cache_pending(&n, &kb);
    plat_lock(A.m); A.cache_dirty_n = n; A.cache_dirty_kb = kb; plat_unlock(A.m);
    if (n) ad_log("%s: %u file%s (%u KB) stay in cache/ and upload on the next mount", why, n, n == 1 ? "" : "s", kb);
}
static int unmount_op(sess *s, int mode, char *msg, int mn, char (*paths)[1024], int *np) {
    *np = 0; msg[0] = 0;
    plat_lock(A.m); int mounted = A.mounted; plat_unlock(A.m);
    if (!mounted && !is_mounted(A.mp)) { if (mode == 2) { plat_lock(A.m); A.abandon_ok = 1; plat_unlock(A.m); } cleanup_after(); snprintf(msg, mn, "Google Drive is not mounted"); return ADR_OK; }
    if (mode == 0) {
        int n = open_files(paths, MAXPATHS);
        if (n) { *np = n < MAXPATHS ? n : MAXPATHS; snprintf(msg, mn, "%d file%s open on Google Drive", n, n == 1 ? " is" : "s are"); return ADR_FILES_OPEN; }
    }
    int r = ADR_OK;
    int open_n = open_files(NULL, 0);
    if (mode != 2 && mounted) {
        plat_lock(A.m); A.busy_state = ADS_FLUSHING; plat_unlock(A.m);
        char fm[200];
        if (flush_wait(s, 0, fm, sizeof fm)) {
            if (mode == 0) { plat_lock(A.m); A.busy_state = 0; plat_unlock(A.m); snprintf(msg, mn, "Not unmounted: %s", fm); return ADR_PENDING; }
            ad_log("unmount later: %s; pending files stay in cache/", fm);
        }
    }
    plat_lock(A.m); A.busy_state = ADS_UNMOUNTING; plat_unlock(A.m);
    int u = umount_mp(mode == 1);
    if (u && mode == 0) {
        int n = open_files(paths, MAXPATHS);
        plat_lock(A.m); A.busy_state = 0; plat_unlock(A.m);
        if (n) { *np = n < MAXPATHS ? n : MAXPATHS; snprintf(msg, mn, "%d file%s open on Google Drive", n, n == 1 ? " is" : "s are"); return ADR_FILES_OPEN; }
        snprintf(msg, mn, "Unmounting failed (see amidrive.log on the Host PC)"); return ADR_FAILED;
    }
    if (u) { u = umount_mp(1); }
    int drain = 0;
    if (open_n && !u && (mode == 1 || mode == 2)) {   /* lazily detached: rclone keeps serving the open files, then exits */
        plat_lock(A.m); if (A.rpid > 0) { A.drain_pid = A.rpid; A.rpid = 0; A.expect_exit = 1; drain = 1; } plat_unlock(A.m);
    }
    if (!drain) stop_rclone(10000);
    if (u && is_mounted(A.mp)) { snprintf(msg, mn, "Unmounting failed (see amidrive.log on the Host PC)"); r = ADR_FAILED; }
    cleanup_after();
    if (drain) { char pb[32]; int k = snprintf(pb, sizeof pb, "%ld\n", (long)A.drain_pid); plat_write_private(A.pidf, pb, (size_t)k);
        ad_log("%d file%s still open: rclone serves %s until closed", open_n, open_n == 1 ? "" : "s", open_n == 1 ? "it" : "them"); }
    else note_cache_pending("unmounted");
    plat_lock(A.m); A.busy_state = 0; if (mode == 2) A.abandon_ok = 1; plat_unlock(A.m);
    if (r == ADR_OK) { ad_log("unmounted (mode %d)", mode); snprintf(msg, mn, "Google Drive unmounted"); }
    return r;
}
static int do_unmount(sess *s, int mode, char *msg, int mn, char (*paths)[1024], int *np) {
    pthread_mutex_lock(&A.opm); int r = unmount_op(s, mode, msg, mn, paths, np); pthread_mutex_unlock(&A.opm); return r;
}
static int flush_op(sess *s, char *msg, int mn) {
    plat_lock(A.m); int mounted = A.mounted; plat_unlock(A.m); int r;
    if (!mounted) { snprintf(msg, mn, "Google Drive is not mounted"); r = ADR_FAILED; }
    else {
        plat_lock(A.m); A.busy_state = ADS_FLUSHING; plat_unlock(A.m);
        r = flush_wait(s, 0, msg, mn) ? ADR_FAILED : ADR_OK; if (r == ADR_OK) snprintf(msg, mn, "All files are uploaded");
        plat_lock(A.m); A.busy_state = 0; plat_unlock(A.m);
    }
    return r;
}
static int revoke_head(void *ud, int st, long long len) { (void)len; *(int *)ud = st; return 0; }
static int revoke_body(void *ud, const unsigned char *b, size_t n) { (void)ud; (void)b; (void)n; return 0; }
static int allow_google(const char *h) { return !strcmp(h, "oauth2.googleapis.com"); }
static int conf_token_field(const char *field, char *out, int n) {   /* access_token / refresh_token from rclone.conf */
    char *b = read_small(A.conf, 65536); out[0] = 0; if (!b) return -1;
    char *t = strstr(b, "token = "); int r = -1;
    if (t) { char k[48]; snprintf(k, sizeof k, "\"%s\"", field); char *p = strstr(t, k);
        if (p) { p += strlen(k); while (*p == ':' || *p == ' ') p++; if (*p == '"') { p++; int o = 0; while (*p && *p != '"' && o < n - 1) out[o++] = *p++; out[o] = 0; r = 0; } } }
    memset(b, 0, strlen(b)); free(b); return r;
}
static int signout_op(sess *s, int revoke, char *msg, int mn, char (*paths)[1024], int *np) {
    int r;
    if (isdir(A.dir) && !take_lock(1)) { snprintf(msg, mn, "another AmiStudio Host is using this Google Drive"); r = ADR_BUSY; goto done; }
    r = unmount_op(s, 0, msg, mn, paths, np);
    if (r != ADR_OK) goto done;
    uint32_t n, kb; cache_pending(&n, &kb);
    plat_lock(A.m); int ab = A.abandon_ok; plat_unlock(A.m);
    if (n && !ab) { snprintf(msg, mn, "%u upload%s still waiting: connect and let them finish first", n, n == 1 ? " is" : "s are"); r = ADR_PENDING; goto done; }
    if (revoke) {
        char tok[2048]; if (!conf_token_field("refresh_token", tok, sizeof tok) && tok[0]) {
            char body[2200]; snprintf(body, sizeof body, "token=%s", tok); int st = 0; char err[200];
            int hs = http_req("POST", "https://oauth2.googleapis.com/revoke", "Content-Type: application/x-www-form-urlencoded\r\n", body, allow_google, revoke_head, revoke_body, &st, err, sizeof err);
            memset(body, 0, sizeof body); memset(tok, 0, sizeof tok);
            ad_log("revoke at Google: %s", hs == 200 ? "done" : "failed (the token is deleted here anyway)");
        }
    }
    unlink(A.conf); unlink(A.drawerf); unlink(A.accountf); set_expired(0); rm_rf(A.cache);
    plat_lock(A.m); A.account[0] = 0; A.copied = 0; A.abandon_ok = 0; A.cache_dirty_n = A.cache_dirty_kb = 0; A.error = 0; A.msg[0] = 0; A.retry_at = 0; plat_unlock(A.m);
    ad_log("signed out%s", revoke ? " (revoke requested)" : "");
    snprintf(msg, mn, revoke ? "Signed out (access revoked at Google)" : "Signed out"); r = ADR_OK;
done:
    return r;
}
/* ---------- sign-in: rclone authorize drive in the user's real desktop environment ---------- */
static int about_head(void *ud, int st, long long len) { (void)len; ((int *)ud)[0] = st; return 0; }
typedef struct { int st; char b[8192]; int n; } about_buf;
static int about_body(void *ud, const unsigned char *b, size_t n) { about_buf *a = ud; size_t c = n; if (c > sizeof a->b - 1 - (size_t)a->n) c = sizeof a->b - 1 - (size_t)a->n; memcpy(a->b + a->n, b, c); a->n += (int)c; a->b[a->n] = 0; return 0; }
static int allow_gapi(const char *h) { return !strcmp(h, "www.googleapis.com"); }
static void fetch_account(const char *access) {
    about_buf a; memset(&a, 0, sizeof a); char hdr[2300], err[200];
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\n", access);
    int st = http_req("GET", "https://www.googleapis.com/drive/v3/about?fields=user(emailAddress)", hdr, NULL, allow_gapi, about_head, about_body, &a, err, sizeof err);
    memset(hdr, 0, sizeof hdr);
    char em[256]; if (st == 200 && !js_str(a.b, "emailAddress", em, sizeof em) && em[0]) {
        char b[300]; int k = snprintf(b, sizeof b, "%s\n", em); plat_write_private(A.accountf, b, (size_t)k);
        plat_lock(A.m); snprintf(A.account, sizeof A.account, "%s", em); plat_unlock(A.m);
    } else ad_log("signed in; the account name could not be read (HTTP %d)", st);
}
static int write_conf(const char *token_json) {
    /* the remote is written directly (0600, atomic) instead of "rclone config create": that would put the token and
       the secret on a command line */
    size_t cap = strlen(token_json) + 1024; char *b = malloc(cap); int both = g_cfg.amidrive_client_id[0] && g_cfg.amidrive_client_secret[0];
    int n = snprintf(b, cap, "[gdrive]\ntype = drive\nscope = drive\n%s%s%s%s%s%stoken = %s\nteam_drive = \n",
                     both ? "client_id = " : "", both ? g_cfg.amidrive_client_id : "", both ? "\n" : "",
                     both ? "client_secret = " : "", both ? g_cfg.amidrive_client_secret : "", both ? "\n" : "", token_json);
    int r = plat_write_private(A.conf, b, (size_t)n); memset(b, 0, cap); free(b); return r;
}
/* the end of a sign-in: AUTH 2/3 and RESULT op 3 to every session that started or joined it, RESULT op 4 to the one that
   cancelled it, then STATE to the watchers */
static void push_watchers(int force);
static void auth_finish(int ok, const char *text) {
    sess *v[16]; int n = 0;
    plat_lock(A.m); A.auth_running = 0; A.apid = 0; sess *cb = A.auth_cancel_by; A.auth_cancel_by = NULL;
    for (sess *s = A.list; s && n < 16; s = s->next) if (s->auth_attached) { s->ref++; v[n++] = s; }
    plat_unlock(A.m);
    auth_broadcast(ok ? ADA_DONE : ADA_FAILED, text);
    for (int i = 0; i < n; i++) { s_result(v[i], AD_AUTH_START, ok ? ADR_OK : ADR_FAILED, text, NULL, 0); s_unref(v[i]); }
    if (cb) { s_result(cb, AD_AUTH_CANCEL, ADR_OK, "Sign-in cancelled", NULL, 0); s_unref(cb); }
    plat_lock(A.m); for (sess *s = A.list; s; s = s->next) s->auth_attached = s->auth_want = s->auth_url_sent = 0; A.auth_url[0] = 0; plat_unlock(A.m);
    push_watchers(1);
}
/* Before rclone and a browser start: ask Google's authorization endpoint whether it accepts this OAuth client with the
   redirect rclone uses (http://127.0.0.1:53682/). A client of the wrong type or without that redirect URI gets Google's
   "Error 400: redirect_uri_mismatch" page in every browser, on the PC as in AmiSurf; say so at once instead of opening
   a browser on an error page. The request carries the public client ID only (never the secret). Network trouble or an
   unexpected answer is not an error here: the sign-in goes on as before. 0 = go on, 1 = refused (why). */
typedef struct { char tail[96]; int tn; char code[48], num[4]; int found; } pf_scan;
static char pf_host[256];
static int pf_allow(const char *h) { return !strcmp(h, pf_host); }
static int pf_head(void *ud, int st, long long len) { (void)ud; (void)st; (void)len; return 0; }
static int pf_body(void *ud, const unsigned char *b, size_t n) {
    pf_scan *x = ud; size_t off = 0;
    while (off < n && !x->found) {   /* scan in pieces, keeping a tail so "Error 400: code" split across reads is found */
        char w[96 + 4096 + 1]; size_t k = n - off > 4096 ? 4096 : n - off;
        memcpy(w, x->tail, (size_t)x->tn); memcpy(w + x->tn, b + off, k); int wn = x->tn + (int)k; w[wn] = 0;
        for (int i = 0; i + 11 < wn && !x->found; i++) {
            if (memcmp(w + i, "Error 4", 7) || !isdigit((unsigned char)w[i + 7]) || !isdigit((unsigned char)w[i + 8]) || w[i + 9] != ':' || w[i + 10] != ' ') continue;
            int j = i + 11, o = 0; while (j < wn && o < (int)sizeof x->code - 1 && (islower((unsigned char)w[j]) || w[j] == '_')) x->code[o++] = w[j++];
            x->code[o] = 0;
            if (o >= 4 && j < wn) { memcpy(x->num, w + i + 6, 3); x->num[3] = 0; x->found = 1; }
            else if (j >= wn) break;   /* cut off: look again with more data */
        }
        int keep = wn < 64 ? wn : 64; memmove(x->tail, w + wn - keep, (size_t)keep); x->tn = keep; off += k;
    }
    return x->found;   /* found: stop reading */
}
static void url_enc(const char *in, char *out, int n) {
    int o = 0; for (; *in && o < n - 4; in++) { unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out[o++] = (char)c; else o += snprintf(out + o, (size_t)(n - o), "%%%02X", c); }
    out[o] = 0;
}
static int auth_preflight(char *why, int wn) {
    why[0] = 0;
    if (!g_cfg.amidrive_auth_check || !g_cfg.amidrive_client_id[0] || !g_cfg.amidrive_client_secret[0]) return 0;   /* rclone's own client: nothing of ours to check */
    const char *base = g_cfg.amidrive_auth_check_url[0] ? g_cfg.amidrive_auth_check_url : "https://accounts.google.com/o/oauth2/auth";
    if (strncmp(base, "https://", 8)) return 0;
    { const char *h = base + 8; int i = 0; while (h[i] && h[i] != '/' && h[i] != ':' && i < (int)sizeof pf_host - 1) { pf_host[i] = h[i]; i++; } pf_host[i] = 0; }
    char cid[400], url[1024], err[200]; url_enc(g_cfg.amidrive_client_id, cid, sizeof cid);
    snprintf(url, sizeof url, "%s?access_type=offline&client_id=%s&redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A53682%%2F&response_type=code"
             "&scope=https%%3A%%2F%%2Fwww.googleapis.com%%2Fauth%%2Fdrive&state=amistudio-host-check", base, cid);
    pf_scan x; memset(&x, 0, sizeof x);
    http_set_timeout(8000);   /* 0.1.10: a dead network or a silent server delays the sign-in by seconds, not 45 s */
    int st = http_get(url, pf_allow, pf_head, pf_body, &x, err, sizeof err);
    http_set_timeout(0);
    if (!x.found) { if (st < 0) ad_log("sign-in check: Google not reachable (%s): going on", err); return 0; }
    if (!strcmp(x.code, "redirect_uri_mismatch"))
        snprintf(why, (size_t)wn, "Google refuses this sign-in (Error %s: redirect_uri_mismatch). The AmiStudio Google client "
                 "needs fixing; nothing on this PC can solve it.", x.num);   /* texts stay well under 255 bytes (u8 strings) */
    else snprintf(why, (size_t)wn, "Google refuses this sign-in (Error %s: %s). Check amidrive_client_id and _secret in host.conf.", x.num, x.code);
    ad_log("sign-in check: Google answers Error %s: %s for this client: no browser opened", x.num, x.code);
    return 1;
}
static void auth_thread(void *arg) {
    int method = (int)(intptr_t)arg;
    if (!take_lock(1)) { auth_finish(0, "another AmiStudio Host is using this Google Drive"); return; }
    { char why[300]; if (auth_preflight(why, sizeof why)) { auth_finish(0, why); return; } }
    if (A.auth_cancel) { auth_finish(0, "Sign-in cancelled"); return; }
    /* 2 = for AmiSurf: URL only, like 1 (never a desktop browser nobody asked for). AmiSurf renders through a browser
       engine on this PC (this Host's "browser" service, or another engine on the same PC), so 127.0.0.1:53682 reaches
       rclone. AUTH 0 says whether this Host has an engine (ADAU_ENGINE). */
    int has_engine = service_find("browser") != NULL, engine = method == AD_AUTH_ENGINE;
    if (engine && !has_engine) ad_log("sign-in for AmiSurf: this Host has no browser engine (the URL works in any browser on this PC)");
    method = (method == AD_AUTH_URL_ONLY || engine) ? 1 : 0;
    /* the real desktop environment saved by run-host.sh before it moved HOME (AMISTUDIO_REAL_*) */
    static const char *keys[] = { "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", NULL };
    char *set[40]; int k = 0; char a[800], b[800], c[800];
    if (getenv("AMISTUDIO_REAL_ENV")) for (int i = 0; keys[i]; i++) {
        char nm[64]; snprintf(nm, sizeof nm, "AMISTUDIO_REAL_%s", keys[i]); const char *v = getenv(nm);
        char *e = malloc(900); if (v && *v) snprintf(e, 900, "%s=%s", keys[i], v); else snprintf(e, 900, "-%s", keys[i]); set[k++] = e;
    }
    snprintf(a, sizeof a, "RCLONE_CONFIG=%s", A.conf); snprintf(b, sizeof b, "RCLONE_CONFIG_DIR=%s", A.dir); snprintf(c, sizeof c, "RCLONE_CACHE_DIR=%s", A.cache);
    set[k++] = strdup(a); set[k++] = strdup(b); set[k++] = strdup(c);
    int both = g_cfg.amidrive_client_id[0] && g_cfg.amidrive_client_secret[0];
    char *cid = NULL, *csec = NULL;
    if (both) {   /* through the environment, never argv (rclone authorize reads RCLONE_DRIVE_CLIENT_ID/SECRET) */
        size_t l1 = strlen(g_cfg.amidrive_client_id) + 32, l2 = strlen(g_cfg.amidrive_client_secret) + 32;
        cid = malloc(l1); snprintf(cid, l1, "RCLONE_DRIVE_CLIENT_ID=%s", g_cfg.amidrive_client_id);
        csec = malloc(l2); snprintf(csec, l2, "RCLONE_DRIVE_CLIENT_SECRET=%s", g_cfg.amidrive_client_secret);
        set[k++] = cid; set[k++] = csec;
    } else { set[k++] = strdup("-RCLONE_DRIVE_CLIENT_ID"); set[k++] = strdup("-RCLONE_DRIVE_CLIENT_SECRET"); }
    set[k] = NULL;
    char **env = env_make((const char *const *)set);
    for (int i = 0; i < k; i++) { memset(set[i], 0, strlen(set[i])); free(set[i]); }
    char *argv[] = { A.rclone, "authorize", "drive", method == 1 ? "--auth-no-open-browser" : NULL, NULL };
    int p[2]; char buf[16384]; int o = 0; buf[0] = 0; int done = 0, ok = 0; char fail[256] = "";
    if (pipe2(p, O_CLOEXEC)) { env_free(env); auth_finish(0, "cannot start rclone"); return; }
    pid_t pid = spawn(argv, env, p[1], 1); close(p[1]); env_free(env);
    if (pid < 0) { close(p[0]); auth_finish(0, "cannot start rclone"); return; }
    plat_lock(A.m); A.apid = pid; plat_unlock(A.m);
    ad_log("sign-in started (%s)", engine ? "URL for AmiSurf" : method == 1 ? "URL only" : "desktop browser");
    int64_t t0 = plat_now_ms(), end = t0 + (int64_t)g_cfg.amidrive_auth_s * 1000; int url_sent = 0, opened_sent = 0, browser_failed = 0; char url[1024] = "";
    while (!done) {
        struct pollfd pf = { p[0], POLLIN, 0 }; int r = poll(&pf, 1, 200);
        if (r > 0) { ssize_t n = read(p[0], buf + o, sizeof buf - 1 - (size_t)o); if (n <= 0) done = 1; else { o += (int)n; buf[o] = 0; if (o > (int)sizeof buf - 512) { memmove(buf, buf + 4096, (size_t)o - 4096); o -= 4096; buf[o] = 0; } } }
        if (!url[0]) { char *u = strstr(buf, "http://127.0.0.1:53682/auth"); if (u) { int i = 0; while (u[i] && u[i] > ' ' && i < (int)sizeof url - 1) { url[i] = u[i]; i++; } url[i] = 0; } }
        if (strstr(buf, "Failed to open browser")) browser_failed = 1;
        if (url[0] && !url_sent) {   /* the URL: to everyone in a URL sign-in (or when no desktop browser opened), else to joiners that asked for it */
            int fl = (has_engine ? ADAU_ENGINE : 0) | (method == 0 && browser_failed ? ADAU_NO_DESKTOP : 0);
            plat_lock(A.m); snprintf(A.auth_url, sizeof A.auth_url, "%s", url); A.auth_url_flags = fl; plat_unlock(A.m);
            if (method == 1 || browser_failed) { auth_send(ADA_URL, url, fl, AUTH_TO_ALL); url_sent = 1; }
            else auth_send(ADA_URL, url, fl, AUTH_TO_URL);
        }
        if (url[0] && method == 0 && !opened_sent && !browser_failed && plat_now_ms() - t0 > 1500) { auth_send(ADA_OPENED, "Sign in in the browser on the Host PC", 0, AUTH_TO_DESKTOP); opened_sent = 1; }
        char *s = strstr(buf, "--->"), *e = s ? strstr(s, "<---End paste") : NULL;
        if (s && e) {
            s += 4; while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
            char *t = e; while (t > s && (t[-1] == ' ' || t[-1] == '\n' || t[-1] == '\r' || t[-1] == '\t')) t--;
            size_t tl = (size_t)(t - s); char *tok = malloc(tl + 1); memcpy(tok, s, tl); tok[tl] = 0;
            if (tok[0] == '{' && strstr(tok, "access_token")) {
                if (!write_conf(tok)) {
                    char acc[2048]; js_str(tok, "access_token", acc, sizeof acc);
                    write_drawer_path(); set_expired(0); unlink(A.accountf);
                    plat_lock(A.m); A.copied = 0; A.error = 0; A.msg[0] = 0; A.account[0] = 0; plat_unlock(A.m);
                    if (acc[0]) fetch_account(acc);
                    memset(acc, 0, sizeof acc);
                    ok = 1;
                } else snprintf(fail, sizeof fail, "cannot write rclone.conf");
            } else snprintf(fail, sizeof fail, "rclone returned no token");
            memset(tok, 0, tl); free(tok); done = 1;
        }
        int st; if (!done && waitpid(pid, &st, WNOHANG) == pid) { pid = 0; done = 1; if (!fail[0]) snprintf(fail, sizeof fail, "sign-in failed (rclone ended)"); }
        if (A.auth_cancel) { snprintf(fail, sizeof fail, "Sign-in cancelled"); done = 1; }
        if (A.stopping) { snprintf(fail, sizeof fail, "The Host is stopping"); done = 1; }
        if (!done && plat_now_ms() > end) { snprintf(fail, sizeof fail, "sign-in timed out"); done = 1; }
    }
    memset(buf, 0, sizeof buf);
    close(p[0]);
    if (pid > 0) { kill(pid, SIGTERM); if (!wait_pid(pid, 3000)) { kill(pid, SIGKILL); wait_pid(pid, 2000); } }   /* rclone only, never the browser */
    if (ok) {
        ad_log("signed in%s%s", A.account[0] ? " as " : "", A.account);
        /* still mounted on an expired token: remount with the new one when nothing is open */
        plat_lock(A.m); int mounted = A.mounted; plat_unlock(A.m);
        if (mounted && !open_files(NULL, 0)) {
            char m[200], (*paths)[1024] = malloc(sizeof(char[1024]) * MAXPATHS); int np;
            if (do_unmount(NULL, 1, m, sizeof m, paths, &np) == ADR_OK) do_connect(0, m, sizeof m);
            free(paths);
        }
        { char t[300]; snprintf(t, sizeof t, "Signed in%s%s", A.account[0] ? " as " : "", A.account); auth_finish(1, t); }
    } else { ad_log("%s", fail); auth_finish(0, fail); }
}
/* ---------- requests ---------- */
static void worker(void *arg) {
    sess *s = arg; int op = s->op, a = s->arg; char msg[256] = ""; int st = ADR_OK;
    char (*paths)[1024] = calloc(MAXPATHS, 1024); int np = 0;
    /* CONNECT/UNMOUNT/FLUSH/SIGNOUT from any client run one at a time; one that arrives while another runs (or while
       the Host's monitor is remounting) is answered RESULT 4 BUSY at once, not queued */
    if (pthread_mutex_trylock(&A.opm)) {
        plat_lock(A.m); int bs = A.busy_state; plat_unlock(A.m);
        st = ADR_BUSY; snprintf(msg, sizeof msg, "%s", bs == ADS_MOUNTING ? "Google Drive is being mounted" : bs == ADS_FLUSHING ? "uploads are being flushed"
                                                       : bs == ADS_UNMOUNTING ? "Google Drive is being unmounted" : "another request is running");
    } else {
        switch (op) {
        case AD_CONNECT: st = connect_locked_op(a & AD_CONNECT_AUTO, msg, sizeof msg);
            if ((a & AD_CONNECT_AUTO) && st == ADR_NOT_SIGNED_IN) msg[0] = 0;   /* automatic: status 3 with no message (nothing to show) */
            break;
        case AD_UNMOUNT: st = unmount_op(s, a > 2 ? 0 : a, msg, sizeof msg, paths, &np); break;
        case AD_FLUSH: st = flush_op(s, msg, sizeof msg); break;
        case AD_SIGNOUT: st = signout_op(s, a & AD_SIGNOUT_REVOKE, msg, sizeof msg, paths, &np); break;
        }
        pthread_mutex_unlock(&A.opm);
    }
    s_result(s, op, st, msg, paths, np); free(paths);
    s_state(s);
    plat_lock(A.m); s->worker = 0; plat_unlock(A.m);
    s_unref(s);
}
static void handle(sess *s, int op, const unsigned char *p, uint32_t n) {
    int a = n ? p[0] : 0;
    switch (op) {
    case AD_STATUS: s_state(s); return;   /* one STATE, no RESULT (spec) */
    case AD_WATCH: plat_lock(A.m); s->watch = a ? 1 : 0; plat_unlock(A.m); s_state(s); return;   /* the current STATE, no RESULT */
    case AD_AUTH_START: {
        if (!A.rclone_ok) { s_result(s, op, ADR_FAILED, "rclone is missing from the Host", NULL, 0); return; }
        int want = a == AD_AUTH_URL_ONLY || a == AD_AUTH_ENGINE ? a : AD_AUTH_DESKTOP;
        plat_lock(A.m); int running = A.auth_running; s->auth_attached = 1; s->auth_want = want; s->auth_url_sent = 0;
        int step = A.auth_step, uflags = A.auth_url_flags; char text[1100], url[1024]; snprintf(text, sizeof text, "%s", A.auth_text); snprintf(url, sizeof url, "%s", A.auth_url);
        if (running && want && url[0]) s->auth_url_sent = 1;
        if (!running) { A.auth_running = 1; A.auth_cancel = 0; A.auth_step = -1; A.auth_text[0] = 0; A.auth_url[0] = 0; A.auth_url_flags = 0; }
        plat_unlock(A.m);
        if (running) {   /* join the sign-in that is already running: its AUTH steps and final RESULT come here too. A joiner
                            that asks for the URL (1/2) gets AUTH 0 even when the sign-in opened the desktop browser */
            fbuf f;
            if (want && url[0]) { auth_frame(&f, ADA_URL, url, uflags); s_send(s, AD_AUTH, f.b, (uint32_t)f.n); }
            else if (want && step == ADA_OPENED) { /* the URL is not read yet: it comes with AUTH_TO_URL */ }
            else if (step >= 0) { auth_frame(&f, step, text, step == ADA_URL ? uflags : 0); s_send(s, AD_AUTH, f.b, (uint32_t)f.n); }
            return;
        }
        if (plat_thread(auth_thread, (void *)(intptr_t)want)) { plat_lock(A.m); A.auth_running = 0; plat_unlock(A.m); s_result(s, op, ADR_FAILED, "cannot start the sign-in", NULL, 0); return; }
        s_state(s); return; }   /* RESULT op 3 comes with AUTH 2/3 when the sign-in ends */
    case AD_AUTH_CANCEL: {
        plat_lock(A.m); int running = A.auth_running, mine = 0;
        if (running) { A.auth_cancel = 1; if (!A.auth_cancel_by) { A.auth_cancel_by = s; s->ref++; mine = 1; } }
        plat_unlock(A.m);
        if (!running) s_result(s, op, ADR_FAILED, "No sign-in is running", NULL, 0);
        else if (!mine) s_result(s, op, ADR_OK, "Sign-in cancelled", NULL, 0);
        return; }
    case AD_CONNECT: case AD_UNMOUNT: case AD_FLUSH: case AD_SIGNOUT: {
        plat_lock(A.m); int busy = s->worker; if (!busy) { s->worker = 1; s->ref++; s->op = op; s->arg = a; } plat_unlock(A.m);
        if (busy) { s_result(s, op, ADR_BUSY, "another request of this session is still running", NULL, 0); return; }
        if (plat_thread(worker, s)) { plat_lock(A.m); s->worker = 0; s->ref--; plat_unlock(A.m); s_result(s, op, ADR_FAILED, "cannot start", NULL, 0); }
        return; }
    default: return;   /* unknown ops are ignored */
    }
}
static void ad_run(service *sv, conn *c) {
    (void)sv;
    sess *s = calloc(1, sizeof *s); s->c = c; s->wm = plat_mutex_new(); s->ref = 1;
    plat_lock(A.m); s->next = A.list; A.list = s; A.sessions++; A.retry_at = A.retry_at; plat_unlock(A.m);
    for (;;) {
        int op; unsigned char *p = NULL; uint32_t n;
        if (frame_recv(c->s, &op, &p, &n, 4096)) { free(p); break; }
        handle(s, op, p, n); free(p);
    }
    plat_lock(s->wm); s->dead = 1; plat_unlock(s->wm);
    plat_lock(A.m);
    for (sess **pp = &A.list; *pp; pp = &(*pp)->next) if (*pp == s) { *pp = s->next; break; }
    A.sessions--; s->auth_attached = 0;
    int last = A.sessions == 0, auth = A.auth_running; int any_attached = 0; for (sess *q = A.list; q; q = q->next) if (q->auth_attached) any_attached = 1;
    plat_unlock(A.m);
    if (auth && !any_attached && last) { plat_lock(A.m); A.auth_cancel = 1; plat_unlock(A.m); }   /* nobody is left to finish the sign-in */
    s_unref(s);
}
/* ---------- monitor ---------- */
static void push_watchers(int force) {
    fbuf f; f.n = 0; state_frame(&f); unsigned h = hash_bytes(f.b, f.n); int64_t now = plat_now_ms();
    sess *v[16]; int n = 0;
    plat_lock(A.m);
    for (sess *s = A.list; s && n < 16; s = s->next) if (s->watch && (force || s->last_hash != h) && now - s->last_push >= 500) { s->ref++; v[n++] = s; s->last_hash = h; s->last_push = now; }
    plat_unlock(A.m);
    for (int i = 0; i < n; i++) { s_send(v[i], AD_STATE, f.b, (uint32_t)f.n); s_unref(v[i]); }
}
static void check_grant_log(void) {
    plat_lock(A.m); long off = A.rlog_off; plat_unlock(A.m);
    long sz = file_size(A.rlogf); if (sz < off) off = 0; if (sz == off) return;
    char lm[300]; int grant, net; log_since(off, lm, sizeof lm, &grant, &net);
    plat_lock(A.m); A.rlog_off = sz; plat_unlock(A.m);
    if (grant && !A.expired) { set_expired(1); setmsg("The Google sign-in has expired: sign in again"); ad_log("rclone reports invalid_grant: sign-in expired (mount and pending uploads kept)"); }
}
static void on_crash(const char *why) {
    ad_log("%s: lazy unmount", why);
    pthread_mutex_lock(&A.opm);
    umount_mp(1); stop_rclone(2000); cleanup_after(); note_cache_pending("after the crash");
    plat_lock(A.m); A.error = 1; snprintf(A.msg, sizeof A.msg, "%s", why); int retry = !A.crash_retry && A.sessions > 0 && !A.expired; if (retry) A.crash_retry = 1; plat_unlock(A.m);
    pthread_mutex_unlock(&A.opm);
    if (retry && !A.stopping) {
        char m[256]; ad_log("retrying the mount once");
        int r = do_connect(0, m, sizeof m); plat_lock(A.m); A.crash_retry = 1; plat_unlock(A.m);
        if (r != ADR_OK) { plat_lock(A.m); A.error = 1; plat_unlock(A.m); }
    }
}
static void monitor(void *arg) {
    (void)arg; int64_t t_stats = 0, t_quota = 0, t_open = 0, t_slow = 0;
    while (!A.stopping) {
        plat_sleep_ms(250); if (A.stopping) break;
        int64_t now = plat_now_ms();
        plat_lock(A.m); pid_t p = A.rpid; int mounted = A.mounted, expect = A.expect_exit, bs = A.busy_state; plat_unlock(A.m);
        plat_lock(A.m); pid_t dp = A.drain_pid; plat_unlock(A.m);
        if (dp > 0) { int st; if (waitpid(dp, &st, WNOHANG) == dp) {
            plat_lock(A.m); A.drain_pid = 0; plat_unlock(A.m); unlink(A.pidf); ad_log("the last open file was closed: rclone ended");
            note_cache_pending("after the open files closed"); } }
        if (mounted && p > 0 && !bs && pthread_mutex_trylock(&A.opm) == 0) {
            int st; int gone = waitpid(p, &st, WNOHANG) == p;
            if (gone) { plat_lock(A.m); A.rpid = 0; plat_unlock(A.m); }
            pthread_mutex_unlock(&A.opm);
            if (gone && !expect) { on_crash("rclone exited"); continue; }
        }
        if (mounted && now - t_stats >= 1000) {
            t_stats = now; queue_info q;
            if (!rc_queue(&q)) {
                char js[4096]; double ip = 0, er = 0;
                if (rc_call("core/stats", "{}", js, sizeof js) == 200) { js_num(js, "transfers", &er); er = 0; js_num(js, "errors", &er);
                    const char *tr = strstr(js, "\"transferring\""); ip = 0; if (tr) { const char *e = strchr(tr, ']'); for (const char *x = tr; x && (!e || x < e); x = strstr(x + 1, "\"name\"")) if (x != tr) ip++; } }
                plat_lock(A.m); A.q_queued = (uint32_t)q.n; A.q_inprog = (uint32_t)ip; A.errored = (uint32_t)er; A.pending_kb = (uint32_t)((q.bytes + 1023) / 1024); A.rc_fail = 0; plat_unlock(A.m);
            } else {
                plat_lock(A.m); int f = ++A.rc_fail; plat_unlock(A.m);
                if (f >= 10 && !bs) { on_crash("rclone stopped answering"); continue; }
            }
            check_grant_log();
        }
        if (mounted && now - t_quota >= 60000) {
            t_quota = now; char js[4096]; double u = 0, t = 0;
            if (rc_call("operations/about", "{\"fs\":\"gdrive:\"}", js, sizeof js) == 200) { js_num(js, "used", &u); js_num(js, "total", &t); plat_lock(A.m); A.quota_used = (uint64_t)u; A.quota_total = (uint64_t)t; plat_unlock(A.m); }
        }
        if (mounted && now - t_open >= 2000) { t_open = now; int n = open_files(NULL, 0); plat_lock(A.m); A.open_files = (uint32_t)n; plat_unlock(A.m); }
        if (now - t_slow >= 10000) {
            t_slow = now; struct statvfs vf; int low = 0;
            if (!statvfs(A.dir, &vf)) low = (uint64_t)vf.f_bavail * vf.f_frsize < 1024ull * 1024 * 1024;
            plat_lock(A.m); A.lowdisk = low; plat_unlock(A.m);
            if (!mounted && !bs) { uint32_t n, kb; cache_pending(&n, &kb); plat_lock(A.m); A.cache_dirty_n = n; A.cache_dirty_kb = kb; plat_unlock(A.m);
                if (A.copied && signed_in() && !drawer_check()) { plat_lock(A.m); A.copied = 0; plat_unlock(A.m); } }
        }
        plat_lock(A.m); int64_t ra = A.retry_at; int sessions = A.sessions; plat_unlock(A.m);
        if (ra && now >= ra && !mounted && !bs) {
            plat_lock(A.m); A.retry_at = 0; plat_unlock(A.m);
            if (sessions > 0 && !A.expired && !A.copied && !A.notempty) { char m[256]; ad_log("automount retry"); do_connect(1, m, sizeof m); }
        }
        plat_lock(A.m); A.busy = g_cfg.amidrive_keeps_host && (A.mounted || A.busy_state); plat_unlock(A.m);
        push_watchers(0);
    }
}
/* ---------- service hooks ---------- */
static int ad_busy(service *sv) { (void)sv; return A.busy; }
static void ad_status(service *sv, char *out, int n) {
    (void)sv;
    if (!A.rclone_ok) { snprintf(out, n, "down no-rclone"); return; }
    if (!A.fuse_ok) { snprintf(out, n, "down no-fuse"); return; }
    plat_lock(A.m); int st = state_locked(1); plat_unlock(A.m);
    (void)st; snprintf(out, n, "up");
}
static void ad_init(service *sv) {
    (void)sv;
    snprintf(A.dir, sizeof A.dir, "%s/amidrive", g_cfg.data_dir);
#define P(f, name) snprintf(A.f, sizeof A.f, "%s/%s", A.dir, name)
    P(conf, "rclone.conf"); P(cache, "cache"); P(icon_park, "Google Drive.info"); P(marker, "Drive.mount"); P(pidf, "rclone.pid");
    P(drawerf, "drawer.path"); P(logf, "amidrive.log"); P(rlogf, "rclone.log"); P(expiredf, "signin.expired"); P(accountf, "account");
#undef P
    if (g_cfg.amidrive_mountpoint[0]) {
        if (g_cfg.amidrive_mountpoint[0] == '/') snprintf(A.mp, sizeof A.mp, "%s", g_cfg.amidrive_mountpoint);
        else { char cwd[400]; if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/"); snprintf(A.mp, sizeof A.mp, "%s/%s", cwd, g_cfg.amidrive_mountpoint); }
    } else snprintf(A.mp, sizeof A.mp, "%s/Google Drive", A.dir);
    size_t ml = strlen(A.mp); while (ml > 1 && A.mp[ml - 1] == '/') A.mp[--ml] = 0;
    { /* resolve the parent (the AmiStudio drawer), keep the last name as given */
        char par[700], rp[700]; snprintf(par, sizeof par, "%s", A.mp); char *sl = strrchr(par, '/');
        if (sl && sl != par) { *sl = 0; if (realpath(par, rp)) { char t[720]; snprintf(t, sizeof t, "%s/%s", rp, sl + 1); snprintf(A.mp, sizeof A.mp, "%s", t); } }
    }
    snprintf(A.icon_live, sizeof A.icon_live, "%s.info", A.mp);
    { const char *last = strrchr(A.mp, '/'); const char *prev = last; if (last && last != A.mp) { prev = last - 1; while (prev > A.mp && *prev != '/') prev--; if (*prev == '/') prev++; }
      snprintf(A.mpname, sizeof A.mpname, "%s", prev ? prev : A.mp); }
    if (g_cfg.amidrive_rclone[0]) snprintf(A.rclone, sizeof A.rclone, "%s", g_cfg.amidrive_rclone);
    else { char ed[600]; plat_exe_dir(ed, sizeof ed); snprintf(A.rclone, sizeof A.rclone, "%s/engine/rclone/rclone", ed); }
    { char ed[600]; if (!plat_exe_dir(ed, sizeof ed)) snprintf(A.icon_ship, sizeof A.icon_ship, "%s/services/amidrive/Google Drive.info", ed); }
    snprintf(A.fusermount, sizeof A.fusermount, "%s", g_cfg.amidrive_fusermount[0] ? g_cfg.amidrive_fusermount : "fusermount3");
    char fm[600]; A.rclone_ok = access(A.rclone, X_OK) == 0;
    A.fuse_ok = access("/dev/fuse", R_OK | W_OK) == 0 && find_exe(A.fusermount, fm, sizeof fm) == 0;
    if (A.fuse_ok) snprintf(A.fusermount, sizeof A.fusermount, "%s", fm);
    A.expired = exists(A.expiredf);
    read_line(A.accountf, A.account, sizeof A.account);
    ad_log("ready: mount point \"%s\"%s%s", A.mp, A.rclone_ok ? "" : " (rclone missing: down no-rclone)", A.fuse_ok ? "" : " (FUSE missing: down no-fuse)");
    if (take_lock(0)) stale_clear("Host start");
    else if (isdir(A.dir)) ad_log("another Host holds %s/amidrive.lock: leaving its mount alone", A.dir);
    note_cache_pending("Host start");
    /* no automount here: CONNECT flag 1 from AmiStudio/AmiDrive mounts */
    plat_thread(monitor, NULL);
}
static void ad_stop(service *sv) {
    (void)sv;
    A.stopping = 1;
    plat_lock(A.m); if (A.auth_running) A.auth_cancel = 1; pid_t ap = A.apid; plat_unlock(A.m);
    if (ap > 0) kill(ap, SIGTERM);
    int64_t deadline = plat_now_ms() + (int64_t)(g_cfg.amidrive_stop_s > 0 ? g_cfg.amidrive_stop_s : 1) * 1000;
    /* an op in progress (connect/flush) ends on A.stopping; wait for it within the budget */
    while (pthread_mutex_trylock(&A.opm)) { if (plat_now_ms() > deadline) { ad_log("stop: an operation is still running; unmounting anyway"); goto force; } plat_sleep_ms(50); }
    plat_lock(A.m); int mounted = A.mounted; plat_unlock(A.m);
    if (mounted || (g_lockfd >= 0 && is_mounted(A.mp))) {   /* never another Host's mount */
        plat_lock(A.m); A.busy_state = ADS_FLUSHING; plat_unlock(A.m);
        char m[200];
        if (mounted && flush_wait(NULL, deadline, m, sizeof m)) ad_log("stop: %s", m);
        plat_lock(A.m); A.busy_state = ADS_UNMOUNTING; plat_unlock(A.m);
        if (umount_mp(0)) umount_mp(1);
        stop_rclone(5000); cleanup_after(); note_cache_pending("Host stop");
        ad_log("stop: unmounted");
        plat_lock(A.m); A.busy_state = 0; plat_unlock(A.m);
    }
    pthread_mutex_unlock(&A.opm);
    goto end;
force:
    if (g_lockfd >= 0 && umount_mp(0)) umount_mp(1);
    stop_rclone(3000); cleanup_after(); note_cache_pending("Host stop");
end:
    { pid_t dp; plat_lock(A.m); dp = A.drain_pid; plat_unlock(A.m);
      if (dp > 0) {
          while (!wait_pid(dp, 100) && plat_now_ms() < deadline) continue;
          if (!wait_pid(dp, 0)) { ad_log("stop: files are still open after the lazy unmount; stopping rclone"); kill(dp, SIGTERM); if (!wait_pid(dp, 5000)) { kill(-dp, SIGKILL); kill(dp, SIGKILL); wait_pid(dp, 2000); } }
          plat_lock(A.m); A.drain_pid = 0; plat_unlock(A.m); unlink(A.pidf); note_cache_pending("Host stop"); } }
    for (int i = 0; i < 40; i++) { plat_lock(A.m); int a = A.auth_running; plat_unlock(A.m); if (!a) break; plat_sleep_ms(100); }
}
service *svc_amidrive_new(void) {
    service *sv = calloc(1, sizeof *sv); snprintf(sv->name, sizeof sv->name, "amidrive");
    sv->vmin = sv->vmax = 1; sv->need_auth = 1; sv->max_clients = 4;
    sv->run = ad_run; sv->status = ad_status; sv->stop = ad_stop; sv->busy = ad_busy; sv->init = ad_init;
    A.m = plat_mutex_new(); pthread_mutex_init(&A.opm, NULL);
    return sv;
}
#endif
