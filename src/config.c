/* AmiStudio Host - host.conf: same key/value block style as the release manifest.
 *   # globals first
 *   name odin
 *   lan yes
 *   fetch_allow github.com raw.githubusercontent.com objects.githubusercontent.com release-assets.githubusercontent.com
 *
 *   service browser            # each "service NAME" block mounts a child-process backend (svc_proxy.c)
 *   proto 2
 *   exec /opt/amistudio/asb-chromed -port {port} -token {token} -idle 0
 *   handshake asbc2
 *   compat_port 9223
 */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define MAXKV 64

static int yes(const char *v) { return !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1") || !strcasecmp(v, "true"); }
static int flush(char *name, char **kv, int *nkv, char *err, int en) {
    if (!name[0]) return 0;
    kv[*nkv] = NULL;
    service *sv = svc_proxy_new(name, (const char *const *)kv, err, en);
    for (int i = 0; i < *nkv; i++) free(kv[i]);
    *nkv = 0; name[0] = 0;
    if (!sv) return -1;
    service_add(sv);
    return 0;
}
int config_load(const char *path, char *err, int en) {
    FILE *f = fopen(path, "r"); char line[1024], name[ASH_MAX_NAME + 1] = ""; char *kv[MAXKV * 2 + 1]; int nkv = 0, ln = 0;
    if (!f) { snprintf(err, en, "cannot read %s", path); return -1; }
    while (fgets(line, sizeof line, f)) {
        ln++;
        /* '#' after a blank starts a comment, except inside "double quotes" (0.1.8: paths such as
           amidrive_mountpoint "/home/me/AmiStudio/Google Drive" are written quoted) */
        for (int i = 0, q = 0; line[i]; i++) { if (line[i] == '"') q = !q; if (!q && line[i] == '#' && (i == 0 || line[i-1] == ' ' || line[i-1] == '\t')) { line[i] = 0; break; } }
        trim(line);
        if (!line[0]) continue;
        char *k = line, *v = line; while (*v && *v != ' ' && *v != '\t') v++;
        if (*v) { *v++ = 0; while (*v == ' ' || *v == '\t') v++; }
        { size_t vl = strlen(v); if (!name[0] && vl >= 2 && v[0] == '"' && v[vl-1] == '"') { v[vl-1] = 0; v++; } }   /* a quoted global value */
        if (!strcmp(k, "service")) {
            if (flush(name, kv, &nkv, err, en)) { fclose(f); return -1; }
            if (!*v || strlen(v) > ASH_MAX_NAME) { snprintf(err, en, "%s:%d: service needs a name (max %d chars)", path, ln, ASH_MAX_NAME); fclose(f); return -1; }
            snprintf(name, sizeof name, "%s", v); continue;
        }
        if (name[0]) { if (nkv < MAXKV * 2) { kv[nkv++] = strdup(k); kv[nkv++] = strdup(v); } continue; }
        if (!strcmp(k, "name")) snprintf(g_cfg.name, sizeof g_cfg.name, "%s", v);
        else if (!strcmp(k, "port")) g_cfg.port = atoi(v);
        else if (!strcmp(k, "lan")) { g_cfg.lan = yes(v); snprintf(g_cfg.listen, sizeof g_cfg.listen, "%s", g_cfg.lan ? "0.0.0.0" : "127.0.0.1"); }
        else if (!strcmp(k, "listen")) { snprintf(g_cfg.listen, sizeof g_cfg.listen, "%s", v); g_cfg.lan = strncmp(v, "127.", 4) != 0; }
        else if (!strcmp(k, "discovery")) g_cfg.discovery = yes(v);
        else if (!strcmp(k, "update_check")) g_cfg.update_check = yes(v);
        else if (!strcmp(k, "fetch_allow")) snprintf(g_cfg.fetch_allow, sizeof g_cfg.fetch_allow, "%s", v);
        else if (!strcmp(k, "fetch_max_mb")) g_cfg.fetch_max = atol(v) * 1048576L;
        else if (!strcmp(k, "cafile")) snprintf(g_cfg.cafile, sizeof g_cfg.cafile, "%s", v);
        else if (!strcmp(k, "bundled_browser")) g_cfg.bundled_browser = yes(v);
        else if (!strcmp(k, "browser_idle")) g_cfg.browser_idle = atoi(v);
        else if (!strcmp(k, "bundled_amidiscord")) g_cfg.bundled_amidiscord = yes(v);
        else if (!strcmp(k, "amidiscord_idle")) g_cfg.amidiscord_idle = atoi(v);
        else if (!strcmp(k, "allow_quit")) g_cfg.allow_quit = yes(v);
        else if (!strcmp(k, "exit_idle")) g_cfg.exit_idle_ms = parse_idle_ms(v);
        else if (!strcmp(k, "data_dir")) snprintf(g_cfg.data_dir, sizeof g_cfg.data_dir, "%s", v);
        else if (!strcmp(k, "manifest")) snprintf(g_cfg.manifest_url, sizeof g_cfg.manifest_url, "%s", v);
        else if (!strcmp(k, "amidrive")) g_cfg.amidrive = yes(v);
        else if (!strcmp(k, "amidrive_mountpoint")) snprintf(g_cfg.amidrive_mountpoint, sizeof g_cfg.amidrive_mountpoint, "%s", v);
        else if (!strcmp(k, "amidrive_rclone")) snprintf(g_cfg.amidrive_rclone, sizeof g_cfg.amidrive_rclone, "%s", v);
        else if (!strcmp(k, "amidrive_flags")) snprintf(g_cfg.amidrive_flags, sizeof g_cfg.amidrive_flags, "%s", v);
        else if (!strcmp(k, "amidrive_client_id")) snprintf(g_cfg.amidrive_client_id, sizeof g_cfg.amidrive_client_id, "%s", v);
        else if (!strcmp(k, "amidrive_client_secret")) snprintf(g_cfg.amidrive_client_secret, sizeof g_cfg.amidrive_client_secret, "%s", v);
        else if (!strcmp(k, "amidrive_fusermount")) snprintf(g_cfg.amidrive_fusermount, sizeof g_cfg.amidrive_fusermount, "%s", v);
        else if (!strcmp(k, "amidrive_automount")) g_cfg.amidrive_automount = yes(v);
        else if (!strcmp(k, "amidrive_keeps_host")) g_cfg.amidrive_keeps_host = yes(v);
        else if (!strcmp(k, "amidrive_stop_timeout")) g_cfg.amidrive_stop_s = atoi(v);
        else if (!strcmp(k, "amidrive_retry")) g_cfg.amidrive_retry_s = atoi(v);
        else if (!strcmp(k, "amidrive_auth_timeout")) g_cfg.amidrive_auth_s = atoi(v);
        else if (!strcmp(k, "amidrive_auth_check")) g_cfg.amidrive_auth_check = yes(v);
        else if (!strcmp(k, "amidrive_auth_check_url")) snprintf(g_cfg.amidrive_auth_check_url, sizeof g_cfg.amidrive_auth_check_url, "%s", v);
        else LOG("%s:%d: unknown setting '%s' ignored", path, ln, k);
    }
    fclose(f);
    return flush(name, kv, &nkv, err, en);
}
