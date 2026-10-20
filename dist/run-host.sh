#!/bin/sh
# run-host.sh - run the AmiStudio Host from this folder (the unpacked release tarball). Ctrl-C stops Host and engine.
#   sh run-host.sh [--port N] [--idle MIN] [--engine-idle MIN] [--host-idle MIN] [--lan] [--pair] [--background] [...]
# The bundled browser engine (engine/: asb-chromed + Chromium, headless) is the "browser" service for AmiSurf; the
# Host starts it when AmiSurf connects and restarts it on demand.
# --port N      ASH port (default 9220)
# --idle MIN    both of these (the AmiKit way: run-host.sh --idle 30):
# --engine-idle MIN  the engine (Chromium) quits after MIN minutes without an AmiSurf session (default 30, 0 = never);
#               the Host starts it again when AmiSurf connects
# --host-idle MIN    the Host itself stops after MIN minutes with NO client connection at all: any open or new
#               connection (AmiSurf, the launcher's status checks every ~45 s, pairing, fetch) resets it (default never)
# --lan         also serve Amigas on the network (they pair once; see --pair)
# --pair        print a 6-digit pairing code for an Amiga on the network, then exit
# --background  detach and return at once (log: CONFDIR/hostd.log); safe to run twice
# Settings, tokens and logs: ~/.config/amistudio-host (--confdir DIR). Browser data (bookmarks, history, profile):
# ~/.local/share/amistudio-host/browser (ASB_STATE=DIR). Both stay when you unpack or install a newer Host.
# Self-contained (e.g. the AmiStudio launcher on AmiKit): set HOME, XDG_CONFIG_HOME, XDG_DATA_HOME, XDG_STATE_HOME,
# XDG_CACHE_HOME, TMPDIR and ASB_DOWNLOADS under one folder and pass --confdir DIR --data-dir DIR (docs/INSTALL.md).
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=""; BG=0; PAIR=0; CONFDIR=""; set -f
ARGS=""
q(){ printf "'%s' " "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT=$2; ARGS="$ARGS--port $(q "$2")"; shift ;;
    --idle) ARGS="$ARGS--browser-idle $(q "$2")--exit-idle $(q "$2")"; shift ;;
    --engine-idle) ARGS="$ARGS--browser-idle $(q "$2")"; shift ;;
    --host-idle) ARGS="$ARGS--exit-idle $(q "$2")"; shift ;;
    --pair) PAIR=1 ;;
    --background) BG=1 ;;
    --confdir) CONFDIR=$2; ARGS="$ARGS--confdir $(q "$2")"; shift ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) ARGS="$ARGS$(q "$1")" ;;
  esac; shift
done
for f in amistudio-hostd ash-cli engine/engine.sh engine/asb-chromed engine/chromium/chrome engine/chromium/chrome_crashpad_handler engine/rclone/rclone; do
  [ -f "$HERE/$f" ] && [ ! -x "$HERE/$f" ] && chmod +x "$HERE/$f" 2>/dev/null && echo "run-host: restored exec bit on $f"
done
if [ $PAIR = 1 ]; then
  if [ -n "$CONFDIR" ]; then exec "$HERE/amistudio-hostd" pair --confdir "$CONFDIR"; else exec "$HERE/amistudio-hostd" pair; fi
fi
eval "set -- $ARGS"
if [ $BG = 0 ]; then exec "$HERE/amistudio-hostd" run "$@"; fi
P=${PORT:-9220}
if command -v nc >/dev/null 2>&1 && nc -z 127.0.0.1 "$P" 2>/dev/null; then echo "run-host: already running on port $P"; exit 0; fi
C=${CONFDIR:-${AMISTUDIO_HOST_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/amistudio-host}}; mkdir -p "$C" && chmod 700 "$C"
if command -v setsid >/dev/null 2>&1; then setsid "$HERE/amistudio-hostd" run "$@" </dev/null >>"$C/hostd.log" 2>&1 &
else nohup "$HERE/amistudio-hostd" run "$@" </dev/null >>"$C/hostd.log" 2>&1 & fi
echo "$!" >"$C/hostd.pid"
echo "run-host: AmiStudio Host starting on port $P (PID $!, log $C/hostd.log, engine log $C/browser.log)"
