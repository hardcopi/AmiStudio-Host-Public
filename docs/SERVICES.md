# Adding a service

Two ways:

1. **Built in** (small, no extra process): write `src/svc_NAME.c` with a constructor returning a `service`
   (`host.h`): name, protocol version range, `need_auth`, and `run(service*, conn*)`, which owns the socket after the
   ASH OK. Use `frame_send`/`frame_recv` (u32 len + u8 op). Register it in `main()` next to hostinfo/pair/fetch.
   Examples: `svc_builtin.c` (hostinfo, pair, control), `svc_fetch.c`.
2. **Child process** (an existing program, or anything heavy/crashy): a `service NAME` block in host.conf, handled
   by `svc_proxy.c`. The Host starts the program on demand on a random loopback port with a random token
   (`{port}`, `{token}` placeholders), reaps it within a second if it exits (no zombies; hostinfo then shows
   `idle exited ...` and the next client starts it again; `start boot` backends are restarted at once, with a 5 s
   back-off after a quick crash), stops it with the Host, logs its output to
   `CONFDIR/NAME.log`, and passes bytes through. `handshake asbc2` makes the Host do the backend's ASBC v2 handshake;
   `handshake none` passes raw bytes from the first byte. Keys: `proto MIN [MAX]`, `exec`, `handshake`, `start
   ondemand|boot`, `ready_timeout`, `max_clients`, `compat_port`, `log`, `auth yes|no`, `stop_timeout` (0.1.4).
   Placeholders in `exec`: `{port}`, `{token}`, `{port2}` (a second free loopback port), `{name}`, `{confdir}` /
   `{config}` (the Host's config folder) and `{state}` (0.1.4: its data folder, `~/.local/share/amistudio-host`,
   `%LOCALAPPDATA%\AmiStudio-Host` on Windows, or `data_dir` / `--data-dir`); paths are substituted quoted. Give each
   service its own data, e.g. `ASB_STATE={state}/NAME ASB_CONFIG={state}/NAME/config`.
   Stopping (0.1.4): on Host shutdown the backend gets SIGTERM alone and `stop_timeout` seconds (default 15) to exit
   by itself - so asb-chromed can close Chrome cleanly and the profile is written - then its whole process group
   gets SIGTERM and, 2 s later, SIGKILL. On Windows the job object is ended as before.

## Bundled browser engine (release tarball, 0.1.2+)
`tools/fetch-engine.sh` (run by `make tarball`) builds `build/engine/stage/`:
- `engine/asb-chromed`: `git archive` of hardcopi/amistudio-browser at the pinned `ASB_PIN` (AmiSurf 0.87,
  6493b84 on main: the 0.87 engine; main's next commit only adds screenshots), `make build/static/asb-chromed`; AmiSurf's repo is only read (`ASB_REPO`, default
  ../amistudio-browser, else a bare clone in build/engine).
- `engine/chromium/`: ungoogled-chromium portablelinux `UCVER` 155.0.8059.79-1 x86_64, sha256 checked against the
  pin (= GitHub's asset digest), chromedriver removed - as AmiSurf's dist/amikit/mkbundle.sh does.
- `engine/engine.sh` (from dist/engine/): AmiSurf's AmiKit engine.sh with data in
  ~/.local/share/amistudio-host/browser instead of the drawer.
- `LICENSES/`: Chromium-LICENSE.txt, chromium-credits.html, ungoogled-chromium + portablelinux licences, AmiSurf
  ATTRIBUTIONS, stb_image, qoi-MIT, OpenSSL, zlib, zstd, ENGINE-VERSION.txt.
Data layout = AmiSurf AmiKit `Host/data`: `config/` (ASB_CONFIG), `profile/` (-profile), optional `WidevineCdm/`.
The pin was checked for Host 0.1.3: asb-chromed's source (chrome/) is identical from 4bb4f04 to bd10794.
Host 0.1.7 moves it to AmiSurf 0.85 (e762b82): AmiSurf's own browser-wide CDP connection (`g_bcon`: Browser.download*
events and status, tabs the page opens shown at once, a tab closed by the page falls back to its opener), released
window.open popups, a fresh frame on tab switch. That connection is off in app mode (`ASB_APP`): app mode keeps
its own browser connection (`g_bws`: popups caught and closed, downloads named with allowAndName) and has no tabs.
The engine patches were rebased onto it (`tools/engine-work.sh`: conflicts only in the Makefile's static-build
rule and in the download status lines of features.inc, both sides kept).
Host 0.1.9 moves it to AmiSurf 0.87 (6493b84): `ASBC_BMARKS` (45: the bookmark list, `url\ttitle\tfolder` lines, sent
after the client's `bookmarks` command and whenever the list changes, for AmiSurf's bookmarks bar and menu), an
optional 4th folder column in bookmarks.tsv (Folder field on the internal Bookmarks page; older engines keep it), and
the internal pages server closing idle (preconnected) sockets without a reply. Rebase conflicts only in asbc.h's
message enum (ASBC_BMARKS next to app mode's ASBC_OPENURL) and the client-connect line of host.c (`g_bm_want=0` and
app mode's client key reset, both kept); features.inc and the Makefile merged cleanly.
To move to a newer AmiSurf engine or Chromium, change `ASB_PIN` / `UCVER` / `UC_SHA` together and run
`ASH_TEST_BROWSER=1 make tarball test`.
At run time amistudio-hostd mounts `<its folder>/engine/engine.sh` as `browser` (proto 2, handshake asbc2,
ready_timeout 40, exec `sh engine.sh -port {port} -token {token} -cdp {port2} -idle <browser_idle>`) unless host.conf
defines `browser` or says `bundled_browser no`.

## AmiDiscord (`amidiscord`, app mode) - 0.1.4
The bundled engine is also mounted as `amidiscord` (unless host.conf defines it or says `bundled_amidiscord no`):
```
service amidiscord
proto 2
exec env ASB_APP=amidiscord ASB_APP_URL=https://discord.com/app ASB_APP_TITLE=AmiDiscord ASB_APP_EXTERNAL=client ASB_STATE={state}/amidiscord ASB_CONFIG={state}/amidiscord/config sh engine.sh -port {port} -token {token} -cdp {port2} -idle <amidiscord_idle>
handshake asbc2
ready_timeout 40
max_clients 1              # a reconnect of the same Amiga takes the slot over (0.1.4)
log amidiscord.log
stop_timeout 20            # time for Browser.close, so the Discord login persists
```
Data (engine patch 0003): both `ASB_STATE` (Chrome profile, downloads) and `ASB_CONFIG` (settings) are set per
service, so nothing is shared with AmiSurf's `browser` (the AmiDiscord prototype ran raw asb-chromed, whose
ASB_CONFIG default is AmiSurf's `~/.config/amisurf`; in app mode that default is now `~/.config/amisurf-app-<id>`
and the profile default `~/.config/amistudio-chrome-<id>`). On SIGTERM, and on its own idle exit, asb-chromed in app
mode closes Chrome with `Browser.close` and waits for it, so cookies and localStorage (the login) are on disk; it
logs `app: Chrome closed cleanly`. `CMD app logout` clears cookies, cache and all storage of the allowlisted
origins and loads the start page again.
App mode is the Host's engine patch 0002 (engine-patches/, chrome/appmode.inc): everything is gated on `ASB_APP`,
so AmiSurf's `browser` service behaves as before. Settings (environment of the exec line):

| var | default | meaning |
|---|---|---|
| `ASB_APP` | - | app id; turns app mode on. engine.sh keeps its data in `<data>/<id>` (see below) |
| `ASB_APP_URL` | `https://discord.com/app` | start page (loaded at start and on `CMD app home`) |
| `ASB_APP_ALLOW` | discord.com, *.discord.com, discordapp.com, *.discordapp.com, *.discordapp.net, discord.gg, *.discord.gg, discord.new, *.hcaptcha.com, hcaptcha.com | host globs that may load in the page (`host:port` allowed) |
| `ASB_APP_EXTERNAL` | `client` | other links: `client` = cancel + OPENURL to the Amiga, `deny` = just cancel |
| `ASB_APP_TITLE` | app id | META title until the page has one |
| `ASB_APP_REDUCEDMOTION` | 1 | prefers-reduced-motion + frame throttling (`CMD app reducedmotion 0|1`) |
| `ASB_APP_SMALL_MS` / `ASB_APP_BIG_MS` | 100 / 1000 | throttle: one update per SMALL_MS; changes over 1/4 of the view one per BIG_MS |
| `ASB_APP_DARK` | 0 | 1 = also prefers-color-scheme: dark |
| `ASB_APP_MAXUPLOAD` | 25 | upload limit (MB per FILEREQ, 1..4095); sent to the client in `APPINFO` (0.1.5) |
| `ASB_APP_NOUNREAD` | `/login,/register,/reset,/verify` | logged-out path prefixes (and `/`): no unread state there (0.1.5) |
| `ASB_UPLOADS` / `ASB_DOWNLOADS` | `<ASB_STATE>/uploads`, `/downloads` | files handed to / saved by the page |
| `ASB_STATE` / `ASB_CONFIG` | `<data>/<id>`, `<ASB_STATE>/config` | the app's own profile/downloads and settings (engine.sh) |

Data folder rule (0.1.5, the same for every service started through engine.sh): `<data>` is the Host's `{state}`
(`--data-dir` / `data_dir`, default `~/.local/share/amistudio-host`), passed to every service as the environment
variable `AMISTUDIO_HOST_DATA`; the service's folder is `ASB_STATE` if the exec line sets it, else `<data>/<id>`
(`<id>` = `ASB_APP`, or `browser`); downloads and uploads are its `downloads/` and `uploads/` unless `ASB_DOWNLOADS`
/ `ASB_UPLOADS` are set. So `--data-dir DIR` moves AmiDiscord's downloads to `DIR/amidiscord/downloads` whether or
not a custom service block sets `ASB_STATE` (before 0.1.5 a block without it ignored `--data-dir`).
asb-chromed run without engine.sh in app mode uses `<ASB_STATE>/downloads` (or `<ASB_CONFIG>/downloads`), never
AmiKit's Downloads drawer.

Notifications (engine patch 0004, chrome/appnotify.inc; no DOM scraping): a small preload script on the allowlisted
origins replaces `window.Notification` and `ServiceWorkerRegistration.showNotification` and reports each
notification (title, body, tag, `data.url`, a <= 64x64 RGB icon) plus the main frame's title and favicon (a
MutationObserver on `<head>` only). Notification permission is granted for the app's origins, so Discord turns its
desktop notifications on. Runtime.enable stays off (pages can detect it): the binding has a random name, is
re-added on each navigation and removed from `window` by the script at once, and every message carries a per-run
key. Unread state = a title pattern table (`(N) `, `• `) plus a favicon change. Focus/visibility: `CMD app focus F V`
(ASBC_FOCUS with caps) makes the page unfocused/hidden - Discord only notifies when it is - and `V=0` stops all
frames while notifications keep coming; with no client the page is unfocused and hidden, so Discord does not mark
messages read. `CMD app notifyact ID 0|1` clicks/dismisses a notification (the page's onclick opens the channel).

Client protocol (engine patch 0005, chrome/appproto.inc): the client sends `CAPS` after `FMT`; only then does it get
`APPINFO`, `NOTIFY`/`NOTIFYCLOSE`, `UNREAD`, `OPENURL`, `FILEREQ`/`FILEDONE` and `DOWNLOAD`, and only those it set bits
for (docs/PROTOCOL.md, "App mode messages"). Uploads go to `ASB_UPLOADS` (engine.sh: `<app data>/uploads`, removed after
10 minutes, limit `ASB_APP_MAXUPLOAD` MB, default 25); downloads stay in `ASB_DOWNLOADS` (`<app data>/downloads`,
for the bundled service `<data>/amidiscord/downloads`) and `DOWNLOAD` reports the exact path.
Engine patch 0006 (0.1.5, from AmiDiscord's tests): short `FILEPUT` cancel (5 bytes), `APPINFO` max upload,
`OPENURL` url limit 1023 bytes, no unread state on logged-out pages, app-mode download default.
| `ASB_APP_MUTE` | 1 | Chrome `--mute-audio`, no audio sink: the app's own sounds never play |

Behaviour: no tabs (TABNEW/TABSEL/TABCLS ignored, no TABS), no asb:// pages, bookmarks, history, suggestions, password
manager, save page/source/PDF; CMD only `zoom`, `find*`, `ctx`, `avdelay`, `home`, `app home|reducedmotion`.
Top-level loads of other hosts are cancelled before any request (CDP Fetch); new windows (`window.open`,
`target=_blank`) are caught paused through Chrome's browser endpoint and closed: allowlisted URLs open in the app's
page, others go to the client as ASBC `OPENURL` (op 48: u16 len + url, u8 reason 0 link / 1 new window / 2 client
NAV refused). Client NAV is accepted for allowlisted hosts only. Measured with the test app: an animated page sends
5 frames / 3.8 MB in 5 s with reduced motion, 150 frames / 115 MB without.

## Google Drive (`amidrive`) - 0.1.8
Built in (`src/svc_amidrive.c`; wire format in docs/PROTOCOL.md "amidrive v1"). The Host mounts the user's Google
Drive with the bundled rclone (`engine/rclone/rclone`, v1.75.2, MIT, `LICENSES/rclone-COPYING.txt`;
`tools/fetch-rclone.sh` pins it by sha256) through FUSE, at a folder the Amiga sees through Amiberry.

**Files** (`{state}` = `data_dir`; with the AmiStudio launcher `AmiStudio/Host/data/services`):

| path | what |
|---|---|
| `{state}/amidrive/rclone.conf` | 0600: remote `gdrive` (type drive, scope drive, client_id/client_secret when configured, token) |
| `{state}/amidrive/cache/` | rclone's VFS cache (`--cache-dir`): pending uploads live here until they are sent |
| `{state}/amidrive/drawer.path` | the realpath of `{state}/amidrive`, stored at sign-in (copied/moved drawer check) |
| `{state}/amidrive/Drive.mount` | marker while mounted: `amidrive<TAB>1<TAB><nonce><TAB><account or -><TAB><mount path>` |
| `{state}/amidrive/rclone.pid` | rclone's PID (a leftover from a killed Host is stopped at the next start) |
| `{state}/amidrive/amidrive.lock` | held (flock) by the Host that owns this Drive; a second Host on the same data folder never clears or mounts it |
| `{state}/amidrive/account`, `signin.expired` | the signed-in address; set when Google answered `invalid_grant` |
| `{state}/amidrive/amidrive.log`, `rclone.log` | the service's log and rclone's (rotated at 1 MB / 4 MB) |
| `{state}/amidrive/Google Drive.info` | the drawer icon, parked here while unmounted (first copied from the Host's `services/amidrive/`) |
| `{state}/amidrive/cache.copied-<date>/` | the cache of a copied drawer, set aside and never uploaded |
| mount point | host.conf `amidrive_mountpoint` (the launcher writes `<AmiStudio drawer>/Google Drive`); default `{state}/amidrive/Google Drive`. Created 0755 at mount time and removed when unmounted; its icon sits next to it while mounted |

`{state}/amidrive` is created on first use (sign-in or CONNECT), not at Host start. Every rclone run gets
`RCLONE_CONFIG`, `RCLONE_CONFIG_DIR` and `RCLONE_CACHE_DIR` in `{state}/amidrive`: nothing goes to `~/.config/rclone`,
`~/.cache/rclone` or `/tmp`.

**Mount:** `rclone mount gdrive: "<mount point>" --cache-dir {state}/amidrive/cache --vfs-cache-mode full
--vfs-write-back 10s --vfs-cache-max-size 2G --vfs-cache-max-age 24h --vfs-cache-min-free-space 1G --dir-cache-time 5m
--poll-interval 1m --drive-skip-gdocs --transfers 2 --tpslimit 8 <amidrive_flags> --drive-use-trash=true --rc
--rc-addr 127.0.0.1:<free port> --rc-user amidrive --rc-pass <random> --log-file {state}/amidrive/rclone.log`.
`amidrive_flags` can override the defaults but not `--allow-non-empty`, `--allow-other`, `--allow-root`, `--daemon`,
`--drive-use-trash` (always on: deletes go to Drive's trash), `--config`, `--cache-dir`, `--rc-*` or `--log-file`.
A mount point that is not empty is refused (flag 32, RESULT 7). Status comes from rclone's rc (`vfs/queue`,
`core/stats`, `operations/about`); FLUSH sets every queued item's expiry to 0 (`vfs/queue-set-expiry`). Unmount uses
`fusermount3 -u` (lazy `-uz`) and falls back to `umount` (`umount -l`), because some systems have no `/etc/mtab`.
After a lazy unmount with files still open, rclone keeps serving them until they are closed (CONNECT answers 4
until then).

**Lifecycle** (Rich, 19.10.2026: Drive is mounted while AmiStudio is in use, and unmounted when it quits):
- Nothing mounts when the Host starts. CONNECT flag 1, sent by the launcher and AmiDrive when they start, mounts
  if `amidrive_automount yes` (default) and the user is signed in.
- When the Host stops (the launcher's STOPHOST / quit-when-unused, the idle exit, SIGTERM) amidrive stops in
  parallel with the other services: it flushes pending uploads (PROGRESS to watchers) for up to
  `amidrive_stop_timeout` seconds (default 60), unmounts, stops rclone, then the Host exits. Whatever did not upload
  stays in `cache/` and goes out on the next mount. At logout, when rclone and the Host get SIGTERM together, only
  "pending files stay in cache/" is promised.
- `amidrive_keeps_host no` (default): a mount never keeps the Host running by itself; the launcher's and AmiDrive's
  sessions do. With `yes`, the service is busy while mounted: it counts as a client for quit-when-unused and
  `exit_idle`, and the Host keeps `CONFDIR/busy` for the launcher's run-host.sh watcher.
- At start the Host stops a leftover rclone (the pidfile, checked against its command line: rclone, `mount`, the
  mount point) and clears a stale mount, unless another Host holds `amidrive.lock`.
- rclone exits or its rc stops answering for 10 s: lazy unmount, state 8, at most one automatic retry while a
  session is open.
- **Copied or moved drawer:** every CONNECT compares the realpath of `{state}/amidrive` with `drawer.path`. On a
  mismatch rclone is not started with that token: flag 64, RESULT 8 "This drawer was copied/moved: sign in again".
  A copy (the stored path still exists) has its `cache/` set aside as `cache.copied-<date>/` (never uploaded, so the
  original's pending files are not sent twice); a moved drawer keeps `cache/`, which uploads after the new sign-in.
  Don't delete or copy the AmiStudio drawer while Google Drive is mounted: quit AmiStudio first.
- **Expired sign-in** (`invalid_grant` from Google; the OAuth project is in Testing, so refresh tokens last 7 days):
  flag 16, CONNECT answers 6 without starting rclone, no retries; the mount and pending uploads are kept until the
  user signs in again (then the Host remounts with the new token if no file is open).

**Sign-in** runs `rclone authorize drive` with the user's real desktop environment, so the PC's own browser opens:
the launcher's run-host.sh saves `HOME`, `XDG_*`, `XDG_RUNTIME_DIR` and `TMPDIR` as `AMISTUDIO_REAL_*` (plus
`AMISTUDIO_REAL_ENV=1`) before it moves HOME into the drawer; `DISPLAY`, `WAYLAND_DISPLAY` and
`DBUS_SESSION_BUS_ADDRESS` are passed through. The OAuth client is `amidrive_client_id` / `amidrive_client_secret`
(release tarball: `host-defaults.conf`, see below); with either empty rclone's built-in client is used. They reach
rclone through `RCLONE_DRIVE_CLIENT_ID/SECRET` in its environment, never on a command line, and the Host writes
rclone.conf itself (0600) rather than with `rclone config create`, which would put the token on a command line. The
account address is read from Drive's `about` with the new token. Tokens and the secret are never sent to a client
and never logged.
*0.1.9:* **sign-in in AmiSurf** - AUTH_START 2 gives the `http://127.0.0.1:53682/auth?...` URL
for the Host's own browser engine when a `browser` service exists (rclone `--auth-no-open-browser`); AmiSurf renders
through that engine on this PC, so Google's redirect back to 127.0.0.1:53682 reaches rclone. No engine: still the URL
only (it works in a browser on this PC; logged), never the desktop browser. AUTH 0 has a flags byte (1 = engine,
2 = desktop browser failed for 0); joiners with 1/2 get the URL, never AUTH 1. **Sign-in check** - before rclone starts, the Host GETs Google's authorization endpoint with the
client ID, rclone's redirect and the drive scope (never the secret) and looks for Google's "Error 4xx: <code>"
page; `redirect_uri_mismatch` (the client does not allow `http://127.0.0.1:53682/`: a Web-type client without that
redirect URI) or any other refusal ends the sign-in with that error instead of a browser on an error page. Google
unreachable or any other answer: the sign-in goes on. Only with our own client (both `amidrive_client_*` set).
`amidrive_auth_check no` turns it off; `amidrive_auth_check_url` (tests) points it at another https endpoint.

**Settings** (host.conf): `amidrive yes|no`, `amidrive_mountpoint PATH` (quote it: it usually has a space),
`amidrive_rclone PATH`, `amidrive_flags ...`, `amidrive_client_id`, `amidrive_client_secret`, `amidrive_fusermount`
(default `fusermount3`), `amidrive_automount yes|no`, `amidrive_keeps_host no|yes`, `amidrive_stop_timeout 60`,
`amidrive_retry 60`, `amidrive_auth_timeout 600`, `amidrive_auth_check yes` (0.1.9). The release tarball's `host-defaults.conf` (next to the binary,
read before host.conf, mode 0600) carries the AmiStudio OAuth client; host.conf wins.

**CLI:** `amistudio-hostd amidrive status [--watch] | signin [--url] | connect [--auto] | flush |
unmount [--force|--later] | signout [--revoke]` (also `ash-cli amidrive ...`). Exit status is the RESULT status;
9 = the Host could not be reached, 10 = usage.

**Tests:** `test/amidrive_tests.py` (run by `make test`; needs FUSE and `tools/fetch-rclone.sh`): an rclone `alias`
remote of a local folder stands in for Drive, and the mount point has a space. Not covered there: the real Google
sign-in and quota, and Amiberry's view of the drawer.

## AmiStudio Browser (`browser`)
asb-chromed already speaks ASBC v2 on a loopback port with an optional token, so it mounts unchanged:
```
service browser
proto 2
exec /path/to/asb-chromed -port {port} -token {token} -idle 0
handshake asbc2
compat_port 9223
```
`amistudio-hostd --browser /path/to/asb-chromed` does the same from the command line. Tested with the real
asb-chromed + Chrome (`ASH_TEST_BROWSER=1 make test`): page frames arrive through the Host.

Notes for the Browser bot when it moves to ASH (not done; Rich asked to leave Browser alone for now):
- connect to 9220, handshake service `browser` version 2 with the token from `ENV:AmiStudio/HostToken`, then send
  ASBC frames directly (no second ASBC handshake);
- find the Host via `HOST=` / `ENV:AmiStudio/Host` / 127.0.0.1:9220 (LAUNCHER-PLAN 2.4);
- asb-chromed bans an address after 5 bad handshakes; every Host connection comes from 127.0.0.1, so the Host
  only opens one probe connection per backend start.
