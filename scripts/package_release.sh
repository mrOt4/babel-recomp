#!/usr/bin/env bash
# Package a ready-to-play zip from a build directory: the executable plus the
# data the build stages beside it (assets/, i18n/, mods/), the license and
# the README. No ROM, no save, no ROM-derived file can end up in it: the build
# is interpreter-only and the player brings their own ROM.
#
# Usage: scripts/package_release.sh <build-dir> <platform-tag>
#   e.g. scripts/package_release.sh build linux-x86_64
# Writes: dist/babel-recomp-<VERSION>-<platform-tag>.zip (.vpk for the Vita)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:?build directory}"
TAG="${2:?platform tag}"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
NAME="babel-recomp-$VERSION-$TAG"
STAGE="$ROOT/dist/$NAME"

case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
mkdir -p "$ROOT/dist"

# PlayStation Vita: the .vpk already carries the game's data.
if [ -f "$BUILD/babel-recomp.vpk" ]; then
  cp "$BUILD/babel-recomp.vpk" "$ROOT/dist/$NAME.vpk"
  echo "$ROOT/dist/$NAME.vpk"
  exit 0
fi

EXE=""
for f in babel-recomp.exe babel-recomp babel-recomp.nro babel-recomp.3dsx; do
  [ -f "$BUILD/$f" ] && EXE="$f" && break
done
[ -n "$EXE" ] || { echo "package_release.sh: no executable in $BUILD" >&2; exit 1; }

rm -rf "$STAGE" "$STAGE.zip"
# Consoles: the folder layout of the SD card.
case "$EXE" in
  *.nro) DEST="$STAGE/switch/babel-recomp" ;;
  *.3dsx) DEST="$STAGE/3ds/babel-recomp" ;;
  *) DEST="$STAGE" ;;
esac
mkdir -p "$DEST"
cp "$BUILD/$EXE" "$DEST/"
for d in assets i18n; do
  [ -d "$BUILD/$d" ] && cp -r "$BUILD/$d" "$DEST/"
done
mkdir -p "$DEST/mods"
cp -r "$BUILD/mods/preloaded" "$DEST/mods/"
cp "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/MODS.md" "$STAGE/"
# Nintendo 3DS: the installable .cia beside the SD-card folder.
[ "${EXE##*.}" = 3dsx ] && [ -f "$BUILD/babel-recomp.cia" ] && cp "$BUILD/babel-recomp.cia" "$STAGE/"

# Belt and braces: nothing that could come from a ROM or a player.
if find "$STAGE" \( -iname '*.sfc' -o -iname '*.smc' -o -iname '*.srm' -o -iname '*.sav' \
                   -o -iname '*.pcm' -o -iname '*.ips' \) | grep -q .; then
  echo "package_release.sh: ROM, save or patch data in the package; refusing" >&2
  exit 1
fi

(cd "$ROOT/dist" && cmake -E tar cf "$NAME.zip" --format=zip "$NAME")
rm -rf "$STAGE"
echo "$ROOT/dist/$NAME.zip"
