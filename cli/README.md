<div align="center" width="100%">
  <h1>Unreal MCP — <i>CLI</i></h1>

[![npm](https://img.shields.io/npm/v/unreal-mcp-cli?label=npm&labelColor=333A41 'npm package')](https://www.npmjs.com/package/unreal-mcp-cli)
[![Node.js](https://img.shields.io/badge/Node.js-%5E20.19.0%20%7C%7C%20%3E%3D22.12.0-5FA04E?logo=nodedotjs&labelColor=333A41 'Node.js')](https://nodejs.org/)
[![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=License&labelColor=333A41)](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)
[![Website](https://img.shields.io/badge/website-ai--game.dev-bc6c25?labelColor=333A41)](https://ai-game.dev)
[![Stand With Ukraine](https://raw.githubusercontent.com/vshymanskyy/StandWithUkraine/main/badges/StandWithUkraine.svg)](https://stand-with-ukraine.pp.ua)

  <img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/promo/ai-developer-banner-glitch.gif" alt="AI Game Developer" title="Unreal MCP CLI" width="100%">

  <p>
    <a href="https://claude.ai/download"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>
  </p>

</div>

Cross-platform CLI tool for **[Unreal-MCP](https://github.com/IvanMurzak/Unreal-MCP)** — the Unreal
analog of [`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) and
[`godot-cli`](https://www.npmjs.com/package/godot-cli). It resolves and launches the Unreal Editor with
the right `UNREAL_MCP_*` connection env vars, runs MCP/system tools over HTTP, probes server health,
configures AI agents (Claude Code, Cursor, VS Code, …), and manages the `UnrealMCP` plugin.

Backed by **[ai-game.dev](https://ai-game.dev)**. See the
[Unreal-MCP repository](https://github.com/IvanMurzak/Unreal-MCP) for the full project docs and the
[architecture reference](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/ARCHITECTURE.md).

## ![AI Game Developer — Unreal SKILLS and MCP](https://github.com/IvanMurzak/Unity-MCP/blob/main/docs/img/promo/hazzard-features.svg?raw=true)

- :white_check_mark: **Open & Connect** — launch the Unreal Editor with `UNREAL_MCP_*` connection env vars (engine auto-resolved)
- :white_check_mark: **Create projects** — scaffold a minimal Unreal Engine C++ project
- :white_check_mark: **Install plugin** — copy (or junction) the UnrealMCP plugin into a project's `Plugins/`
- :white_check_mark: **Install extensions** — add third-party Unreal-MCP AI tool-family plugins and their gating engine plugins
- :white_check_mark: **Configure** — write `UNREAL_MCP_*` connection settings into the project `.env`
- :white_check_mark: **Setup MCP** — write MCP client config for AI agents (optionally download the shared server for stdio)
- :white_check_mark: **Login** — OAuth device-code auth against `ai-game.dev`
- :white_check_mark: **Status & wait-for-ready** — report project/plugin/connection state; block until the MCP server answers
- :white_check_mark: **Run tools** — invoke MCP and system tools over the local HTTP path
- :white_check_mark: **Install engine** — detect installed engines and link to the Epic launcher for missing versions
- :white_check_mark: **Cross-platform** — Windows, macOS, and Linux
- :white_check_mark: **Library API** — a side-effect-free, typed library surface for embedding

![divider](https://github.com/IvanMurzak/Unity-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Quick Start

Run without installing (`npx` caches the package between runs — use `unreal-mcp-cli@latest` to force the
newest published version):

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

> **Requirements:** [Node.js](https://nodejs.org/) `^20.19.0 || >=22.12.0`.

> **Plugin distribution.** For end users, the **recommended** way to install the UnrealMCP plugin
> itself is **Fab / Epic Marketplace** (precompiled per-engine binaries, auto-updated by the Epic
> Games Launcher — zero compile, zero stale-build risk). This CLI is the current/advanced/dev path
> until the Fab listing is live; its `install-plugin` / `update` commands use a local `UnrealMCP/`
> checkout when present, otherwise they download the dedicated `unreal-mcp-plugin-source-<version>.zip`
> GitHub Release asset that matches the CLI version (override with `--version <x.y.z>`). The downloaded
> zip is **signature-verified** against a pinned publisher key (a detached minisign / Ed25519 signature,
> `…-source-<version>.zip.minisig`) **before** it is extracted — a tampered, unsigned, or wrong-key
> download is a hard failure and is never installed. That source asset keeps the distributed
> descriptor semantics (**no `EngineVersion` pin**) and carries the signed bridge payload under
> `Source/ThirdParty/UnrealMcpBridge/<rid>/`, which the installer materializes into
> `Binaries/ThirdParty/...` for first-open convenience. `--plugin-source <dir>` remains the offline /
> CI / dev override (a trusted local source, so it skips signature verification).

![divider](https://github.com/IvanMurzak/Unity-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Commands (docs/ARCHITECTURE.md §9.1)

| Command | What it does |
| --- | --- |
| `create-project <path>` | Scaffold a minimal Unreal Engine C++ project |
| `open [path]` | Launch the Unreal Editor, wiring `UNREAL_MCP_*` env vars (engine resolved via `EngineAssociation` + `LauncherInstalled.dat`). On desktop platforms it auto-runs a pre-launch build when the project/plugin still needs native editor binaries, then auto-dismisses the known startup blocker dialogs; pass `--no-build` or `--no-auto-dismiss-startup-dialogs` to skip either behavior |
| `close [path]` | Terminate the editor process running a project |
| `install-plugin [path]` | Copy (or `--junction`) the UnrealMCP plugin into `<project>/Plugins` — default source = local repo checkout when present, else the version-matched GitHub source asset |
| `remove-plugin [path]` | Remove the installed plugin |
| `install-extension <id> [path]` | Install a third-party Unreal-MCP **extension** plugin into `<project>/Plugins/<name>`, enable it + its gating engine plugins (e.g. `Niagara`) in the `.uproject`, and (re)compile (on next editor open, or now with `--build`). `--source <dir>` installs from a local copy (offline/CI); `--version <x.y.z>` overrides the catalog pin. Idempotent. See [Extensions](#extensions) |
| `configure` | Write `UNREAL_MCP_*` into `<project>/.env` and gitignore `.env` (§8) |
| `setup-mcp <agent>` | Write an MCP client config snippet (claude-code, cursor, vscode-copilot). Over the default `http` transport it points the agent at the **project-pinned** cloud URL `<base>/mcp/p/<pin>` (so the agent routes to *this* project's editor); pass `--no-pin` for the bare `<base>/mcp` URL. OAuth-capable clients get a credential-free, URL-only config and run their own device-code login. With `--transport stdio` it instead downloads the pinned shared [`gamedev-mcp-server`](https://github.com/IvanMurzak/GameDev-MCP-Server) release into `<project>/Intermediate/UnrealMCP/server/<rid>/` (skipped when `UNREAL_MCP_SERVER_PATH` points at a local build) |
| `login` | OAuth device-code auth against ai-game.dev. Signs in once per machine into the shared machine credential store (`~/.ai-game-dev/credentials.json`): the default mints the account (agent) credential and derives the tool credential from it. `--tools-only` mints a tool credential only (CI/automation runners — the desktop App cannot pick it up); `--yes` confirms switching the machine to a different account; `--path <dir>` writes a project-local `.env` instead |
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
by Unreal's desktop build tooling. The default is **source-ship + compile-on-next-editor-open** (the install
reports `rebuildRequired: true`); `--build` opts into an **eager** compile against the resolved
engine on **Windows / macOS / Linux**. Source-ship is chosen over shipping
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

![divider](https://github.com/IvanMurzak/Unity-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Supported AI Agents

`unreal-mcp-cli setup-mcp <agent>` writes a ready-to-use MCP client config so your favorite AI coding
agent can drive the Unreal Editor. Core support today: **Claude Code**, **Cursor**, and
**VS Code (Copilot)** — with the shared AI Game Developer ecosystem below.

<div align="center">
  <p>
    <a href="https://claude.ai/download"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/anthropics/claude-code"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/open-code-64.png" alt="Open Code" title="Open Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="https://github.com/IvanMurzak/Unity-MCP/raw/main/docs/img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>
  </p>
</div>

```bash
unreal-mcp-cli setup-mcp claude-code ./MyGame       # pinned <base>/mcp/p/<pin> URL (routes to this project)
unreal-mcp-cli setup-mcp claude-code ./MyGame --no-pin   # bare, unpinned <base>/mcp URL instead
unreal-mcp-cli setup-mcp cursor ./MyGame --transport stdio
```

> For the full Unreal-MCP project documentation, see the
> [main README](https://github.com/IvanMurzak/Unreal-MCP/blob/main/README.md) and the
> [architecture reference](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/ARCHITECTURE.md).
> Backed by **[ai-game.dev](https://ai-game.dev)**.

<div align="center">
  <sub>Made with :orange_heart: for game developers — <a href="https://ai-game.dev">ai-game.dev</a></sub>
</div>
