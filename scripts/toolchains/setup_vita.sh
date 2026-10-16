#!/usr/bin/env bash
# PlayStation Vita: VitaSDK (compiler, stubs, vita.cmake) in $TOOLCHAINS/vitasdk.
# No root needed. Afterwards: export VITASDK=$TOOLCHAINS/vitasdk
source "$(dirname "$0")/common.sh"
export VITASDK="$TOOLCHAINS/vitasdk"
if [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
  rm -rf "$TOOLCHAINS/vdpm"
  git clone --depth 1 https://github.com/vitasdk/vdpm "$TOOLCHAINS/vdpm"
  (cd "$TOOLCHAINS/vdpm" && ./bootstrap-vitasdk.sh)
fi
"$VITASDK/bin/arm-vita-eabi-gcc" --version | head -1
echo "export VITASDK=$VITASDK"
