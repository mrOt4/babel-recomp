#!/usr/bin/env bash
# Windows x86-64 cross toolchain: llvm-mingw (UCRT), no root needed.
# Result: $TOOLCHAINS/llvm-mingw and the CMake toolchain file below.
source "$(dirname "$0")/common.sh"
if [ ! -x "$TOOLCHAINS/llvm-mingw/bin/x86_64-w64-mingw32-clang" ]; then
  url=$(gh_latest_asset mstorsjo/llvm-mingw 'ucrt-ubuntu-[0-9.]+-x86_64\.tar\.xz$')
  echo "llvm-mingw: $url"
  curl -fL "$url" -o "$TOOLCHAINS/llvm-mingw.tar.xz"
  rm -rf "$TOOLCHAINS/llvm-mingw" && mkdir -p "$TOOLCHAINS/llvm-mingw"
  tar -xf "$TOOLCHAINS/llvm-mingw.tar.xz" -C "$TOOLCHAINS/llvm-mingw" --strip-components=1
  rm "$TOOLCHAINS/llvm-mingw.tar.xz"
fi
"$TOOLCHAINS/llvm-mingw/bin/x86_64-w64-mingw32-clang" --version | head -1
