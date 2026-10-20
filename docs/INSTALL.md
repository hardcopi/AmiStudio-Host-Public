# Installing the AmiStudio Host (Linux)

The AmiStudio Host runs on a Linux PC on the same network as your Amiga (or on the PC that runs your Amiga
emulator). It does the heavy work for AmiStudio apps: TLS downloads for the launcher, AmiSurf's browser engine
(included: a headless Chromium) and later PDF rendering and more. **Do not run it on a public server and never forward port 9220 to the internet.**
A Windows version is planned before 1.0.

## 1. Get it
- From a release tarball: `tar xzf amistudio-host-0.1.2-linux-x86_64.tar.gz && cd amistudio-host-0.1.2-linux-x86_64`
  (about 230 MB download, 750 MB unpacked: static Host binary plus AmiSurf's engine with its own Chromium; runs on
  any x86-64 distro with the usual desktop libraries, see "Browser engine" below). Since 0.1.10 the static binary
  never loads the system's NSS modules: it looks up host names with the system's `getent` (part of glibc, present on
  every glibc distro), which runs the system's own resolver (/etc/hosts, systemd-resolved, mDNS, DNS) in its own
  process. Host 0.1.9 crashed on Fedora 44 / Nobara at the first name lookup (the Google sign-in check).
- Or build: `sudo apt install build-essential libssl-dev && make` (binary in `build/amistudio-hostd`);
  `make tarball` also builds the engine (tools/fetch-engine.sh: AmiSurf source + Chromium download)

## 2. Try it in a terminal
    ./run-host.sh                 # this computer only (emulator with bsdsocket); Ctrl-C stops it
    ./run-host.sh --lan           # real Amigas on the network can connect after pairing
    ./run-host.sh --pair          # print a pairing code for an Amiga on the network
    ./run-host.sh --background --idle 30   # detach; the browser engine quits after 30 idle minutes
    ./amistudio-hostd status      # in another terminal: what the launcher's status bar will show

`run-host.sh` options: `--port N` (default 9220), `--lan`, `--pair`, `--background`, and the idle options below;
anything else goes to `amistudio-hostd` (e.g. `--name`, `--confdir`).

**Idle rules** (two separate timers):
- *Engine* (`--engine-idle MIN`, host.conf `browser_idle`, default 30, 0 = never): Chromium quits after MIN minutes
  without an AmiSurf session. The Host keeps running and starts it again when AmiSurf connects.
- *Host* (`--host-idle MIN`, `amistudio-hostd --exit-idle MIN`, host.conf `exit_idle`, default never): the Host stops
  (exit 0, engine stopped too) once MIN minutes have passed with **no client connection open and none opened**.
  Every connection to the Host counts as activity: AmiSurf sessions, the launcher's status checks (about every
  45 s, so a running launcher keeps the Host up), pairing, fetch, and the 9223 compat port. LAN discovery
  broadcasts do not count. The timer starts when the last connection closes.
- `--idle MIN` sets both to MIN (what AmiSurf's AmiKit edition uses: `run-host.sh --idle 30`).
- *Launcher quit* (0.1.6+): when AmiStudio closes it asks the Host to quit when unused (`control` service): the
  Host stops at once if no app (AmiSurf, AmiDiscord...) is connected, else about 10 s after the last one
  disconnects, even with the Host timer off. `allow_quit no` in host.conf (or `--no-remote-quit`) ignores that.
Leave the Host timer off for the systemd service.

## 3. Install as a service (starts with your login, or at boot with linger)
    ./install-user.sh --enable    # ~/.local/lib/amistudio-host + ~/.local/bin link + systemd user unit (--lan), no root
    loginctl enable-linger $USER  # optional: start at boot without logging in
    journalctl --user -u amistudio-host -f

To update, unpack the new tarball and run its `./install-user.sh` again (then `systemctl --user restart
amistudio-host`). The program folder is replaced; settings, paired Amigas and browser data are kept.
The unit is `~/.config/systemd/user/amistudio-host.service`; settings are in
`~/.config/amistudio-host/host.conf` (see `host.conf.example`). Remove `--lan` from the unit's `ExecStart` to serve
only this PC.

## 4. Pair your Amiga
    amistudio-hostd pair          # prints a 6-digit code, valid 10 minutes, works once

Enter the code on the Amiga in AmiStudio (Hosts... > Pair). Manage paired Amigas with `amistudio-hostd tokens`
(id, when paired, when last used, name) and `amistudio-hostd revoke ID` (or `all`). Pairing the same Amiga again
replaces its old token, so re-pairing does not leave stale entries. An emulator on the same PC connects over 127.0.0.1 and needs no pairing.

## 5. Browser engine (AmiSurf)
The tarball's `engine/` folder is AmiSurf's browser engine: `asb-chromed` and a portable, unmodified
ungoogled-chromium that runs headless (no X server or Xvfb). The Host mounts it as the `browser` service by
itself, so AmiSurf 0.82+ works as soon as the Host runs - nothing to configure. Chromium starts when AmiSurf
connects, quits after `browser_idle` minutes without it (default 30; host.conf `browser_idle`, `run-host.sh --idle`)
and is started again on demand. To use your own engine instead, add a `service browser` block to host.conf (see
`host.conf.example`) or set `bundled_browser no`.

Chromium needs the common desktop libraries (on a minimal server, for example `sudo apt install libnss3 libatk-bridge2.0-0
libgbm1 libxkbcommon0 libasound2 libcups2 libxcomposite1 libxdamage1 libxrandr2 libpango-1.0-0`). If one is
missing, `browser.log` names it. Page sound needs `pulseaudio-utils` (pactl, parec). DRM video (Widevine) is not
included. AmiSurf 0.81 and older speak plain ASBC on 9223: add `compat_port 9223` to a `service browser` block.

## 6. AmiDiscord (`amidiscord` service)
The same engine also runs as the `amidiscord` service in **app mode** for AmiDiscord on the Amiga: one Chromium
showing https://discord.com/app with no tabs, bookmarks, history or password manager, its own profile and data in
`~/.local/share/amistudio-host/amidiscord` (never shared with AmiSurf's), links to other sites sent to the Amiga
(AmiDiscord hands them to AmiSurf), Discord's own sounds muted (the Amiga plays its own notifications) and reduced
motion on (Discord's animations would otherwise resend near-full frames several times a second). Nothing to
configure. Chromium starts when AmiDiscord connects and stays up `amidiscord_idle` minutes after the last
AmiDiscord client left (default 30, `--amidiscord-idle MIN`, 0 = never), so a short disconnect does not log you
out or miss notifications. `bundled_amidiscord no` / `--no-bundled-amidiscord` turns it off.
Memory: this is a second Chromium next to AmiSurf's, about 300-500 MB with Discord loaded.
Your Discord login is kept in that folder across Host restarts (Chromium is closed cleanly on stop; allow it a few
seconds). To log this Host out of Discord, use AmiDiscord's log-out (it sends `CMD app logout`), or stop the Host and
delete `~/.local/share/amistudio-host/amidiscord/profile`. `data_dir` / `--data-dir DIR` moves all service data
(then e.g. `DIR/amidiscord/`, `DIR/browser/`). Files you download in AmiDiscord are saved on this PC in
`~/.local/share/amistudio-host/amidiscord/downloads/` (`DIR/amidiscord/downloads/` with `--data-dir`).

## 7. Google Drive (`amidrive` service, 0.1.8)
AmiDrive (and the launcher) can mount your Google Drive as a drawer the Amiga sees. The Host does it with the
bundled rclone through FUSE, so the PC needs **fuse3**: `sudo apt install fuse3` (Debian/Ubuntu; `fuse3` on Fedora
and Arch too). No sudo or `--allow-other` is used at run time. Without it hostinfo shows `amidrive 1 down no-fuse`.
Signing in opens Google's page in the **desktop browser of the Host PC** (or prints a link to open on that PC):
`amistudio-hostd amidrive signin`, or from AmiDrive. Then `amistudio-hostd amidrive connect` mounts it at
`~/.local/share/amistudio-host/amidrive/Google Drive` (set `amidrive_mountpoint` in host.conf for another folder),
and `amistudio-hostd amidrive unmount` uploads what is pending and unmounts. Details: docs/SERVICES.md.

## Firewall
Allow TCP 9220 (and UDP 9220 for discovery, TCP 9223 for older Browsers) from your LAN only, e.g.
`sudo ufw allow from 192.168.1.0/24 to any port 9220`.

## Files
| path | what |
|---|---|
| `~/.config/amistudio-host/host.conf` | settings and service backends |
| `~/.config/amistudio-host/tokens` | paired Amigas (SHA-256 of each token only), mode 0600 |
| `~/.config/amistudio-host/pair-code` | the current one-time pairing code |
| `~/.config/amistudio-host/host-id` | random id shown in discovery |
| `~/.config/amistudio-host/<service>.log` | backend output (e.g. browser.log) |
| `~/.local/share/amistudio-host/browser/config/` | AmiSurf's bookmarks, history, settings, passwords (`ASB_STATE` overrides) |
| `~/.local/share/amistudio-host/browser/profile/` | the Chromium profile (cookies, cache, logins) |
| `~/.local/share/amistudio-host/browser/WidevineCdm/` | optional, only if you add Widevine yourself |
| `~/.local/share/amistudio-host/amidiscord/` | AmiDiscord's own `profile/` (Discord login), `config/`, `downloads/` (files downloaded in AmiDiscord), `uploads/` (temporary) |
| `~/.local/share/amistudio-host/amidrive/` | Google Drive: rclone.conf (sign-in token, 0600), cache/ (pending uploads), logs, the mount point `Google Drive` |
| `~/Downloads` | files downloaded in AmiSurf (`ASB_DOWNLOADS` overrides) |
| `~/.local/lib/amistudio-host/` | the installed program and engine (install-user.sh; replaced on update) |
| `~/.config/chromium/Crash Reports`, `~/.cache/fontconfig`, `~/.config/pulse`, `~/.pki`, `/tmp/org.chromium.*`, `/tmp/pulse-*` | what Chromium, fontconfig, PulseAudio and NSS keep by themselves (follow `HOME`, `XDG_*`, `TMPDIR`) |

Coming from AmiSurf's AmiKit edition: its `AmiSurf/Host/data/config/` and `data/profile/` have the same layout;
with AmiSurf and both Hosts stopped, copy them into `~/.local/share/amistudio-host/browser/` (`cp -a`). Paired
Amigas from `data/hostd/tokens` can go to `~/.config/amistudio-host/tokens` if that file does not exist yet.

## Self-contained install (AmiKit, started by the AmiStudio launcher)
The paths above are the standalone Linux install (this guide, `install-user.sh`, the systemd user unit) and stay as
they are. On AmiKit the AmiStudio launcher (0.1.8+) installs the Host into its own drawer (`AmiStudio/Host/bin`) and
starts it through its `AmiStudio/Host/run-host.sh` with every path inside `AmiStudio/Host/`, so copying or deleting
the AmiStudio drawer takes the Host and all its data with it and leaves nothing in `~` or `/tmp`:

| setting | value (under `AmiStudio/Host/`) | what goes there |
|---|---|---|
| `--confdir` / `AMISTUDIO_HOST_DIR` | `data/hostd` | host.conf, tokens, host-id, PID, logs |
| `data_dir` in host.conf (or `--data-dir`) | `data/services` | `browser/` and `amidiscord/` profiles, config, uploads, AmiDiscord downloads; `amidrive/` (Google Drive token, cache, logs) |
| `amidrive_mountpoint` in host.conf | `<AmiStudio drawer>/Google Drive` (outside Host/, still in the drawer: user data) | the Google Drive mount, only while AmiStudio runs (launcher 0.1.9) |
| `AMISTUDIO_REAL_ENV`, `AMISTUDIO_REAL_HOME`, `AMISTUDIO_REAL_XDG_*`, `AMISTUDIO_REAL_TMPDIR` | the user's real values, saved before HOME moves | only for the Google sign-in, so it opens the PC's own browser (launcher 0.1.9) |
| `ASB_DOWNLOADS` | the user's real Downloads folder (`XDG_DOWNLOAD_DIR`, else the real `user-dirs.dirs`, else the real `~/Downloads`) | AmiSurf's and AmiDiscord's downloads: user files, the one exception to the drawer rule (launcher 0.1.8) |
| `PULSE_SERVER`, `PULSE_COOKIE` | the user's own PulseAudio (real `$XDG_RUNTIME_DIR/pulse/native` or the socket behind the real `~/.config/pulse/<machine>-runtime`; real cookie) | sound without a second PulseAudio daemon (launcher 0.1.8) |
| `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` | `data/home` (+ `.config`, `.local/share`, `.local/state`, `.cache`) | Chromium crash reports, fontconfig cache, PulseAudio cookie, NSS `~/.pki` |
| `TMPDIR` (`XDG_RUNTIME_DIR` unset) | `data/tmp` | Chromium's and PulseAudio's temporary files. Chromium's `SingletonSocket` path must stay under 107 bytes: when `data/tmp` is longer than 56 characters the launcher's script points `TMPDIR` at a short symlink `/tmp/amistudio-host-<uid>-<cksum>` while the Host runs and removes it when the Host exits |

Any other installer can do the same: set those variables and pass `--confdir` and `--data-dir` (or `data_dir`) when
starting `amistudio-hostd run` or `run-host.sh`; the Host and both bundled services follow them (test:
`test/app_tests.py` `selfcontained_tests`, also in `ASH_TEST_BROWSER=1 make test`).

Licences of the bundled parts are in `LICENSES/` (Chromium's LICENSE and credits, ungoogled-chromium,
OpenSSL, zlib, zstd, stb_image, QOI, rclone's MIT licence in `rclone-COPYING.txt`); `LICENSES/ENGINE-VERSION.txt` and
`LICENSES/RCLONE-VERSION.txt` say exactly what is bundled. The AmiStudio Host itself is GPL v3 (`LICENSES/GPL-3.0.txt`).
