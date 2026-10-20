# AmiStudio Host

> **Public snapshot.** This repository is a clean public snapshot of the AmiStudio Host's development tree, published
> under the GNU General Public License v3 (see `LICENSE`). The earlier commit history is not included. The release
> tarball's Google Drive OAuth client secret is never in the repository (`dist/host-defaults.conf` keeps it empty), so
> self-built Hosts use rclone's built-in client unless you configure your own. Note: `tools/fetch-engine.sh` still
> pins AmiSurf's engine to a commit in a private repository, so building the bundled browser engine from this
> snapshot does not work yet; it will be switched to [hardcopi/AmiSurf](https://github.com/hardcopi/AmiSurf).

The PC half of AmiStudio. One daemon, `amistudio-hostd`, on **one port (9220)** serves every headless backend the
AmiStudio apps on a classic Amiga need. Plan: `LAUNCHER-PLAN.md` section 2 (AmiStudio shared folder).

**Status: 0.1.2, Phase 2 core** (changes: CHANGES). The Linux release tarball includes AmiSurf's browser engine (asb-chromed + Chromium). Linux first; Windows layer written and compile-checked, not yet run
(docs/PORTING-WINDOWS.md). Not published.

## What it does
- **ASH v1** (docs/PROTOCOL.md): each TCP connection names a service + protocol version in a small handshake,
  then carries that service's own protocol (no multiplexing).
- **Pairing:** `amistudio-hostd pair` prints a one-time 6-digit code; the Amiga trades it for a random token.
  Loopback (emulator) needs no token; LAN clients do. Only token hashes are stored. 5 failed handshakes in 60 s
  ban an address for 5 minutes.
- **LAN discovery:** answers `ASH?` UDP broadcasts on 9220 with name, version, port, pairing need.
- **Services:**
  - `hostinfo` - status text for the launcher's status bar (version, services up/idle/down, update available)
  - `pair` - code → token
  - `fetch` - HTTPS download proxy for 68020s / Amigas without AmiSSL (allow-listed hosts, size + SHA-256 checks)
  - child-process backends from host.conf, e.g. **`browser`** = AmiStudio Browser's asb-chromed, unchanged, plus a
    plain-ASBC compatibility port 9223 for current Browser clients (docs/SERVICES.md)
- Own update check against `hardcopi/AmiStudio-Releases` (an `app host` block), shown in hostinfo.

## Build, test, install
    make                 # build/amistudio-hostd, build/ash-cli (needs libssl-dev)
    make test            # 60 integration checks (ASH_TEST_BROWSER=1 adds the real asb-chromed + Chrome)
    make win-check       # compile check of the Windows layer with MinGW-w64
    make tarball         # dist/amistudio-host-<ver>-linux-x86_64.tar.gz (static binary)
    dist/install-user.sh --enable    # ~/.local/bin + systemd user unit

See docs/INSTALL.md.

## Layout
`src/hostd.c` daemon + CLI · `src/plat.h` + `plat_posix.c` / `plat_win32.c` OS layer · `src/auth.c` pairing,
tokens, bans · `src/config.c` host.conf · `src/svc_*.c` services · `src/http.c` HTTPS client · `src/discovery.c` ·
`client/ashc.[ch]` small portable ASH client (for the Amiga apps too) · `tools/ash-cli.c` test client ·
`test/` integration tests · `dist/` systemd unit, scripts, example config.

## Not done yet
mDNS, the launcher's Host panel/pairing UI (a draft client - status line + FETCH=HOST - is on the launcher's `host-client` branch),
Browser speaking ASH on 9220 (Browser repo, later), pdf/drive/player/office/mail-auth services, Windows build +
run, signed releases, licence (undecided).
