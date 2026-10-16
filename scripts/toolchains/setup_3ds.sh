#!/usr/bin/env bash
# Nintendo 3DS: devkitPro's 3ds-dev (devkitARM, libctru, 3ds-cmake) plus the
# two .cia tools devkitPro does not ship: makerom (Project_CTR) and bannertool.
# The devkitPro step needs root; run it yourself. The tools go to
# $TOOLCHAINS/ctr-tools/bin, where CMakeLists.txt looks for them.
source "$(dirname "$0")/common.sh"
if [ ! -d /opt/devkitpro/devkitARM ]; then
  cat <<'TXT'
Install devkitPro's 3DS toolchain first (devkitPro pacman set up as in
setup_switch.sh devkitpro), then run this script again:

  sudo pacman -S --needed 3ds-dev
TXT
  exit 1
fi
D="$TOOLCHAINS/ctr-tools"
mkdir -p "$D/bin" "$D/dl"
if [ ! -x "$D/bin/makerom" ]; then
  curl -fL "$(gh_latest_asset 3DSGuy/Project_CTR 'makerom-.*ubuntu_x86_64\.zip$')" -o "$D/dl/makerom.zip"
  python3 -m zipfile -e "$D/dl/makerom.zip" "$D/dl/makerom"
  install -m755 "$D/dl/makerom/makerom" "$D/bin/makerom"
fi
if [ ! -x "$D/bin/bannertool" ]; then
  curl -fL "$(gh_latest_asset diasurgical/bannertool 'bannertool\.zip$')" -o "$D/dl/bannertool.zip"
  python3 -m zipfile -e "$D/dl/bannertool.zip" "$D/dl/bannertool"
  install -m755 "$D/dl/bannertool/linux-x86_64/bannertool" "$D/bin/bannertool"
fi
rm -rf "$D/dl"
"$D/bin/makerom" 2>&1 | head -1
"$D/bin/bannertool" 2>&1 | head -1
