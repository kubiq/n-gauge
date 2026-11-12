// The release APK signed with the app's own key (asked 2026-11-12), not the
// React Native template's debug key - which is public, the same in every
// project. android/ is generated (expo prebuild), so the signing goes in
// here, into its app/build.gradle.
//
// The key comes from the environment at build time, never from the repo:
//   APP_KEYSTORE           the keystore file (scripts/build-apk.sh: the one in
//                          ~/.config/n-gauge; CI: decoded from a secret)
//   APP_KEYSTORE_PASSWORD  its password, the key's too
//   APP_KEY_ALIAS          the key in it, "n-gauge" if unset
// Without APP_KEYSTORE the release is signed with the debug key as before -
// a build anyone can make, that does not update the owner's install.
const { withAppBuildGradle } = require('@expo/config-plugins');

const RELEASE_CONFIG = `
        release {
            if (System.getenv("APP_KEYSTORE")) {
                storeFile file(System.getenv("APP_KEYSTORE"))
                storePassword System.getenv("APP_KEYSTORE_PASSWORD")
                keyAlias System.getenv("APP_KEY_ALIAS") ?: "n-gauge"
                keyPassword System.getenv("APP_KEYSTORE_PASSWORD")
            }
        }`;

module.exports = function withReleaseSigning(config) {
  return withAppBuildGradle(config, (c) => {
    let g = c.modResults.contents;
    if (g.includes('System.getenv("APP_KEYSTORE")')) return c;   // applied already
    const configs = 'signingConfigs {';
    const release = /(release \{[\s\S]*?)signingConfig signingConfigs\.debug/;
    if (!g.includes(configs) || !release.test(g)) {
      throw new Error('with-release-signing: app/build.gradle is not as expected - update the plugin');
    }
    // The build type first: before the new block, the first "release {" is
    // the build type's (signingConfigs holds only debug).
    g = g.replace(
      release,
      '$1signingConfig System.getenv("APP_KEYSTORE") ? signingConfigs.release : signingConfigs.debug',
    );
    g = g.replace(configs, configs + RELEASE_CONFIG);
    c.modResults.contents = g;
    return c;
  });
};
