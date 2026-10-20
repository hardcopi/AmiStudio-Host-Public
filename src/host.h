/* AmiStudio Host - shared state, configuration and the service interface. */
#ifndef ASH_HOST_H
#define ASH_HOST_H
#include "plat.h"
#include "ash.h"
#include "util.h"
#define HOST_VERSION "0.1.10"
#define MAX_SERVICES 32

typedef struct {
    sock_t s;
    char peer[64];
    int loopback;              /* 127.x: trusted without a token, as ASBC does today */
    int authed;                /* loopback or a valid paired token */
    char token_id[16];         /* which paired token (for logs / revoke) */
    char client[ASH_MAX_CLIENT + 1];
    char service[ASH_MAX_NAME + 1];
    unsigned want_ver, svc_ver;
    sock_t backend;            /* svc_proxy: connection to the child backend (SOCK_BAD if none) */
    int kicked;                /* 0.1.4: replaced by a reconnect of the same client (guarded by g_lock) */
} conn;

typedef struct service service;
struct service {
    char name[ASH_MAX_NAME + 1];
    unsigned vmin, vmax;       /* protocol versions this service speaks */
    int need_auth;             /* 1: LAN clients need a paired token (everything except hostinfo/pair) */
    int max_clients;           /* 0 = unlimited; more -> ASH_BUSY */
    int active;                /* live sessions (guarded by g_lock) */
    /* before the OK reply (may start a backend). Returns ASH_OK or a status; msg is sent with the reply. */
    int  (*prepare)(service *sv, conn *c, char *msg, int mn);
    /* after the OK reply: the service owns c->s until it returns (the caller closes it) */
    void (*run)(service *sv, conn *c);
    void (*status)(service *sv, char *out, int n);      /* "up", "idle", "down (why)" */
    void (*stop)(service *sv);                          /* host shutting down (0.1.8: all services stop in parallel) */
    /* 0.1.8 hooks (optional): busy() = 1 keeps the Host up with no client (counted like an app session in
       quit-when-unused and exit_idle, and shown to run-host.sh's watcher as CONFDIR/busy); init() runs once after the
       config is read, before the port opens (amidrive: clear a stale mount, wire automount) */
    int  (*busy)(service *sv);
    void (*init)(service *sv);
    void *priv;
};

typedef struct {
    char confdir[512];
    char data_dir[512];        /* {state} in exec lines: ~/.local/share/amistudio-host (data_dir, --data-dir) */
    char listen[64];           /* "127.0.0.1" or "0.0.0.0" (--lan) */
    int  port, lan, discovery, update_check;
    char name[64];             /* shown in discovery and hostinfo (default: hostname) */
    char id[20];               /* stable host id (random, stored in confdir/host-id) */
    char fetch_allow[1024];    /* space separated host names; "*" = any https host */
    long fetch_max;            /* bytes */
    char cafile[512];          /* extra CA bundle for fetch (tests) */
    char manifest_url[512];    /* for the Host's own update check */
    char update_ver[24];       /* newer Host version seen in the manifest, or "" */
    int  bundled_browser;      /* 1: mount INSTALLDIR/engine/engine.sh as "browser" if host.conf has no browser service */
    int  browser_idle;         /* minutes without a client before the bundled engine quits (0 = never) */
    int  bundled_amidiscord;   /* 1: mount INSTALLDIR/engine/engine.sh in app mode as "amidiscord" (0.1.4) */
    int  amidiscord_idle;      /* minutes Chrome stays up (logged in, notifying) after the last AmiDiscord client left */
    int  allow_quit;           /* 0.1.6: control QUIT_WHEN_UNUSED honoured (allow_quit no / --no-remote-quit: never armed) */
    int64_t exit_idle_ms;      /* Host exits after this long with no client connection open or opened (0 = never) */
    /* 0.1.8 amidrive (Google Drive through rclone, svc_amidrive.c, docs/SERVICES.md) */
    int  amidrive;             /* 0 = no amidrive service (amidrive no) */
    char amidrive_mountpoint[512];   /* "" = {state}/amidrive/Google Drive; the launcher's run-host.sh: <AmiStudio>/Google Drive */
    char amidrive_rclone[512];       /* "" = INSTALLDIR/engine/rclone/rclone */
    char amidrive_flags[512];        /* extra / overriding rclone mount flags */
    char amidrive_client_id[160], amidrive_client_secret[160];   /* both empty = rclone's built-in client ID */
    char amidrive_fusermount[256];   /* default fusermount3 (tests point it at a failing stand-in) */
    int  amidrive_automount;   /* 1: CONNECT flag 1 (AmiStudio/AmiDrive start) mounts */
    int  amidrive_keeps_host;  /* 1: a mount alone keeps the Host up (busy); default 0 */
    int  amidrive_stop_s;      /* Host stop: flush/unmount budget (default 60) */
    int  amidrive_retry_s;     /* automount retry after a network failure (default 60) */
    int  amidrive_auth_s;      /* sign-in timeout (default 600) */
    int  amidrive_auth_check;  /* 1: ask Google whether it accepts the OAuth client + redirect before a sign-in (default 1) */
    char amidrive_auth_check_url[256];   /* tests only: another https:// authorization endpoint for that check */
} host_cfg;

extern host_cfg g_cfg;
extern plat_mutex *g_lock;
extern service *g_svcs[MAX_SERVICES];
extern int g_nsvcs;
extern int64_t g_started_ms;
extern volatile int g_stop;

void service_add(service *sv);
/* 0.1.4 session slots: take one of sv's max_clients slots for c. When the service is full and one of its sessions is
   the same client (same paired token, or the same client id text from the same address), that stale session is
   kicked (its socket shut down) and c takes over. Returns 0 (slot taken; call session_end later) or 1 (busy). */
int  session_begin(service *sv, conn *c);
void session_end(service *sv, conn *c);
void host_client(int delta);   /* +1 when a client connection opens, -1 when it closes (idle shutdown bookkeeping) */
int64_t parse_idle_ms(const char *v);   /* "30" = 30 minutes, "45s" = 45 seconds */
service *service_find(const char *name);
/* 0.1.6 quit-when-unused (control service): "app sessions" = live sessions of every service except hostinfo, pair
   and control (browser, amidiscord, fetch, compat-port clients...) */
int  host_app_sessions(void);
int  host_quit_arm(int grace_s, int *others);   /* returns CTL_STOPPING (nothing else connected: stop now) or CTL_ARMED */
void host_quit_cancel(void);
int  host_quit_state(int *others, int *grace_s);

/* auth.c */
int  auth_init(void);
int  auth_check_token(const char *tok, char *id_out, int idn);     /* 1 valid */
int  auth_pair(const char *code, const char *client, const char *client_id, char *tok_out, char *msg, int mn); /* 0 ok; replaces the same client's old token */
int  auth_new_code(char out[7], int minutes);                     /* writes confdir/pair-code */
int  auth_has_tokens(void);
int  auth_list(void);                                             /* CLI: print tokens */
int  auth_revoke(const char *id);                                 /* CLI: "all" or id prefix; returns count */
int  auth_banned(const char *peer);                               /* 1 if refused for now */
void auth_fail(const char *peer);                                 /* record a failed handshake / code */

/* config.c */
int  config_load(const char *path, char *err, int en);            /* globals into g_cfg, service blocks -> proxy services */

/* services */
service *svc_hostinfo_new(void);
service *svc_pair_new(void);
service *svc_fetch_new(void);
service *svc_control_new(void);
service *svc_amidrive_new(void);                                  /* 0.1.8, svc_amidrive.c */
int  amidrive_cli(const char *host, int port, const char *token, int argc, char **argv);   /* tools/amidrive_cli.c */
/* child-process backend (svc_proxy.c): kv is the config block, NULL-terminated "key", "value" pairs */
service *svc_proxy_new(const char *name, const char *const *kv, char *err, int en);
int  svc_proxy_compat_listeners(void);                            /* start 9223-style raw listeners + the backend reaper; 0 ok */

/* http.c: HTTPS GET with redirects, streaming body to a callback */
typedef int (*http_head_cb)(void *ud, int status, long long length);           /* nonzero = abort */
typedef int (*http_body_cb)(void *ud, const unsigned char *b, size_t n);         /* nonzero = abort */
void http_set_timeout(int ms);   /* 0.1.10: this thread's next requests (0 = defaults) */
int  http_get(const char *url, int (*allow)(const char *host), http_head_cb hc, http_body_cb bc, void *ud,
              char *err, int en);                                                  /* final HTTP status, or -1 */
/* 0.1.8: any method, extra header lines ("Name: value\r\n"...) and a body (NULL = none); no redirects followed for POST */
int  http_req(const char *method, const char *url, const char *headers, const char *body, int (*allow)(const char *host),
              http_head_cb hc, http_body_cb bc, void *ud, char *err, int en);
int  fetch_host_allowed(const char *host);

/* discovery.c */
int  discovery_start(void);
/* hostd.c */
void host_update_check(void);
#endif
