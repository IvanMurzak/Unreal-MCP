// Single source of the consumed GameDev-MCP-Server version (the shared,
// engine-agnostic MCP server released from
// https://github.com/IvanMurzak/GameDev-MCP-Server).
//
// This pin is INDEPENDENT of the Unreal-MCP plugin/bridge/cli version
// (plugin 0.x and shared server 8.x diverge) — `commands/bump-version.ps1`
// deliberately does NOT touch this file. Bumping the consumed server =
// changing this constant; the corresponding `v<SERVER_VERSION>` release
// (with all `gamedev-mcp-server-<rid>.zip` assets) MUST already exist on
// GameDev-MCP-Server BEFORE a CLI release that pins it (docs/RELEASING.md).

export const SERVER_VERSION = '8.0.1';
