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

> **Plugin distribution.** For end users, the **recommended** way to install the UnrealMCP plugin
> itself is **Fab / Epic Marketplace** (precompiled per-engine binaries, auto-updated by the Epic
> Games Launcher — zero compile, zero stale-build risk). This CLI is the current/advanced/dev path
> until the Fab listing is live; its `install-plugin` / `update` commands use a local `UnrealMCP/`
> checkout when present, otherwise they download the dedicated `unreal-mcp-plugin-source-<version>.zip`
> GitHub Release asset that matches the CLI version. That source asset keeps the distributed
> descriptor semantics (**no `EngineVersion` pin**) and carries the signed bridge payload under
> `Source/ThirdParty/UnrealMcpBridge/<rid>/`, which the installer materializes into
> `Binaries/ThirdParty/...` for first-open convenience. `--plugin-source <dir>` remains the offline /
> CI / dev override.

## Commands (docs/ARCHITECTURE.md §9.1)

| Command | What it does |
| --- | --- |
| `create-project <path>` | Scaffold a minimal Unreal Engine C++ project |
| `open [path]` | Launch the Unreal Editor, wiring `UNREAL_MCP_*` env vars (engine resolved via `EngineAssociation` + `LauncherInstalled.dat`) |
| `close [path]` | Terminate the editor process running a project |
| `install-plugin [path]` | Copy (or `--junction`) the UnrealMCP plugin into `<project>/Plugins` — default source = local repo checkout when present, else the version-matched GitHub source asset |
| `remove-plugin [path]` | Remove the installed plugin |
| `install-extension <id> [path]` | Install a third-party Unreal-MCP **extension** plugin into `<project>/Plugins/<name>`, enable it + its gating engine plugins (e.g. `Niagara`) in the `.uproject`, and (re)compile (on next editor open, or now with `--build`). `--source <dir>` installs from a local copy (offline/CI); `--version <x.y.z>` overrides the catalog pin. Idempotent. See [Extensions](#extensions) |
| `configure` | Write `UNREAL_MCP_*` into `<project>/.env` and gitignore `.env` (§8) |
| `setup-mcp <agent>` | Write an MCP client config snippet (claude-code, cursor, vscode). With `--transport stdio` it also downloads the pinned shared [`gamedev-mcp-server`](https://github.com/IvanMurzak/GameDev-MCP-Server) release into `<project>/Intermediate/UnrealMCP/server/<rid>/` (skipped when `UNREAL_MCP_SERVER_PATH` points at a local build) |
| `login` | OAuth device-code auth against ai-game.dev |
| `status` | Report project + plugin + connection + live reachability |
| `wait-for-ready` | Block until the project's MCP server responds to a ping |
| `run-tool <tool>` | Invoke an MCP tool over the local HTTP path |
| `run-system-tool <tool>` | Invoke a system tool over the local HTTP path |
| `bootstrap-local [path]` | Build the bridge from source into `Intermediate/UnrealMCP/` (§6) — the MCP server is downloaded from GameDev-MCP-Server releases, not built here |
| `update [path]` | Re-sync the installed plugin from a local source or the version-matched GitHub source asset. On a version change it **auto-cleans** the stale UE C++ build cache (`Intermediate/` + C++ `Binaries/`) so the editor recompiles cleanly — the bundled sidecar bridge under `Binaries/ThirdParty/` is refreshed from `Source/ThirdParty/` when the release source asset ships it, otherwise preserved, junction (dev) installs are never cleaned, and `--no-clean` opts out |
| `install-engine [version]` | Detect installed engines from `LauncherInstalled.dat`; link to the Epic launcher for missing versions (never installs directly) |
| `setup-skills` | Write a Claude-Code skill stub that drives the project's MCP server |

## Engine resolution

`open` reads the project's `.uproject` `EngineAssociation` and resolves it against the Epic launcher
manifest (`LauncherInstalled.dat`): a version association (`"5.7"`) maps to the matching install; an
empty association falls back to the highest installed engine; a GUID (source build) requires
`--engine-root`. `install-engine` enumerates the same manifest and, for a not-yet-installed version,
hands back a `com.epicgames.launcher://` deep link rather than performing the multi-GB download.

## Connection mode

`open` (and the `openProject` library call) select the editor's MCP target via
`UNREAL_MCP_CONNECTION_MODE`: `Custom` points the plugin's sidecar at a local/self-hosted server
(`--host`), `Cloud` at ai-game.dev. Set it explicitly with `--connection-mode <Custom|Cloud>`. When
you omit it, supplying a `--host` **infers `Custom`** — a host implies a non-Cloud target, and the
plugin otherwise defaults to `Cloud` and ignores the host (silently dialing the cloud). An explicit
`--connection-mode` always wins over that inference; with neither a host nor a mode the variable is
left unset and the plugin default applies. Under `--no-connect` no `UNREAL_MCP_*` variables (the mode
included) are wired onto the editor at all.

## Library export

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

`dist/lib.js` exposes the command logic as a side-effect-free library: no argv parsing, no stdout,
no process exit. Every function returns a `{ kind: 'success' | 'failure' }` discriminated union and
never throws past the boundary — the same contract as the Unity/Godot CLIs.

## Extensions

An **extension** is a third-party UE C++ Editor (or Runtime) plugin that implements
`IUnrealMcpToolProvider` (see [`docs/EXTENSIONS.md`](../docs/EXTENSIONS.md)) and contributes MCP tools
to the core Unreal-MCP plugin. `install-extension` is the install channel for them:

```bash
# From the extension's GitHub release (the default channel):
unreal-mcp-cli install-extension com.acme.niagara ./MyGame --version 1.2.0

# From a local copy (offline / CI / dev — mirrors godot-cli install-plugin --source):
unreal-mcp-cli install-extension com.acme.niagara ./MyGame --source ../Unreal-AI-Niagara/MyNiagaraExt

# Compile immediately instead of on next editor open:
unreal-mcp-cli install-extension com.acme.niagara ./MyGame --source <dir> --build
```

It (1) resolves `<id>` against the **shared extension catalog**, (2) places the plugin into
`<project>/Plugins/<pluginName>/`, (3) enables the extension **and its gating engine plugins** (e.g.
`Niagara`, read from the extension's own `.uplugin` `Plugins[]` ∪ the catalog hint) in the `.uproject`,
and (4) compiles it. It is **idempotent** — a re-run that finds the same version already installed and
enabled writes nothing.

**Catalog / manifest format.** The installable extensions are listed in a shared, **engine-agnostic**
catalog (`src/utils/extensions-catalog.ts`, the typed mirror of the source-of-truth
`extensions.catalog.json` — `{ schemaVersion, extensions[] }`, the same shape as Godot's
`addons/godot_mcp/extensions.catalog.json`; a parity test keeps the two in lockstep). Each
`ExtensionDescriptor` carries `extensionId` (reverse-DNS resolve key), `name`, `pluginName` (the
`Plugins/<pluginName>` folder), `repo` (GitHub release source), `version`, **`minCoreVersion`** (the
minimum core Unreal-MCP version — surfaced as a warning when unmet), `enginePlugins` (gating plugins),
and `tools`. The catalog currently lists **Niagara Tools** (`com.ivanmurzak.unreal-ai-niagara`);
`--source <dir>` installs an unpublished or off-catalog extension regardless (the descriptor is then
synthesized from the source `.uplugin`).

**Pluggable install source (Fab-ready).** `resolveInstallSource` (`src/utils/extension-source.ts`) is
the single decision point for *where* the files come from — `--source` (local) or the GitHub release
(github.com-only, fail-closed). A future **Fab** (Epic marketplace) channel is added as one new `kind`
there plus a materializer branch, **without touching callers**.

**Compile-on-install strategy (chosen + documented).** Extensions ship as **source** and are compiled
by **UnrealBuildTool**. The default is **source-ship + compile-on-next-editor-open** (the install
reports `rebuildRequired: true`); `--build` opts into an **eager** UBT compile against the resolved
engine (**Windows-only** — it invokes `UnrealBuildTool.exe` for the `Win64` target; on macOS/Linux omit
`--build` and let the editor recompile on next open). Source-ship is chosen over shipping
precompiled-per-UE-version binaries because UE plugin
binaries are version/config/platform-specific and ABI-unstable across engine minors — exactly why the
core `install-plugin` also ships source and excludes the stale build cache.

**Engine-agnostic API.** `installExtension(opts)` and its `ExtensionDescriptor` / `InstallExtensionResult`
shapes are deliberately engine-neutral (no Unreal-only types in the signature) so `unity-mcp-cli` /
`godot-cli` can adopt the identical surface — the Unreal-specific bits (plugin placement, `.uproject`
enable, UBT) live behind it.

## Develop

```bash
npm install
npm run build   # tsc -> dist/ (ESM)
npm test        # vitest (unit tests; fixtures under tests/fixtures/)
node bin/unreal-mcp-cli.js status

# Optional end-to-end integration test (NOT part of the default run):
UNREAL_MCP_CLI_INTEGRATION=1 npm test
```
