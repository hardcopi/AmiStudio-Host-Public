#!/bin/bash
# tools/engine-work.sh : work on the Host's patches to AmiSurf's engine (asb-chromed) without touching AmiSurf's repo.
#   tools/engine-work.sh init     build/engine/work = git repo of AmiSurf at the pin (tools/fetch-engine.sh ASB_PIN)
#                                 with engine-patches/*.patch applied as commits (git am)
#   tools/engine-work.sh export   write the work repo's commits since the pin back to engine-patches/ (git format-patch)
#   tools/engine-work.sh build    make build/host/asb-chromed (dynamic, for tests) in the work repo
# The patches are plain `git format-patch` mails against AmiSurf's tree, so AmiSurf can take them with
#   git am /path/to/amistudio-host/engine-patches/*.patch
set -euo pipefail
cd "$(dirname "$0")/.."
PIN=$(sed -n 's/^ASB_PIN=\([0-9a-f]*\).*/\1/p' tools/fetch-engine.sh)
ASB_REPO=${ASB_REPO:-../amistudio-browser}
W=build/engine/work
G=(git -C $W -c user.name="${GIT_AUTHOR_NAME:-AmiStudio Host}" -c user.email="${GIT_AUTHOR_EMAIL:-host@amistudio.invalid}")
case "${1:-}" in
init)
  rm -rf $W && mkdir -p $W && git -C "$ASB_REPO" archive "$PIN" | tar -x -C $W
  git -C $W init -q && "${G[@]}" add -A && "${G[@]}" commit -qm "AmiSurf ${PIN:0:7} (pinned by the Host)" && "${G[@]}" tag base
  if ls engine-patches/*.patch >/dev/null 2>&1; then "${G[@]}" am -q --committer-date-is-author-date "$PWD"/engine-patches/*.patch; fi
  echo "engine-work: $W at ${PIN:0:7} + $(ls engine-patches/*.patch 2>/dev/null | wc -l) patch(es)" ;;
export)
  rm -f engine-patches/*.patch
  git -C $W format-patch -q --no-signature --zero-commit -N -o "$PWD/engine-patches" base..HEAD
  ls engine-patches ;;
build) make -s -C $W build/host/asb-chromed && echo "engine-work: $W/build/host/asb-chromed" ;;
*) echo "usage: $0 init|export|build"; exit 2 ;;
esac
