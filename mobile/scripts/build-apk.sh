#!/usr/bin/env bash
# Release APK for the phone, and optionally install it. Usage:
#   ./scripts/build-apk.sh            # build
#   ./scripts/build-apk.sh install    # build, install over adb, start it
#
# A release build carries its own JS bundle, so the app runs in the car with
# no computer. Signed with the debug key, which is fine for one's own phone.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

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
(cd android && ./gradlew :app:assembleRelease -PreactNativeArchitectures=arm64-v8a --console=plain)

APK=android/app/build/outputs/apk/release/app-release.apk
echo "==> $APK"

if [ "${1:-}" = install ]; then
  adb install -r "$APK"
  adb shell am start -n dev.ngauge.pod/.MainActivity >/dev/null
fi
