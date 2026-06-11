# unreal-mcp-cli

Cross-platform CLI tool for [Unreal-MCP](https://github.com/IvanMurzak/Unreal-MCP) — the Unreal
analog of [`unity-mcp-cli`](https://github.com/IvanMurzak/Unity-MCP/tree/main/cli) and
[`godot-cli`](https://github.com/IvanMurzak/Godot-MCP/tree/main/cli). It resolves and launches the
Unreal Editor with the right `UNREAL_MCP_*` connection env vars, runs MCP/system tools over HTTP,
probes server health, configures AI agents (Claude Code, Cursor, VS Code, …), and manages the
`UnrealMCP` plugin.

## Install

Run without installing (npx caches the package between runs — use
`unreal-mcp-cli@latest` to force the newest published version):

```bash
npx unreal-mcp-cli status
# or, to always fetch the newest published version:
npx unreal-mcp-cli@latest status
```

Or install globally to get the `unreal-mcp-cli` command on your `PATH`:

```bash
npm i -g unreal-mcp-cli
unreal-mcp-cli status
```

Requires Node `^20.19.0 || >=22.12.0`. See the
[Unreal-MCP repository](https://github.com/IvanMurzak/Unreal-MCP) for the full project docs and the
[architecture reference](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/ARCHITECTURE.md).

## Commands (docs/ARCHITECTURE.md §9.1)

| Command | What it does |
| --- | --- |
| `create-project <path>` | Scaffold a minimal Unreal Engine C++ project |
| `open [path]` | Launch the Unreal Editor, wiring `UNREAL_MCP_*` env vars (engine resolved via `EngineAssociation` + `LauncherInstalled.dat`) |
| `close [path]` | Terminate the editor process running a project |
| `install-plugin [path]` | Copy (or `--junction`) the UnrealMCP plugin into `<project>/Plugins` |
| `remove-plugin [path]` | Remove the installed plugin |
| `configure` | Write `UNREAL_MCP_*` into `<project>/.env` and gitignore `.env` (§8) |
| `setup-mcp <agent>` | Write an MCP client config snippet (claude-code, cursor, vscode). With `--transport stdio` it also downloads the pinned shared [`gamedev-mcp-server`](https://github.com/IvanMurzak/GameDev-MCP-Server) release into `<project>/Intermediate/UnrealMCP/server/<rid>/` (skipped when `UNREAL_MCP_SERVER_PATH` points at a local build) |
| `login` | OAuth device-code auth against ai-game.dev |
| `status` | Report project + plugin + connection + live reachability |
| `wait-for-ready` | Block until the project's MCP server responds to a ping |
| `run-tool <tool>` | Invoke an MCP tool over the local HTTP path |
| `run-system-tool <tool>` | Invoke a system tool over the local HTTP path |
| `bootstrap-local [path]` | Build the bridge from source into `Intermediate/UnrealMCP/` (§6) — the MCP server is downloaded from GameDev-MCP-Server releases, not built here |
| `update [path]` | Re-sync the installed plugin from the repo source |
| `install-engine [version]` | Detect installed engines from `LauncherInstalled.dat`; link to the Epic launcher for missing versions (never installs directly) |
| `setup-skills` | Write a Claude-Code skill stub that drives the project's MCP server |

## Engine resolution

`open` reads the project's `.uproject` `EngineAssociation` and resolves it against the Epic launcher
manifest (`LauncherInstalled.dat`): a version association (`"5.7"`) maps to the matching install; an
empty association falls back to the highest installed engine; a GUID (source build) requires
`--engine-root`. `install-engine` enumerates the same manifest and, for a not-yet-installed version,
hands back a `com.epicgames.launcher://` deep link rather than performing the multi-GB download.

## Library export

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

`dist/lib.js` exposes the command logic as a side-effect-free library: no argv parsing, no stdout,
no process exit. Every function returns a `{ kind: 'success' | 'failure' }` discriminated union and
never throws past the boundary — the same contract as the Unity/Godot CLIs.

## Develop

```bash
npm install
npm run build   # tsc -> dist/ (ESM)
npm test        # vitest (unit tests; fixtures under tests/fixtures/)
node bin/unreal-mcp-cli.js status

# Optional end-to-end integration test (NOT part of the default run):
UNREAL_MCP_CLI_INTEGRATION=1 npm test
```
