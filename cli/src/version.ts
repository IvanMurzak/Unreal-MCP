// Plain constants, NOT a runtime `require('../package.json')`: that read throws inside a bundled
// standalone binary (e.g. `bun build --compile`) and crashed every importer at load time.
// `commands/bump-version.ps1` rewrites PACKAGE_VERSION with package.json; tests/version.test.ts
// fails on drift (load-bearing: PACKAGE_VERSION names the plugin-source release asset).
export const PACKAGE_NAME: string = 'unreal-mcp-cli';
export const PACKAGE_VERSION: string = '0.24.1';
