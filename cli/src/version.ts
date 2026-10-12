// The package identity as plain constants — deliberately NOT read from package.json at runtime.
// A module-top-level `require('../package.json')` throws when this package is bundled into a
// standalone binary (e.g. `bun build --compile`, which has no package.json on disk), crashing every
// importer at load time. `commands/bump-version.ps1` rewrites PACKAGE_VERSION together with
// package.json, and tests/version.test.ts fails CI (and so the release run) if the two ever drift —
// load-bearing, because PACKAGE_VERSION picks the plugin-source release asset to download.
export const PACKAGE_NAME = 'unreal-mcp-cli';
export const PACKAGE_VERSION = '0.20.0';
