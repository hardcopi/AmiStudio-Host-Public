#!/bin/sh
# engine.sh - AmiSurf's browser engine (asb-chromed + the bundled Chromium, headless), started by the AmiStudio Host
# as its "browser" service: amistudio-hostd mounts INSTALLDIR/engine/engine.sh by itself and runs
#   sh engine.sh -port {port} -token {token} -cdp {port2} -idle MIN
# Based on AmiSurf 0.83's AmiKit engine.sh. Extra arguments go to asb-chromed.
#
# Data is kept OUTSIDE the install folder, so unpacking or installing a new Host keeps it. One rule (Host 0.1.5):
#   DATA  = $AMISTUDIO_HOST_DATA (exported by amistudio-hostd: its {state}, i.e. --data-dir / data_dir, default
#           ${XDG_DATA_HOME:-~/.local/share}/amistudio-host)
#   STATE = $ASB_STATE, else DATA/<id>  (<id> = $ASB_APP in app mode, "browser" otherwise)
#     config/     history.tsv, bookmarks.tsv, settings, passwords ($ASB_CONFIG overrides)
#     profile/    the Chromium profile (cookies, cache, logins)
#     downloads/  app mode: the page's downloads ($ASB_DOWNLOADS overrides); DOWNLOAD reports the exact path
#     uploads/    app mode: files handed to the page ($ASB_UPLOADS overrides; removed after 10 minutes)
#   The bundled "amidiscord" service sets ASB_STATE={state}/amidiscord, the same folder this rule gives.
#   AmiSurf's own (non-app) downloads go to ~/Downloads (ASB_DOWNLOADS overrides). The engine log is the Host's
#   CONFDIR/<service>.log.
# App mode (ASB_APP=amidiscord, set by the Host's "amidiscord" service): the engine serves one web app with its
# own data, apart from AmiSurf's.
# ASB_VISIBLE=1 (with DISPLAY set) shows Chromium on the desktop instead of headless.
# Widevine (DRM video) is not shipped; if a WidevineCdm folder with current.txt exists in the data folder it is used.
HERE=$(cd "$(dirname "$0")" && pwd)
APPDIR=browser; [ -n "${ASB_APP:-}" ] && APPDIR=$(printf %s "$ASB_APP" | tr -c 'A-Za-z0-9_.-' _)
DATA="${AMISTUDIO_HOST_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/amistudio-host}"
STATE="${ASB_STATE:-$DATA/$APPDIR}"
CONFIG="${ASB_CONFIG:-$STATE/config}"   # bookmarks, history, settings, passwords: per service, never shared (0.1.4)
mkdir -p "$CONFIG" "$STATE/profile" && chmod 700 "$STATE" "$CONFIG" 2>/dev/null
if [ -n "${ASB_APP:-}" ]; then
  export ASB_STATE="$STATE" ASB_DOWNLOADS="${ASB_DOWNLOADS:-$STATE/downloads}" ASB_UPLOADS="${ASB_UPLOADS:-$STATE/uploads}"
  mkdir -p "$ASB_DOWNLOADS" "$ASB_UPLOADS" && chmod 700 "$ASB_UPLOADS"
fi
# tar keeps exec bits, but copies through other tools may not
for f in asb-chromed chromium/chrome chromium/chrome_crashpad_handler chromium/chrome-wrapper; do
  [ -f "$HERE/$f" ] && [ ! -x "$HERE/$f" ] && chmod +x "$HERE/$f" 2>/dev/null && echo "engine.sh: restored exec bit on $f" >&2
done
MISSING=$(ldd "$HERE/chromium/chrome" 2>/dev/null | sed -n 's/^[[:space:]]*\([^ ]*\) => not found.*/\1/p' | tr '\n' ' ')
[ -n "$MISSING" ] && { echo "engine.sh: Chromium needs system libraries this PC lacks: $MISSING (see README-HOST.md, 'Browser engine')" >&2; exit 1; }
WV="$STATE/WidevineCdm"; [ -f "$WV/current.txt" ] && export ASB_WIDEVINE_DIR="$WV/$(cat "$WV/current.txt")"
export ASB_CHROME="$HERE/chromium/chrome" ASB_CONFIG="$CONFIG" ASB_DOWNLOADS="${ASB_DOWNLOADS:-$HOME/Downloads}"
if [ "${ASB_VISIBLE:-0}" = 1 ] && [ -n "${DISPLAY:-}" ]; then export ASB_CHROME_DISPLAY="$DISPLAY"; else export ASB_HEADLESS=1; fi
unset ASB_TOKEN
# Chrome's private debug port: the Host passes a free one with -cdp; otherwise the first free one from 9323
case " $* " in *" -cdp "*) CDPARG="" ;; *)
  CDP=9323; if command -v nc >/dev/null 2>&1; then while nc -z 127.0.0.1 $CDP 2>/dev/null; do CDP=$((CDP+1)); done; fi
  CDPARG="-cdp $CDP" ;; esac
echo "=== $(date '+%F %T') engine.sh: state $STATE, config $CONFIG${ASB_APP:+ (app $ASB_APP, downloads $ASB_DOWNLOADS)}" >&2
exec "$HERE/asb-chromed" -browser chrome $CDPARG -profile "$STATE/profile" "$@"
