#!/usr/bin/env bash
# Nintendo Switch (homebrew): devkitPro (devkitA64 + libnx + portlibs) and
# mesa-switch (https://github.com/NaGaa95/mesa-switch: EGL + OpenGL ES 3 on
# the Tegra X1). Step 1 needs root; run it yourself. Steps 2-3 are user-level.
source "$(dirname "$0")/common.sh"
step="${1:-help}"
case "$step" in
  devkitpro)   # --- 1. needs sudo -------------------------------------------
    cat <<'TXT'
Run these yourself (Arch/CachyOS; see https://devkitpro.org/wiki/devkitPro_pacman):

  sudo pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
  sudo pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0
  sudo pacman -U https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst
  grep -q '^\[dkp-libs\]' /etc/pacman.conf || printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n[dkp-linux]\nServer = https://pkg.devkitpro.org/packages/linux/$arch/\n' | sudo tee -a /etc/pacman.conf
  sudo pacman -Syu
  sudo pacman -S switch-dev switch-pkg-config switch-sdl2 switch-mesa switch-libdrm_nouveau switch-glad \
                  switch-zlib switch-libzstd switch-libexpat
  # switch-dev = devkitA64 + libnx + switch-tools + the CMake toolchain file.
  # switch-mesa is devkitPro's own OpenGL/GLES; mesa-switch (step mesa) is the
  # newer alternative (nvc0 / Zink+NVK).
  # then open a new shell (it sets DEVKITPRO=/opt/devkitpro)

Then: ./setup_switch.sh rust && ./setup_switch.sh mesa
TXT
    ;;
  rust)        # --- 2. Rust for mesa-switch (NVK); user-level ------------------
    if ! command -v rustup >/dev/null; then
      curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal
      source "$HOME/.cargo/env"
    fi
    rustup target add aarch64-unknown-linux-gnu
    ;;
  mesa)        # --- 3. build mesa-switch (OpenGL / GLES / EGL only) -----------
    : "${DEVKITPRO:?run step devkitpro first (DEVKITPRO unset)}"
    [ -d "$TOOLCHAINS/mesa-switch" ] || git clone --depth 1 https://github.com/NaGaa95/mesa-switch "$TOOLCHAINS/mesa-switch"
    cd "$TOOLCHAINS/mesa-switch"
    ./build-opengl.sh
    echo "Mesa for Switch staged in $TOOLCHAINS/mesa-switch (see its README for the SDK path)."
    ;;
  *) echo "usage: $0 devkitpro|rust|mesa" ;;
esac
