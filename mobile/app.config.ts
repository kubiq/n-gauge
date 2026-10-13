// app.json plus the version, read from git when the app is built - nothing
// to bump by hand, the way the firmware's FW_BUILD is (scripts/build.sh):
//   versionCode  the repo's commit count: always rising, so Android takes
//                each build as an update of the last
//   version      0.1.<count>
//   extra.build  the commit, "+changes" when built from uncommitted work -
//                shown in Settings, so a build on a phone can be traced
import { execSync } from 'node:child_process';

import type { ConfigContext, ExpoConfig } from 'expo/config';

const git = (args: string) => {
  try {
    return execSync(`git ${args}`, { cwd: __dirname, stdio: ['ignore', 'pipe', 'ignore'] })
      .toString()
      .trim();
  } catch {
    return '';
  }
};

export default ({ config }: ConfigContext): ExpoConfig => {
  const count = Number(git('rev-list --count HEAD')) || 1;
  const commit = git('rev-parse --short HEAD') || 'unknown';
  const changed = git('status --porcelain --untracked-files=no -- .') !== '';
  return {
    ...config,
    name: config.name ?? 'N Gauge',
    slug: config.slug ?? 'n-gauge',
    version: `0.1.${count}`,
    android: { ...config.android, versionCode: count },
    extra: { ...config.extra, build: commit + (changed ? '+changes' : '') },
  };
};
