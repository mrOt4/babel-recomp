#!/usr/bin/env bash
# Android: JDK 21 (Gradle), command-line tools, platform 35, build-tools, NDK.
# All under $TOOLCHAINS, no root needed. Accepts the SDK licences (read them:
# $TOOLCHAINS/android-sdk/licenses after the run).
source "$(dirname "$0")/common.sh"
NDK_VERSION="${NDK_VERSION:-27.3.13750724}"
if [ ! -x "$TOOLCHAINS/jdk21/bin/java" ]; then
  curl -fL "https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse" -o "$TOOLCHAINS/jdk21.tar.gz"
  rm -rf "$TOOLCHAINS/jdk21" && mkdir -p "$TOOLCHAINS/jdk21"
  tar -xf "$TOOLCHAINS/jdk21.tar.gz" -C "$TOOLCHAINS/jdk21" --strip-components=1
  rm "$TOOLCHAINS/jdk21.tar.gz"
fi
export JAVA_HOME="$TOOLCHAINS/jdk21"
SDK="$TOOLCHAINS/android-sdk"
if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
  url=$(curl -fsSL https://dl.google.com/android/repository/repository2-3.xml |
        grep -o 'commandlinetools-linux-[0-9]*_latest\.zip' | sort -V | tail -1)
  curl -fL "https://dl.google.com/android/repository/$url" -o "$TOOLCHAINS/cmdline-tools.zip"
  mkdir -p "$SDK/cmdline-tools" && rm -rf "$SDK/cmdline-tools/latest"
  unzip -q "$TOOLCHAINS/cmdline-tools.zip" -d "$SDK/cmdline-tools"
  mv "$SDK/cmdline-tools/cmdline-tools" "$SDK/cmdline-tools/latest"
  rm "$TOOLCHAINS/cmdline-tools.zip"
fi
(yes | "$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" --licenses >/dev/null) || true
"$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" \
  "platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;$NDK_VERSION" "cmake;3.31.6"
cat > "$TOOLCHAINS/android-env.sh" <<ENV
export JAVA_HOME="$TOOLCHAINS/jdk21"
export ANDROID_HOME="$SDK"
export ANDROID_NDK_HOME="$SDK/ndk/$NDK_VERSION"
export PATH="\$JAVA_HOME/bin:\$ANDROID_HOME/platform-tools:\$PATH"
ENV
echo "OK: source $TOOLCHAINS/android-env.sh"
