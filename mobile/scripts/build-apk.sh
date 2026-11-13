#!/usr/bin/env bash
# Release APK for the phone, and optionally install it. Usage:
#   ./scripts/build-apk.sh            # build
#   ./scripts/build-apk.sh install    # build, install over adb, start it
#
# A release build carries its own JS bundle, so the app runs in the car with
# no computer. Signed with the app's own key (plugins/with-release-signing.js)
# - ~/.config/n-gauge/app-signing.jks and .pass, as CI signs from its secrets
# - so this build and CI's install over each other. Without that keystore it
# falls back to the debug key, and says so: a phone with the other signature
# refuses it (INSTALL_FAILED_UPDATE_INCOMPATIBLE, 2026-11-13).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

KEYDIR="$HOME/.config/n-gauge"
if [ -z "${APP_KEYSTORE:-}" ] && [ -f "$KEYDIR/app-signing.jks" ] && [ -f "$KEYDIR/app-signing.pass" ]; then
  export APP_KEYSTORE="$KEYDIR/app-signing.jks"
  APP_KEYSTORE_PASSWORD="$(cat "$KEYDIR/app-signing.pass")"
  export APP_KEYSTORE_PASSWORD
fi
[ -n "${APP_KEYSTORE:-}" ] || echo "!!! no $KEYDIR/app-signing.jks - signed with the DEBUG key" >&2

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
# Gradle needs JDK 17+; this machine's default java is 8.
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"

[ -d node_modules ] || npm install
# android/ is generated (app.json, app.config.ts and the plugins are the
# source of truth for it), and regenerated on every build: the version comes
# from git (app.config.ts), and prebuild is what writes it into build.gradle.
CI=1 npx expo prebuild --platform android --no-install

# Build output to another disk (this machine: the NVMe at /data; the system
# disk is nearly full). Set BUILD_ROOT to choose, or BUILD_ROOT= to keep it
# all in place.
#
# Every Android module's build/ becomes a symlink there - the app's and each
# native package's, which Gradle writes into node_modules/<pkg>/android/
# (~7 GB, against a 0.7 GB node_modules). Symlinks rather than pointing
# Gradle's buildDir elsewhere: several packages' CMake files (reanimated,
# screens, safe-area-context) read ${CMAKE_SOURCE_DIR}/build by path, and
# broke when it moved. Not .cxx/: with CMake's own build dir a symlink, ninja
# fails with "build.ninja still dirty after 100 tries". npm install and
# prebuild replace the directories, so this runs before every build;
# whatever is there is build output and is regenerated.
if [ -z "${BUILD_ROOT+x}" ] && [ -d /data ]; then BUILD_ROOT=/data/cache/n-gauge-build; fi
if [ -n "${BUILD_ROOT:-}" ]; then
  mkdir -p "$BUILD_ROOT"
  modules=$( { echo android/app; find node_modules -path '*/android/build.gradle*' -not -path '*/build/*' -printf '%h\n'; } | sort -u)
  for m in $modules; do
    target="$BUILD_ROOT/$(echo "$m" | tr / _)"
    [ "$(readlink "$m/build" 2>/dev/null)" = "$target" ] && continue
    rm -rf "${m:?}/build"
    mkdir -p "$target"
    ln -s "$target" "$m/build"
  done
fi

# arm64 only: every current phone, and a quarter of the native build.
# 4 GB for Gradle, not the template's 2: dex merging ran out of heap twice on
# 2026-10-14 (D8 OutOfMemoryError), the second time after react-native-svg.
# Here, not in android/gradle.properties: prebuild rewrites that every build.
(cd android && ./gradlew :app:assembleRelease -PreactNativeArchitectures=arm64-v8a --console=plain \
  -Dorg.gradle.jvmargs="-Xmx4096m -XX:MaxMetaspaceSize=1024m")

APK=android/app/build/outputs/apk/release/app-release.apk
echo "==> $APK"
# Which key it carries - a debug-signed one will not update the app on a phone.
BT=$(ls -d "$ANDROID_HOME"/build-tools/* 2>/dev/null | tail -1)
[ -n "$BT" ] && "$BT/apksigner" verify --print-certs "$APK" 2>/dev/null | grep -m2 -E "DN|SHA-256" || true

if [ "${1:-}" = install ]; then
  adb install -r "$APK"
  adb shell am start -n dev.ngauge.pod/.MainActivity >/dev/null
fi
