#!/usr/bin/env bash
# The app's release APK, and optionally onto the phone. Usage:
#   ./scripts/build-apk.sh            # build
#   ./scripts/build-apk.sh install    # build, then adb install
#
# arm64-v8a only: every phone this is for (a Samsung, 2026) is 64-bit ARM.
# The generated android/ project builds all four ABIs - 137 MB instead of
# ~56. android/ is not ours to edit (Expo regenerates it), so the choice is
# made here, at build time.
#
# Java 17 and the SDK are set here too, not taken from the shell: Gradle
# needs JVM 17+, and with the system default switched to Java 8 and no
# ANDROID_HOME the build failed outright (2026-10-22).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export JAVA_HOME="${APK_JAVA_HOME:-/usr/lib/jvm/java-17-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
ABIS="${APK_ABIS:-arm64-v8a}"

[ -d "$REPO/mobile/android" ] || (cd "$REPO/mobile" && npx expo prebuild --platform android)

echo "==> Building the APK ($ABIS, Java $JAVA_HOME)"
(cd "$REPO/mobile/android" && ./gradlew assembleRelease -q -PreactNativeArchitectures="$ABIS")
APK="$REPO/mobile/android/app/build/outputs/apk/release/app-release.apk"
echo "==> $APK ($(du -m "$APK" | cut -f1) MB)"

[ "${1:-}" = "install" ] || exit 0
adb install -r "$APK"
