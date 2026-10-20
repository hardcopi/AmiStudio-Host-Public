#!/bin/bash
# tools/fetch-rclone.sh : build/rclone/stage/ = rclone for the Host's amidrive service (Google Drive, 0.1.8)
#   engine/rclone/rclone            rclone v1.75.2 linux-amd64 (upstream static binary, unmodified)
#   LICENSES/rclone-COPYING.txt     rclone's licence (MIT), from the same tag
# mktarball.sh copies the stage into the tarball. Both files are pinned by sha256.
#   RCLONE_ZIP=F   use this rclone-v1.75.2-linux-amd64.zip instead of downloading (sha256 is still checked)
set -euo pipefail
cd "$(dirname "$0")/.."
# ---- pins (update together; see docs/SERVICES.md "amidrive") ----
RCVER=v1.75.2
RC_URL=https://downloads.rclone.org/$RCVER/rclone-$RCVER-linux-amd64.zip
RC_SHA=349ac8fba6ff65d6247043f1750cdcb518ec5d500ef91463a10d37c0ccdf3702
COPYING_URL=https://raw.githubusercontent.com/rclone/rclone/$RCVER/COPYING
COPYING_SHA=8cd2e9e750b90a04b7d82dbbca3930c696ae0309d7c10464f90a44f45754cd04
# ------------------------------------------------------------------
B=build/rclone; mkdir -p $B
Z=$B/rclone-$RCVER-linux-amd64.zip
if [ -n "${RCLONE_ZIP:-}" ]; then cp "$RCLONE_ZIP" $Z.part && mv $Z.part $Z
elif [ ! -f $Z ]; then curl -fL -o $Z.part "$RC_URL" && mv $Z.part $Z; fi
echo "$RC_SHA  $Z" | sha256sum -c --quiet || { echo "fetch-rclone: $Z has the wrong sha256"; rm -f $Z; exit 1; }
C=$B/rclone-COPYING-$RCVER.txt
[ -f $C ] || { curl -fsSL -o $C.part "$COPYING_URL" && mv $C.part $C; }
echo "$COPYING_SHA  $C" | sha256sum -c --quiet || { echo "fetch-rclone: $C has the wrong sha256"; rm -f $C; exit 1; }
rm -rf $B/x && mkdir -p $B/x && (cd $B/x && unzip -q "$OLDPWD/$Z")
R=$B/x/rclone-$RCVER-linux-amd64/rclone
[ -x $R ] || { echo "fetch-rclone: no rclone binary in $Z"; exit 1; }
"$R" version | head -1 | grep -q "rclone $RCVER" || { echo "fetch-rclone: $R is not rclone $RCVER"; exit 1; }
S=$B/stage; rm -rf $S && mkdir -p $S/engine/rclone $S/LICENSES
cp $R $S/engine/rclone/rclone && chmod 755 $S/engine/rclone/rclone
cp $C $S/LICENSES/rclone-COPYING.txt
cat > $S/LICENSES/RCLONE-VERSION.txt <<V
rclone bundled with the AmiStudio Host (amidrive service: Google Drive)
rclone $RCVER linux-amd64, upstream binary, unmodified
  $RC_URL
  sha256 $RC_SHA
Licence: MIT (rclone-COPYING.txt, from $COPYING_URL)
Source: https://github.com/rclone/rclone/tree/$RCVER
V
rm -rf $B/x
echo "fetch-rclone: staged rclone $RCVER in $S"
