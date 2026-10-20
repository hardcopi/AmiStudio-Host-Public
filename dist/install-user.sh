#!/bin/sh
# install-user.sh [--enable] : install the AmiStudio Host for the current user (no root needed)
#   ~/.local/lib/amistudio-host/   the program and the bundled browser engine (replaced on every install)
#   ~/.local/bin/amistudio-hostd   a link to it
#   ~/.config/systemd/user/amistudio-host.service, ~/.config/amistudio-host/host.conf (kept if it exists)
# Your data is not in the install folder and is kept: ~/.config/amistudio-host (settings, paired Amigas, logs) and
# ~/.local/share/amistudio-host/browser (bookmarks, history, Chromium profile).
#   --enable also runs: systemctl --user daemon-reload && systemctl --user enable --now amistudio-host
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=""; for b in "$HERE/amistudio-hostd" "$HERE/../build/amistudio-hostd"; do [ -x "$b" ] && { BIN=$b; break; }; done
[ -n "$BIN" ] || { echo "amistudio-hostd not found (run make first)"; exit 1; }
UNIT=""; for u in "$HERE/amistudio-host.service" "$HERE/systemd/amistudio-host.service"; do [ -f "$u" ] && { UNIT=$u; break; }; done
CONFEX=""; for c in "$HERE/host.conf.example"; do [ -f "$c" ] && CONFEX=$c; done
LIB="${AMISTUDIO_HOST_LIB:-$HOME/.local/lib/amistudio-host}"
mkdir -p "$HOME/.local/bin" "$HOME/.config/systemd/user" "$(dirname "$LIB")"
# copy into a new folder, then swap it in (a running Host keeps its old files until it restarts)
rm -rf "$LIB.new" && mkdir -p "$LIB.new"
install -m 755 "$BIN" "$LIB.new/amistudio-hostd"
for f in ash-cli run-host.sh; do [ -f "$HERE/$f" ] && install -m 755 "$HERE/$f" "$LIB.new/$f"; done
for f in README-HOST.md CHANGES host.conf.example amistudio-host.service; do [ -f "$HERE/$f" ] && install -m 644 "$HERE/$f" "$LIB.new/$f"; done
[ -d "$HERE/engine" ] && cp -a "$HERE/engine" "$LIB.new/"
[ -d "$HERE/LICENSES" ] && cp -a "$HERE/LICENSES" "$LIB.new/"
[ -d "$HERE/services" ] && cp -a "$HERE/services" "$LIB.new/"   # 0.1.8: amidrive's drawer icon
[ -f "$HERE/host-defaults.conf" ] && install -m 600 "$HERE/host-defaults.conf" "$LIB.new/host-defaults.conf"   # the OAuth client
rm -rf "$LIB.old"; [ -e "$LIB" ] && mv "$LIB" "$LIB.old"; mv "$LIB.new" "$LIB"; rm -rf "$LIB.old"
# 0.1/0.1.1 installed the binary itself in ~/.local/bin: replace it with the link
rm -f "$HOME/.local/bin/amistudio-hostd"; ln -s "$LIB/amistudio-hostd" "$HOME/.local/bin/amistudio-hostd"
[ -n "$UNIT" ] && install -m 644 "$UNIT" "$HOME/.config/systemd/user/amistudio-host.service"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/amistudio-host"
mkdir -p "$CONF" && chmod 700 "$CONF"
if [ -n "$CONFEX" ] && [ ! -f "$CONF/host.conf" ]; then install -m 600 "$CONFEX" "$CONF/host.conf"; echo "wrote $CONF/host.conf"; fi
echo "installed $LIB ($("$HOME/.local/bin/amistudio-hostd" --version))"
[ -x "$LIB/engine/asb-chromed" ] && echo "browser engine: $LIB/engine (AmiSurf's 'browser' service, no setup needed)"
if [ "${1:-}" = "--enable" ]; then
  systemctl --user daemon-reload && systemctl --user enable amistudio-host && systemctl --user restart amistudio-host && systemctl --user --no-pager status amistudio-host | head -5
else
  echo "next: systemctl --user daemon-reload && systemctl --user enable --now amistudio-host"
  echo "      (after an update: systemctl --user restart amistudio-host)"
  echo "      (loginctl enable-linger $USER to start it at boot without logging in)"
fi
echo "then: amistudio-hostd pair     # 6-digit code to enter on the Amiga"
