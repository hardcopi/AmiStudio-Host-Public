# ASH v1: the AmiStudio Host protocol

The Host listens on **TCP 9220**. Every connection is one service session: the client names a service in the
handshake, and after an OK the socket carries that service's own protocol. There is no multiplexing inside a
socket (a PDF page never waits behind video frames, and 68k clients need no demuxer). All integers are
little-endian. Strings are `u8 length` + bytes, no terminator.

## Handshake

Client → Host:

| field | type |
|---|---|
| magic | `"ASH1"` |
| ASH version | u16 = 1 |
| service name | u8 len + name (`hostinfo`, `pair`, `fetch`, `browser`, ...; max 32) |
| service protocol version | u16 |
| token | u8 len + token (0-64 bytes; the 64-hex token from pairing, or empty) |
| client id | u8 len + text (`AmiStudio/0.2 68040`; max 64) |

Host → client: `"ASH1"` · u16 ASH version · u8 status · u16 service version · u8 len + message (for a requester).

| status | meaning | service version field |
|---|---|---|
| 0 | OK; the socket now belongs to the service | version spoken (= requested) |
| 1 | bad or missing token: pair this Amiga first | 0 |
| 2 | no such service on this Host | 0 |
| 3 | client too old for this service | lowest version the Host speaks |
| 4 | client newer than this Host's service (or ASH version) | highest version the Host speaks |
| 5 | busy (service `max_clients` reached) | requested |
| 6 | refused: address banned after 5 failed handshakes in 60 s (5 minutes), or the backend failed to start | requested |

The Host closes a connection without a reply if the first bytes are not `ASH1` (port scanners, HTTP).
The handshake must arrive within 10 seconds.

## Authentication

- **Loopback** clients (127.x, e.g. an emulator on the Host PC) need no token, as with ASBC today.
- **LAN** clients need a paired token for every service except `hostinfo` and `pair`.
- No TLS (68k CPU cost); the token travels in clear text, so the Host is for the LAN or a VPN/SSH tunnel only.
  Never forward 9220 to the internet.

### Pairing
1. On the PC: `amistudio-hostd pair` prints a one-time 6-digit code (valid 10 minutes, `--minutes N`). When the
   Host starts with `--lan` and nothing is paired yet, it logs a code valid 30 minutes.
2. The Amiga connects to service `pair` v1 (no token) and sends frame `PAIR_REQ` (op 1) = `"123456"` optionally
   followed by `"\n"` and a name for the Amiga (`A4000 study`), optionally followed by `"\n"` and a stable client id
   (any printable text up to 64 chars, e.g. a random id the Amiga keeps in ENVARC:). Spaces in the code are ignored.
   Since Host 0.1.1 a successful pair **replaces** the same Amiga's earlier token: same client id if one is sent,
   otherwise the same name from the same address.
3. Host replies `PAIR_OK` (op 2) = 64 lower-case hex chars (32 random bytes), or `PAIR_FAIL` (op 3) = message.
   A code works once; after 5 wrong codes it is thrown away.
4. The launcher stores the token in `ENVARC:AmiStudio/HostToken` (copied to ENV:) and every app sends it in the
   handshake. The Host keeps only SHA-256(token) in `~/.config/amistudio-host/tokens` (mode 0600).
   `amistudio-hostd tokens` lists paired Amigas with paired and last-used times, `amistudio-hostd revoke ID|all`
   unpairs (effective at once).

## Service framing

Built-in services (and new ones) use ASBC-style frames: **u32 length** (op + payload) · **u8 op** · payload.
Frames are at most 1 MB unless a service says otherwise.

### hostinfo v1
After OK the Host sends one frame, op 1 (`HI_INFO`), of `key value` text lines, then closes:
```
host 0.1.1
ash 1
name odin
id 3f1c0a9be2d47781        # stable random id (same in discovery)
os linux-x86_64
port 9220
lan 1
paired 1                    # this connection is trusted (loopback or valid token)
uptime 3600
load 0.12
update host 0.2             # only when a newer Host is listed in AmiStudio-Releases
service hostinfo 1 up
service pair 1 up
service fetch 1 up
service control 1 up        # Host 0.1.6+: quit-when-unused (below)
service browser 2 idle      # NAME HIGHEST-VERSION up|idle|down [reason]
                            # "idle exited signal 9": the backend ran and quit; the next client restarts it
```
Unknown lines must be ignored.

### fetch v1 (HTTPS download proxy)
For Amigas without AmiSSL or with a slow 68020 TLS handshake (launcher `FETCH=HOST`).
- Client `GET` (op 1): `url` [`\n` expected size [`\n` expected sha256]]. Only `https://`; only hosts on the
  Host's `fetch_allow` list (default: GitHub release and raw hosts), checked again on every redirect.
- Host `HEAD` (op 2): u32 size (0xFFFFFFFF = unknown, chunked) · u16 HTTP status (200).
- Host `DATA` (op 3): body in chunks of at most 32 KB.
- Host `END` (op 4): 64-hex SHA-256 of the body · u32 byte count.
- Host `ERR` (op 5): message, instead of HEAD or after some DATA (size/SHA-256 mismatch with the expected
  values, HTTP error, TLS failure, host not allowed, over `fetch_max_mb`). A size mismatch known from the server's
  Content-Length is refused before any DATA.
- The client may send further `GET`s on the same connection. It still checks size and SHA-256 itself against
  the manifest.

### control v1 (quit-when-unused, Host 0.1.6+)
For the AmiStudio launcher: when the user closes AmiStudio, the Host should not keep running for nobody.
- **Access:** like `fetch` (need_auth): loopback clients, or LAN clients with a paired token. A LAN client without
  a valid token gets ASH status 1.
- **Version gate:** request service `control` version 1. A Host older than 0.1.6 answers ASH status **2** (no such
  service): then there is nothing to arm (stop it the old way, or leave it). `hostinfo` of a 0.1.6+ Host lists
  `service control 1 up`, so a launcher can also check that first.
- **Frames** (u32 length (op + payload) · u8 op · payload, little-endian), the connection stays open for more:

| op | direction | name | payload |
|---|---|---|---|
| 1 | client → Host | `QUIT_WHEN_UNUSED` | optional `u16 grace` seconds (absent or 0 = 10, max 300) |
| 2 | client → Host | `QUIT_CANCEL` | none: disarm (e.g. the launcher starts again) |
| 3 | client → Host | `QUIT_STATUS` | none: just report |
| 16 | Host → client | `QUIT_STATE` | `u8 state` · `u16 other clients` · `u16 grace` seconds (5 bytes) |

  The Host answers every request (1, 2, 3) with one `QUIT_STATE`; unknown ops are ignored without an answer.
  `state`: 0 not armed (also the answer to op 1 when the Host runs with `allow_quit no` / `--no-remote-quit`),
  1 armed (waiting for the other clients), 2 stopping now.
- **Other clients** = open sessions of every service except `hostinfo`, `pair` and `control` itself: AmiSurf
  (`browser`), AmiDiscord (`amidiscord`), `fetch`, other child services and the plain-ASBC compat port.
- **Behaviour:** `QUIT_WHEN_UNUSED` with no other client → `QUIT_STATE 2 0 grace`, and the Host stops right away:
  it leaves its main loop within 0.5 s and stops every service the normal way (engines get SIGTERM and close
  Chromium cleanly with `Browser.close`, up to `stop_timeout`), then exits 0. With other clients connected →
  `QUIT_STATE 1 N grace`: the Host keeps serving them and stops once **no** other client has been connected for
  `grace` seconds; a client that reconnects or takes its slot over within the grace period resets the timer. This
  works whether or not `exit_idle` is set (an `exit_idle` that fires earlier still stops the Host). Arming again
  replaces the grace; `QUIT_CANCEL` disarms (too late once state 2 was answered). The armed state is not stored:
  a Host restart starts unarmed. The Host logs each step.
- The launcher on quit, byte by byte: connect, send the handshake `"ASH1" 01 00 07 "control" 01 00 <u8 n>
  <token, n bytes; n = 0 on loopback> <u8 m> <client id, m bytes>`, read the 10-byte ASH reply (+ message) and check
  status 0, send `03 00 00 00 01 0A 00` (QUIT_WHEN_UNUSED, grace 10 s; `01 00 00 00 01` for the default), read
  `06 00 00 00 10 <u8 state> <u16 others> <u16 grace>`, close. Closing right after the request is fine: it stays
  armed. State 2 means the Host is going away now; a later status check finds nothing listening.

### amidrive v1 (Google Drive, Host 0.1.8+)
Google Drive mounted on the Host PC with rclone, inside the AmiStudio drawer, so Amiberry shows it as an ordinary
drawer (`AmiStudio/Google Drive`). The Amiga reads and writes files there directly; the service only controls the
mount. Client: AmiDrive (and the launcher). Request spec: `AMIDRIVE-HOST-REQUEST.md`. Files and lifecycle:
docs/SERVICES.md "Google Drive".
- **Access:** loopback, or LAN with a paired token. At most 4 sessions. hostinfo:
  `service amidrive 1 up`, `down no-fuse` or `down no-rclone`.
- **Strings** are u8 length + UTF-8 (cut to 255 bytes on a character boundary). Every Host → client frame is under
  8 KB (largest: RESULT 1 with 20 paths, 5381 bytes); the Host accepts client frames up to 4 KB.

| op | direction | name | payload |
|---|---|---|---|
| 1 | client → Host | `STATUS` | none → one STATE (no RESULT) |
| 2 | client → Host | `WATCH` | u8 on → one STATE at once (no RESULT); with 1 the Host pushes STATE on change, at most twice a second, while the session is open |
| 3 | client → Host | `AUTH_START` | u8 method: 0 desktop browser on the Host PC, 1 URL only, 2 URL for AmiSurf (0.1.9+; older Hosts: as 0) |
| 4 | client → Host | `AUTH_CANCEL` | none |
| 5 | client → Host | `CONNECT` | u8 flags: 1 automatic (sent when AmiStudio or AmiDrive starts; honours `amidrive_automount`) |
| 6 | client → Host | `UNMOUNT` | u8 mode: 0 safe (refuse with files open, flush, unmount), 1 flush then lazy unmount even with files open, 2 unmount now (pending uploads stay in `cache/`) |
| 7 | client → Host | `FLUSH` | none: upload what is pending now, stay mounted |
| 8 | client → Host | `SIGNOUT` | u8 flags (1 = also revoke the token at Google) |
| 16 | Host → client | `STATE` | u8 state · u8 flags · u32 uploads queued · u32 in progress · u32 pending KB · u32 open files · u32 errored · u64 quota used · u64 quota total (0 unknown) · str account · str Linux mount path · str nonce · str message |
| 17 | Host → client | `AUTH` | u8 step (0 URL ready, 1 opened in the desktop browser, 2 done, 3 failed/cancelled) · str text · step 0 only (0.1.9+): u8 flags |
| 18 | Host → client | `RESULT` | u8 op · u8 status · str message · status 1 only: u16 n · n × str path |
| 19 | Host → client | `PROGRESS` | u32 files left · u32 KB left |

- **state:** 0 unavailable (no FUSE or no rclone), 1 signed out, 2 signing in, 3 ready (signed in, not mounted),
  4 mounting, 5 mounted, 6 flushing, 7 unmounting, 8 error (mount failed or rclone died; the message says why).
  **flags:** 1 FUSE ok, 2 rclone ok, 4 uploads pending (while unmounted: left in `cache/`, counts in queued/KB),
  8 less than 1 GB free for the cache, 16 sign-in expired (`invalid_grant`), 32 mount point not empty, 64 drawer
  copied or moved (sign in again). The mount path is sent in every STATE; the nonce (16 hex digits, new for every
  mount, the same as in `Drive.mount`) only while mounted. open files = distinct paths open under the mount.
- **RESULT status:** 0 ok, 1 refused: files open (the paths relative to the Drive root, at most 20, each cut to 255
  bytes keeping the end with a leading `…`; the message has the full count), 2 failed, 3 not signed in, 4 busy,
  5 uploads pending, 6 sign-in expired, 7 mount point not empty, 8 drawer copied/moved: sign in again. Messages are
  short user-facing English, never containing a token or secret.
- **Addressing:** RESULT goes only to the requesting session, followed by one STATE to it. STATE pushes go to every
  watcher. PROGRESS (about once a second during FLUSH, UNMOUNT, SIGNOUT and the Host's own stop) goes to the
  requester and to every watcher.
- **Busy:** CONNECT, UNMOUNT, FLUSH and SIGNOUT from all sessions run one at a time. One that arrives while another
  runs gets RESULT 4 at once (not queued).
- **CONNECT:** 0 when mounted (or already mounted). Flag 1 while signed out → status 3 with an empty message (the
  client shows nothing); `amidrive_automount no` → 0 with an empty message; 6/7/8 as listed (the Host never retries
  those). A network failure on an automatic CONNECT is retried every `amidrive_retry` seconds while any amidrive
  session is open.
- **Sign-in:** AUTH_START 0 runs `rclone authorize drive` in the user's real desktop session, which opens the PC's
  browser: AUTH 1 ("Sign in in the browser on the Host PC"), or AUTH 0 with the `http://127.0.0.1:53682/auth?...`
  URL if no browser could be opened. AUTH_START 1 sends AUTH 0 with the URL only (it must be opened on the Host PC).
  AUTH_START 2 (0.1.9+): sign in inside AmiSurf. The Host sends AUTH 0 with the URL like 1 and never opens a
  desktop browser; the client opens it in AmiSurf (ARexx port `AMISURF`: `OPENURL "<url>" NEWTAB`, or start
  `AmiSurf <url>`). With the bundled engine (a `browser` service) AmiSurf renders on the Host PC, so 127.0.0.1:53682
  reaches rclone; without one the URL still works in any browser on the Host PC. Hosts up to 0.1.8 treat 2 as 0, so a
  client can always send 2, open any AUTH 0 URL in AmiSurf and take AUTH 1 as "old Host opened the PC's browser".
  AUTH 0 carries a trailing u8 flags byte (0.1.9+): bit 0 (1) this Host has a `browser` engine, bit 1 (2) sent
  because a method-0 sign-in could not open the desktop browser. No byte = a Host up to 0.1.8. Joining: a session that
  sends AUTH_START 1 or 2 while a sign-in runs gets AUTH 0 with the URL (with flags) as soon as it is known, also
  when the running sign-in is a desktop one, and never AUTH 1; a method-0 joiner gets the current step. AUTH texts
  are short (refusals ~140 bytes) and every str is cut at 255 bytes on a UTF-8 boundary. The AUTH 2/3 text is
  always the same text as the RESULT(op 3) message that follows. Before rclone starts (all methods, 0.1.9+) the Host checks
  that Google accepts the OAuth client with that redirect; if not, the sign-in ends at once with AUTH 3 + RESULT(op
  3, 2) and Google's error in the text (e.g. "… (Error 400: redirect_uri_mismatch) …"), and no browser opens.
  The sign-in ends with AUTH 2 + RESULT(op 3, 0 "Signed in as …") or AUTH 3 + RESULT(op 3, 2). AUTH_CANCEL: AUTH 3
  and RESULT(op 3, 2) to the sessions in the sign-in, then RESULT(op 4, 0) to the canceller; with no sign-in
  running RESULT(op 4, 2). Another session's AUTH_START joins the running sign-in. Timeout `amidrive_auth_timeout`
  (default 600 s). The sign-in is cancelled when the last session in it closes.
- **SIGNOUT:** a safe unmount first (1 with paths, or 5 if uploads do not finish). Uploads already left in `cache/`
  give 5 unless the session sent UNMOUNT 2 before. Then the remote, `drawer.path` and `cache/` are deleted.
- **Drive.mount** (`{state}/amidrive/Drive.mount`, written after the mount, removed on unmount):
  `amidrive<TAB>1<TAB><nonce><TAB><account or -><TAB><Linux mount path>\n`. The path is last and may contain spaces.
  A marker whose nonce differs from the live STATE is stale.
- Unknown ops are ignored without an answer; clients must ignore bytes after the fields they know in STATE.

### Child-process services (e.g. `browser`)
After OK the bytes are passed through unchanged to the backend. For `handshake asbc2` services the Host has already
done the backend's ASBC v2 handshake with an internal token, so **the client does not send its own ASBC
handshake**: after the ASH OK, a Browser client sends ASBC frames (`HELLO`...) straight away.

Since Host 0.1.6 the Host itself first sends the backend one ASBC `CMD` (op 18): `asbhost client KEY`, where KEY
identifies the client the way the session-slot rule below does (`t` + paired token id, or for loopback `l` + 16 hex
of SHA-256 of client id text `|` address). Clients never see it; asb-chromed (engine patch 0007) uses it to keep
per-client view state for the same client only. Backends that do not know the command ignore it (asb-chromed did
so since AmiSurf 0.80); a custom backend behind `handshake asbc2` must at least skip unknown `CMD` frames.

### Session slots and reconnects (0.1.4)
A service with `max_clients` answers status 5 (busy) when it is full. Since Host 0.1.4 a reconnect from **the same
client** takes the slot over instead: the Host shuts the stale session down (its socket is closed; a backend such
as asb-chromed sees its connection end) and the new session gets OK. "The same client" is the same paired token,
or for loopback clients without a token the same client id text from the same address. Any other client still gets
status 5. This is what an Amiga needs after a reset or a dropped network: its old TCP session may stay half-open on
the PC for a long time.

## ASBC v2 inside `browser` (and app services such as `amidiscord`)
After the ASH OK the socket carries AmiSurf's ASBC v2 messages (the Host has done the backend handshake):
**u32 length** (op + payload) · **u8 op** · payload, all integers little-endian. The full op list is in AmiSurf's
`chrome/asbc.h` (the app-mode ops below come with the Host's engine patches). The frame header, since it is the part every client decodes:

`FRAME` (op 32, host → client):

| offset | type | field |
|---|---|---|
| 0 | u16 | page width (device pixels of the whole view) |
| 2 | u16 | page height |
| 4 | u16 | tile x |
| 6 | u16 | tile y |
| 8 | u16 | tile width `tw` |
| 10 | u16 | tile height `th` |
| 12 | u8 | `fmt`: 1 PNG, 2 QOI, 3 raw |
| 13 | u8 | fmt 1/2: 0 (reserved). **fmt 3: pixel format** (`ASBC_PF_*`, the one the client announced with `FMT`) |
| 14 | u8 | fmt 3 only: `comp` 0 raw, 1 RLE (see `asbc.h`); fmt 1/2: image data starts here |
| 15 | ... | fmt 3: `tw*th` pixels (`asbc_pf_bpp` bytes each), raw or RLE |

Pixel formats: 0 QOI (no raw tiles), 1 RGB24, 2 RGB565, 3 RGB565PC, 4 RGB555, 5 RGB555PC, 6 ARGB32, 7 BGRA32,
8 RGBA32, 9 CLUT8 (palette sent with `FMT`), 10 BGR24, 11 ABGR32, 12 BGR565PC, 13 BGR555PC.

### App mode messages (`amidiscord`, Host 0.1.4+)
The `amidiscord` service runs asb-chromed in app mode (one web app, no tabs; see SERVICES.md). On top of the AmiSurf
messages it speaks the messages below. **Capability-gated:** the Host sends a new host → client message only if the
client set its bit with `CAPS`; a client that never sends `CAPS` (AmiSurf, an old AmiDiscord) gets exactly the
AmiSurf message set (external URLs then arrive as `STATUS` text). asb-chromed outside app mode (the `browser`
service) ignores ops 22-25, so AmiSurf is unaffected. A Host older than 0.1.4 ignores `CAPS`: no `APPINFO` comes
back, which tells the client the Host is too old for AmiDiscord's extras (it can still work as a plain viewer).

Types: integers little-endian; **str** = u16 byte length + UTF-8 bytes (no terminator).
**Extensible payloads:** a Host may append fields to the end of a host → client message in a later version (as
`APPINFO` did in 0.1.5). Clients must parse the fields they know and ignore any bytes after them.

Capability bits (u32):

| bit | name | enables |
|---|---|---|
| 1 | NOTIFY | `NOTIFY`, `NOTIFYCLOSE`; `NOTIFYACT` honoured |
| 2 | UNREAD | `UNREAD` |
| 4 | FOCUS | `FOCUS` honoured |
| 8 | OPENURL | `OPENURL` (else external URLs come as `STATUS` "Opens outside the app: URL") |
| 16 | FILE | `FILEREQ` / `FILEDONE`; `FILEPUT` honoured (the page's file chooser is intercepted only then) |
| 32 | DOWNLOAD | `DOWNLOAD` |
| 64 | PASTEIMG | reserved (phase 2); Host 0.1.4 does not offer it |

Host 0.1.4 and later support 63 (1+2+4+8+16+32).

Client → Host:

| op | name | payload |
|---|---|---|
| 22 | `CAPS` | u32 caps the client handles. Send once right after `FMT` (again after a reconnect). Answer: `APPINFO`, then the current `UNREAD` if bit 2 is set |
| 23 | `FOCUS` | u8 focused (0/1), u8 visible (0/1). focused=0: the page sees `document.hasFocus()` false and a `blur`. visible=0 (window iconified/hidden): `visibilityState` "hidden", **no frames are sent**, the page keeps running so `NOTIFY`/`UNREAD` keep coming; visible=1 restarts frames with a full one. Discord only notifies when unfocused or hidden, so send `FOCUS 0 1` when the window is deactivated |
| 24 | `NOTIFYACT` | u32 id, u8 action: 0 clicked/opened (runs the page's onclick, e.g. Discord opens the channel; the Host also sets focused+visible), 1 dismissed by the user |
| 25 | `FILEPUT` | u32 req, u8 flags, str name, u32 size, data. flags: 1 = last chunk of the request, 2 = cancel. A chunk whose name is not empty starts a new file of `size` bytes; the following chunks of that file have name "" (size repeated or 0). Data <= 65536 bytes per chunk. Several files (only if `FILEREQ` said multiple): just start the next file with a named chunk; set flag 1 on the very last chunk. **Cancel** (see below): just u32 req + u8 flags=2 (5 bytes) |

Host → client:

| op | name | payload |
|---|---|---|
| 45 | `NOTIFY` | u32 id, u8 kind (0 channel message, 1 mention, 2 DM, 3 call, 255 other; best effort), str title, str body, str tag, str hint (e.g. `#general, My Server` from Discord's `user (#channel, Server)` title; may be empty), str url (`data.url` if the page set one; may be empty), u8 has_icon, then if has_icon: u16 w, u16 h, w*h RGB24 (<= 64x64, on white) |
| 46 | `NOTIFYCLOSE` | u32 id: the page closed it (e.g. read on another device, or replaced by a notification with the same tag) |
| 47 | `UNREAD` | u16 count (the title's `(N)`; 0 none), u16 mentions (= count; Discord's title does not separate them), u8 flags: 1 unread without mention (title `• `), 2 favicon badge. Sent on change, at most twice a second; the latest state is sent after `CAPS` (so after a reconnect) |
| 48 | `OPENURL` | str url (**at most 1023 bytes**), u8 reason: 0 link/navigation, 1 new window (`target=_blank`, `window.open`), 2 a `NAV` from the client to a host outside the app. The page did not load it: open it elsewhere (AmiSurf) |
| 49 | `FILEREQ` | u32 req, u8 flags (1 multiple files allowed), str accept (the input's `accept`, e.g. `.txt,image/png`; may be empty). The page opened a file chooser: show a requester and answer with `FILEPUT` (or `FILEPUT` flag 2 to cancel) |
| 50 | `FILEDONE` | u32 req, u8 status (0 ok: the page has the files; 1 too big; 2 error or cancelled), str message. Limit: `ASB_APP_MAXUPLOAD` MB, default 25 (per request; sent in `APPINFO` since 0.1.5) |
| 51 | `DOWNLOAD` | u8 state (0 started, 1 done, 2 failed/cancelled), str name, u32 size (0 if unknown), str host path (state 1: the absolute path of the file on the Host, see "Download folder" below; renamed `NAME (1).ext` etc. if taken) |
| 52 | `APPINFO` | str app id (`amidiscord`), str start URL, u32 caps the Host supports, **u32 max upload** (0.1.5+: bytes per `FILEREQ`, = `ASB_APP_MAXUPLOAD` MB × 1048576; absent from Host 0.1.4). Reply to `CAPS` |

#### Details (0.1.5)
**FILEPUT cancel.** A cancel needs only the request id: `u32 req` · `u8 flags` with bit 2 set (5 bytes in all).
The Host ignores anything after those 5 bytes, so the full form (`str name`, `u32 size`, no data; 11+ bytes) is
still valid. Answer: `FILEDONE req 2 "cancelled"`; a file already partly sent is deleted on the Host. A cancel for
a request that is not open gets `FILEDONE req 2 "no such file request"`. Host 0.1.4 needs the full 11-byte form
(a shorter cancel is dropped there), so a client that must work with 0.1.4 sends the full form: `req, 2, 00 00,
00 00 00 00`. Any other `FILEPUT` shorter than its header (fewer than 11 bytes, or a name running past the end)
ends the open request with `FILEDONE req 2 "bad FILEPUT (header too short)"` instead of being dropped.

**APPINFO max upload.** `APPINFO` = `str app id` · `str start URL` · `u32 caps` · `u32 max upload` (0.1.5+). The
last field is the Host's upload limit in bytes for one `FILEREQ` (all its files together), from
`ASB_APP_MAXUPLOAD` (MB, default 25, clamped to 1..4095), e.g. 26214400. A Host 0.1.4 `APPINFO` ends after the
caps: then assume 25 MB. A larger upload is refused with `FILEDONE 1` (too big), so check it on the Amiga first.

**OPENURL length.** The url of `OPENURL` is at most **1023 bytes** (UTF-8; in practice ASCII), so it fits a
1024-byte client buffer with its terminator. The Host never cuts a URL (a cut URL would open the wrong page): a
longer one is not sent as `OPENURL`; the client gets `STATUS` "Link too long to open outside the app (N bytes,
limit 1023): <first 120 bytes>..." instead (and without `CAP_OPENURL` the usual "Opens outside the app" `STATUS`).

**UNREAD on logged-out pages.** While the page shows a logged-out page - path `/` or starting with one of the
`ASB_APP_NOUNREAD` prefixes (default `/login /register /reset /verify`, whole path segments) - the Host reports
`UNREAD 0 0 0`, whatever the title and favicon do (Discord's login page swaps its favicon too). The plain favicon
for the badge flag is learned again on the first app page after them, also when Discord goes from `/login` to
`/channels/...` without reloading.

**Download folder.** One rule: the page's downloads go to `<app data>/downloads`, where
- `<app data>` = `ASB_STATE` if the service sets it, else `<data>/<app id>`;
- `<data>` = the Host's data folder (`{state}`): `--data-dir` / `data_dir`, default `~/.local/share/amistudio-host`
  (`$XDG_DATA_HOME/amistudio-host` if set; `%LOCALAPPDATA%\AmiStudio-Host` on Windows). The Host passes it to its
  services as `AMISTUDIO_HOST_DATA`.
- `ASB_DOWNLOADS` overrides the whole path.

For the bundled `amidiscord` service (`ASB_STATE={state}/amidiscord`) that is `<data>/amidiscord/downloads`:
`~/.local/share/amistudio-host/amidiscord/downloads` by default, `DIR/amidiscord/downloads` with `--data-dir DIR`.
Uploads use `<app data>/uploads` the same way (`ASB_UPLOADS`). Clients must not build the path themselves: use the
host path in `DOWNLOAD` state 1, which is always the exact absolute path.

Also in app mode, without a cap: the page's `navigator.clipboard.writeText` (Discord's "Copy Link", "Copy Message
ID") is sent as the usual `CLIP` (op 36). `CMD` verbs (op 18): `app home`, `app logout` (clears the app's cookies and
storage), `app reducedmotion 0|1`, `app focus F V` (= `FOCUS`, for tests), `app notifyact ID A` (= `NOTIFYACT`).
While no client is connected the page is unfocused and hidden; notifications are dropped, and the latest unread
state is kept for the next `CAPS`. A new client starts as focused and visible until it sends `FOCUS`.

Typical AmiDiscord start: ASH OK → `HELLO` → `FMT` → `CAPS 63` → (`APPINFO`, `UNREAD`, frames...) → `FOCUS 1 1`.

#### View size, zoom and per-client state (0.1.6)
These apply to `browser` (AmiSurf) and app mode alike unless noted.

**RESIZE** (op 10, `u16 w` · `u16 h`) now also restarts the screencast, so every frame after it is the new view
size (`FRAME` page width/height = w x h). Up to 0.1.5 Chrome kept its first screencast size limit: after growing the
view the frames came scaled down (e.g. 571x400 for a 1000x700 view) until the client re-sent `FMT`. Re-sending `FMT`
still works (it restarts the screencast too), so clients that do it can keep doing so or drop it for 0.1.6+.

**Zoom** (`CMD zoom N|+|-|0`, 25-400 %): the page lays out at w*100/N x h*100/N CSS pixels, and the frames are
always exactly the view size (w x h) with the content magnified (at 200 % a 100 CSS px box is 200 frame pixels)
or shrunk. Up to 0.1.5, zooming in gave frames of the unzoomed (CSS) size (500x350 for a 1000x700 view at 200 %)
and a "frame ... != view ..., re-applying viewport" loop in the engine log; both are gone. How: under zoom-in each
change Chrome reports becomes a full-size `Page.captureScreenshot` (one in flight, so the frame rate is somewhat
lower than at 100 % on busy pages; a static page sends nothing); frames within 3 px of the view (zoom rounding)
are fitted to it exactly.

**Per-client state in app mode.** When a client connects, the engine compares its Host client KEY (above) with
the previous client's:
- a **new client** (another token, another client id, or another address, or no Host key at all) starts with the
  default zoom (`settings` `zoom`, else 100 %) and no find-in-page state;
- the **same client** reconnecting or taking its slot over (an Amiga after a reset or a dropped connection) keeps
  them.
The page itself (login, scroll position, open channel) is the app's and is kept either way. AmiSurf (`browser`)
keeps its zoom across clients as before.

## LAN discovery
UDP to port 9220 (broadcast). Request: `"ASH?"`. Each Host answers to the sender:
```
ASH!name=odin
ver=0.1.1
port=9220
pair=1          # 1 = this sender must pair (LAN); 0 = loopback
id=3f1c0a9be2d47781
os=linux-x86_64
```
At most 20 answers per second. Only a Host started with `--lan` answers. mDNS (`_amistudio._tcp`) is not
implemented yet.

## Compatibility port (plain ASBC)
A `compat_port` (9223 for `browser`) accepts the old ASBC v2 handshake so Browser 0.79-0.81 keep working: loopback
clients are accepted with any token; LAN clients must send a paired Host token as their ASBC token (Browser
`TOKEN=`). The Host swaps it for the backend's internal token and passes everything through.
