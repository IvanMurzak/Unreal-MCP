import { fetchLatestNpmVersion } from '@baizor/gamedev-cli-core';

export { isRunningViaNpx } from '@baizor/gamedev-cli-core';

const PACKAGE_NAME = 'unreal-mcp-cli';

/** The latest published CLI. It ships as the last leg of a plugin release, so it always names a published plugin. */
export function fetchLatestVersion(): Promise<string> {
  return fetchLatestNpmVersion(PACKAGE_NAME);
}
