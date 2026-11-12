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

# Signed with the app's own key (mobile/plugins/with-release-signing.js,
# 2026-11-12): the keystore and its password beside the firmware key's
# public half, outside the repo - CI has the same in secrets, so its APKs
# update this install too. Without them (another builder): the debug key.
KEYDIR="${APP_KEY_DIR:-$HOME/.config/n-gauge}"
if [ -z "${APP_KEYSTORE:-}" ] && [ -f "$KEYDIR/app-signing.jks" ]; then
  export APP_KEYSTORE="$KEYDIR/app-signing.jks"
  APP_KEYSTORE_PASSWORD="$(cat "$KEYDIR/app-signing.pass")"
  export APP_KEYSTORE_PASSWORD
fi
if [ -n "${APP_KEYSTORE:-}" ] && ! grep -q 'System.getenv("APP_KEYSTORE")' "$REPO/mobile/android/app/build.gradle"; then
  echo "ERROR: android/ is older than the signing plugin - run: cd mobile && npx expo prebuild --platform android --clean"
  exit 1
fi
[ -n "${APP_KEYSTORE:-}" ] || echo "==> No app signing key: signed with the debug key, as any build"

echo "==> Building the APK ($ABIS, Java $JAVA_HOME)"
(cd "$REPO/mobile/android" && ./gradlew assembleRelease -q -PreactNativeArchitectures="$ABIS")
APK="$REPO/mobile/android/app/build/outputs/apk/release/app-release.apk"
echo "==> $APK ($(du -m "$APK" | cut -f1) MB)"
# Who signed it - the one thing an update is accepted or refused by.
SIGNER="$(find "$ANDROID_HOME/build-tools" -name apksigner | sort -V | tail -1)"
[ -n "$SIGNER" ] && "$SIGNER" verify --print-certs "$APK" | grep -m1 "SHA-256 digest" | sed 's/^/==> /'

[ "${1:-}" = "install" ] || exit 0
adb install -r "$APK"
