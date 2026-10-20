#!/bin/bash
# tools/fetch-engine.sh : build build/engine/stage/ = AmiSurf's browser engine for the Host release tarball
#   engine/asb-chromed        static, built from AmiSurf's source at the pinned commit below
#   engine/chromium/          portable ungoogled-chromium (headless), the same build AmiSurf 0.83's AmiKit edition ships
#   LICENSES/                 Chromium + ungoogled-chromium licences, Chromium credits, asb-chromed's third-party notices
# Nothing is written into AmiSurf's repo: the source comes out with `git archive`, read only, and the Host's own
# engine changes (engine-patches/*.patch, git format-patch mails; tools/engine-work.sh edits them) are applied on top.
#   ASB_REPO=DIR   AmiSurf checkout (default ../amistudio-browser; cloned from GitHub into build/engine if missing)
#   UC_TARBALL=F   use this ungoogled-chromium .tar.xz instead of downloading (sha256 is still checked)
set -euo pipefail
cd "$(dirname "$0")/.."
# ---- pins (update together; see docs/SERVICES.md "Bundled browser engine") ----
ASB_PIN=6493b84ae0280277b8d90b2d50494b7891f19b80       # AmiSurf 0.87 engine (main 6493b84: ASBC_BMARKS, bookmark folders, idle page sockets; later commits are shots only)
ASB_PIN_NAME="AmiSurf 0.87"
UCVER=155.0.8059.79-1
UC_URL=https://github.com/ungoogled-software/ungoogled-chromium-portablelinux/releases/download/$UCVER/ungoogled-chromium-$UCVER-x86_64_linux.tar.xz
UC_SHA=274a9253fa7ca2a3b13b3f9c7f18f9223761a21bc4c2b70dbec9a0ee558c235a   # = GitHub's asset digest
# ------------------------------------------------------------------------------
B=build/engine; mkdir -p $B
ASB_REPO=${ASB_REPO:-../amistudio-browser}
if ! git -C "$ASB_REPO" cat-file -e "$ASB_PIN^{commit}" 2>/dev/null; then
  ASB_REPO=$B/amistudio-browser.git
  [ -d "$ASB_REPO" ] || git clone -q --bare https://github.com/hardcopi/amistudio-browser.git "$ASB_REPO"
  git -C "$ASB_REPO" fetch -q origin "+refs/heads/*:refs/heads/*" || true
  git -C "$ASB_REPO" cat-file -e "$ASB_PIN^{commit}" || { echo "fetch-engine: commit $ASB_PIN not found"; exit 1; }
fi
PATCHES=$(ls engine-patches/*.patch 2>/dev/null || true)
PHASH=$( (cat $PATCHES /dev/null) | sha256sum | cut -c1-8)
SRC=$B/src-${ASB_PIN:0:7}-p$PHASH
if [ ! -x $SRC/build/static/asb-chromed ]; then
  rm -rf $SRC && mkdir -p $SRC && git -C "$ASB_REPO" archive "$ASB_PIN" | tar -x -C $SRC
  for p in $PATCHES; do patch -s -p1 -d $SRC --no-backup-if-mismatch < $p || { echo "fetch-engine: $p does not apply"; exit 1; }; done
  make -s -C $SRC build/static/asb-chromed
fi
{ ldd $SRC/build/static/asb-chromed 2>&1 || true; } | grep -q 'not a dynamic' || { echo "fetch-engine: asb-chromed is not static"; exit 1; }
# Chromium
UCX=$B/ungoogled-chromium-$UCVER-x86_64_linux.tar.xz
if [ -n "${UC_TARBALL:-}" ]; then cp "$UC_TARBALL" $UCX.part && mv $UCX.part $UCX
elif [ ! -f $UCX ] && [ -f "$ASB_REPO/build/uc/uc.tar.xz" ]; then cp "$ASB_REPO/build/uc/uc.tar.xz" $UCX.part && mv $UCX.part $UCX
elif [ ! -f $UCX ]; then curl -fL -o $UCX.part "$UC_URL" && mv $UCX.part $UCX; fi
echo "$UC_SHA  $UCX" | sha256sum -c --quiet || { echo "fetch-engine: $UCX has the wrong sha256"; rm -f $UCX; exit 1; }
UCD=$B/uc-$UCVER; if [ ! -x $UCD/chrome ]; then rm -rf $UCD $B/uc-tmp && mkdir -p $B/uc-tmp && tar -C $B/uc-tmp -xf $UCX && mv $B/uc-tmp/ungoogled-chromium-$UCVER-x86_64_linux $UCD && rm -rf $B/uc-tmp; fi
# stage (same content as AmiSurf's dist/amikit/mkbundle.sh puts in AmiSurf/Host: engine + chromium minus chromedriver)
S=$B/stage; rm -rf $S && mkdir -p $S/engine/chromium $S/LICENSES
cp $SRC/build/static/asb-chromed $S/engine/ && strip $S/engine/asb-chromed
cp dist/engine/engine.sh $S/engine/ && chmod +x $S/engine/engine.sh
cp -a $UCD/. $S/engine/chromium/ && rm -f $S/engine/chromium/chromedriver
L=$SRC/dist/amikit/licenses
cp $L/Chromium-LICENSE.txt $L/ungoogled-chromium-LICENSE.txt $L/portablelinux-LICENSE.txt $L/chromium-credits.html $S/LICENSES/
cp $L/Chromium-LICENSE.txt $S/engine/chromium/LICENSE
cp $SRC/dist/docs/ATTRIBUTIONS $S/LICENSES/AmiSurf-ATTRIBUTIONS.txt
( cd $SRC && bash tools/licenses.sh "$OLDPWD/$S/LICENSES" ) && cp $SRC/dist/licenses-host/*.txt $S/LICENSES/   # stb_image, QOI, OpenSSL, zlib, zstd
if find $S -iname '*widevine*' | grep -q .; then echo 'fetch-engine: Widevine files in the engine - refusing'; exit 1; fi
cat > $S/LICENSES/ENGINE-VERSION.txt <<V
AmiSurf browser engine bundled with the AmiStudio Host
asb-chromed: built from hardcopi/amistudio-browser $ASB_PIN ($ASB_PIN_NAME), static,
             plus the AmiStudio Host's engine patches ($(echo $PATCHES | wc -w): $(for p in $PATCHES; do basename $p .patch | cut -c1-4; done | tr '\n' ' ')sha $PHASH)
Chromium:    ungoogled-chromium portablelinux $UCVER x86_64 (unmodified binaries; chromedriver removed)
             $UC_URL
             sha256 $UC_SHA
Licences:    Chromium-LICENSE.txt (BSD-3-Clause), chromium-credits.html (third-party code in Chromium),
             ungoogled-chromium-LICENSE.txt, portablelinux-LICENSE.txt, AmiSurf-ATTRIBUTIONS.txt and the
             asb-chromed notices (stb_image, qoi-MIT, OpenSSL, zlib, zstd).
Engine patches (the Host's changes to asb-chromed, applied on top of $ASB_PIN; full text in engine-patches/):
$(for p in $PATCHES; do printf '             %s\n' "$(basename $p .patch)"; done)
Widevine (DRM) is not included.
V
if [ -n "$PATCHES" ]; then mkdir -p $S/LICENSES/engine-patches && cp $PATCHES $S/LICENSES/engine-patches/; fi
echo "fetch-engine: $S ready ($(du -sh $S | cut -f1))"
