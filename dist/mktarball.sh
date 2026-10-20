#!/bin/sh
# dist/mktarball.sh VER : dist/amistudio-host-VER-linux-x86_64.tar.gz
#   static amistudio-hostd + ash-cli + scripts + docs, and (0.1.2+) AmiSurf's browser engine from tools/fetch-engine.sh:
#   engine/ (engine.sh, asb-chromed, chromium/) and LICENSES/. NOENGINE=1 builds the small Host-only tarball.
#   0.1.8: engine/rclone/rclone + LICENSES/rclone-COPYING.txt from tools/fetch-rclone.sh (NORCLONE=1 leaves them out),
#   services/amidrive/Google Drive.info, and host-defaults.conf (0600) with the AmiStudio OAuth client: the client
#   secret comes from AMIDRIVE_CLIENT_SECRET at build time only (empty when unset; never echoed, never in the repo).
set -e
V=$1; P=amistudio-host-$V-linux-x86_64; B=build/$P
rm -rf $B && mkdir -p $B
cp build/static/amistudio-hostd build/ash-cli dist/run-host.sh dist/install-user.sh dist/host.conf.example dist/systemd/amistudio-host.service CHANGES $B/
cp docs/INSTALL.md $B/README-HOST.md
strip $B/amistudio-hostd $B/ash-cli
if [ "${NOENGINE:-0}" != 1 ]; then
  [ -x build/engine/stage/engine/asb-chromed ] && [ -x build/engine/stage/engine/chromium/chrome ] || { echo "no engine: run tools/fetch-engine.sh (or NOENGINE=1)"; exit 1; }
  cp -a build/engine/stage/engine build/engine/stage/LICENSES $B/
  if find $B -iname '*widevine*' | grep -q .; then echo 'Widevine files in the tarball - refusing'; exit 1; fi
fi
if [ "${NORCLONE:-0}" != 1 ]; then
  [ -x build/rclone/stage/engine/rclone/rclone ] || { echo "no rclone: run tools/fetch-rclone.sh (or NORCLONE=1)"; exit 1; }
  mkdir -p $B/engine $B/LICENSES && cp -a build/rclone/stage/engine/rclone $B/engine/ && cp build/rclone/stage/LICENSES/* $B/LICENSES/
fi
mkdir -p "$B/services/amidrive" && cp "dist/amidrive/Google Drive.info" "$B/services/amidrive/"
# the secret goes from the environment straight into the file (awk ENVIRON: not on any command line, not printed)
( umask 077; awk '/^amidrive_client_secret[ \t]/ { if (ENVIRON["AMIDRIVE_CLIENT_SECRET"] != "") { print "amidrive_client_secret \"" ENVIRON["AMIDRIVE_CLIENT_SECRET"] "\""; next } } { print }' \
    dist/host-defaults.conf > $B/host-defaults.conf )
chmod 600 $B/host-defaults.conf
if grep -q '^amidrive_client_secret ""' $B/host-defaults.conf; then echo "host-defaults.conf: no AMIDRIVE_CLIENT_SECRET set - Google sign-in will use rclone's built-in client"; else echo "host-defaults.conf: OAuth client secret filled in from AMIDRIVE_CLIENT_SECRET"; fi
mkdir -p $B/LICENSES && cp dist/licenses/*.txt $B/LICENSES/
{ echo "amistudio-hostd: AmiStudio Host $V, statically linked with OpenSSL libssl/libcrypto (Apache-2.0,"
  echo "  OpenSSL-Apache-2.0.txt), zlib (zlib.txt), zstd (zstd-BSD-3-Clause.txt) and glibc (LGPL-2.1-or-later;"
  echo "  source and build files: hardcopi/AmiStudio-Host, 'make build/static/amistudio-hostd')."
  echo "ash-cli: AmiStudio Host's command-line client."
  echo "engine/rclone/rclone: rclone (MIT, rclone-COPYING.txt; RCLONE-VERSION.txt), unmodified upstream binary."
  echo "The AmiStudio Host (amistudio-hostd, ash-cli, scripts) is free software under the GNU General Public"
  echo "License version 3 (GPL v3): GPL-3.0.txt. Source: hardcopi/AmiStudio-Host."; } > $B/LICENSES/AmiStudio-Host.txt
tar -C build -czf dist/$P.tar.gz --owner=0 --group=0 $P
ls -l dist/$P.tar.gz; sha256sum dist/$P.tar.gz
