/* AmiStudio Host - platform layer. Everything OS-specific lives behind this header:
 * plat_posix.c (Linux, later macOS) and plat_win32.c (Windows, required before Host 1.0).
 * The rest of the Host is plain C99 and only calls these functions for sockets, threads, processes,
 * time, randomness and paths. */
#ifndef ASH_PLAT_H
#define ASH_PLAT_H
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#else
typedef int sock_t;
#define SOCK_BAD (-1)
#endif

/* ---- sockets ---- */
int     plat_net_init(void);
sock_t  plat_listen_tcp(const char *addr, int port, char *err, int en);     /* addr "127.0.0.1" / "0.0.0.0" */
sock_t  plat_bind_udp(const char *addr, int port, char *err, int en);
sock_t  plat_accept(sock_t l, char *peer, int pn, int *loopback);          /* peer "a.b.c.d" */
sock_t  plat_connect_tcp(const char *host, int port, int timeout_ms);      /* host name or address */
int     plat_free_port(void);                                               /* a free loopback TCP port */
void    plat_close(sock_t s);
void    plat_shutdown(sock_t s);                                          /* wake up a thread blocked on s (it closes it) */
long    plat_send(sock_t s, const void *b, size_t n);
long    plat_recv(sock_t s, void *b, size_t n);
int     plat_send_all(sock_t s, const void *b, size_t n);                  /* 0 ok */
int     plat_recv_all(sock_t s, void *b, size_t n);                        /* 0 ok, -1 error, -2 closed */
int     plat_set_timeout(sock_t s, int ms);                                 /* recv+send timeout, 0 = none */
void    plat_nodelay(sock_t s);
/* wait until a or b is readable: returns bitmask 1 (a), 2 (b), 0 timeout, -1 error */
int     plat_wait2(sock_t a, sock_t b, int timeout_ms);
long    plat_recvfrom(sock_t s, void *b, size_t n, char *peer, int pn, void *addr, int *alen);
long    plat_sendto(sock_t s, const void *b, size_t n, const void *addr, int alen);
int     plat_wait_readable(sock_t s, int timeout_ms);                       /* 1 readable, 0 timeout, -1 error */

/* ---- threads ---- */
typedef struct plat_mutex plat_mutex;
plat_mutex *plat_mutex_new(void);
void    plat_lock(plat_mutex *m);
void    plat_unlock(plat_mutex *m);
int     plat_thread(void (*fn)(void *), void *arg);                         /* detached; 0 ok */

/* ---- processes (service backends run as children) ---- */
typedef struct { long pid; void *h; void *job; } plat_proc;   /* h/job: Windows process + job handles */
/* argv is NULL terminated. log_path: child's stdout/stderr appended there (NULL = inherit). 0 ok */
int     plat_spawn(char *const argv[], const char *log_path, plat_proc *p, char *err, int en);
int     plat_proc_alive(plat_proc *p);                                      /* reaps if dead */
/* 1 running; 0 it has exited and was reaped now (*how: exit code, or -signal); -1 no process. Non-blocking. */
int     plat_proc_reap(plat_proc *p, int *how);
void    plat_proc_kill(plat_proc *p);                                       /* terminate, wait a moment, force */
/* 0.1.4 graceful stop: ask the backend itself first (SIGTERM to it only, so it can shut its browser down cleanly and
   Chrome writes its profile), wait up to ms, then the whole process group as plat_proc_kill does */
void    plat_proc_stop(plat_proc *p, int ms);

/* ---- misc ---- */
int64_t plat_now_ms(void);                                                  /* monotonic */
int64_t plat_time(void);                                                    /* unix seconds */
void    plat_fmt_time(int64_t t, char *out, int n);                         /* local "YYYY-MM-DD HH:MM" */
void    plat_sleep_ms(int ms);
int     plat_random(void *b, size_t n);                                     /* CSPRNG, 0 ok */
int     plat_config_dir(char *out, int n);                                  /* ~/.config/amistudio-host, %APPDATA%\AmiStudio-Host */
int     plat_setenv(const char *name, const char *value);                  /* for child processes (services) */
int     plat_data_dir(char *out, int n);                                    /* ~/.local/share/amistudio-host, %LOCALAPPDATA%\AmiStudio-Host */
int     plat_mkdir_private(const char *path);                               /* 0700 (owner only) */
int     plat_write_private(const char *path, const void *b, size_t n);     /* atomic replace, 0600 */
const char *plat_os_name(void);                                             /* "linux-x86_64" */
int     plat_hostname(char *out, int n);
int     plat_exe_dir(char *out, int n);                                     /* folder of the running binary (symlinks resolved) */
double  plat_load(void);                                                    /* 1-minute load or -1 */
void    plat_on_signal(void (*stop)(void));                                 /* SIGINT/SIGTERM or console close */
#endif
