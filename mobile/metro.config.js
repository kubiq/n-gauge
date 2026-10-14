// Expo's defaults, plus .tsv as an asset: the place lists (assets/places)
// ship as files read when a drive is first summarised, not as JS modules -
// a module stays in memory for the whole run once required.
const { getDefaultConfig } = require('expo/metro-config');

const config = getDefaultConfig(__dirname);
config.resolver.assetExts.push('tsv');

module.exports = config;
