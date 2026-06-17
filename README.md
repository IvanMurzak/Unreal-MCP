<div align="center" width="100%">

## ![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/ai-developer-header.svg?raw=true)

[![MCP](https://badge.mcpx.dev 'MCP Server')](https://modelcontextprotocol.io/introduction)
[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.5%2B-0E1128?style=flat&logo=unrealengine&logoColor=white&labelColor=333A41 'Unreal Engine 5.5+, developed and CI-tested against 5.7')](https://www.unrealengine.com/)
[![release](https://github.com/IvanMurzak/Unreal-MCP/workflows/release/badge.svg 'release')](https://github.com/IvanMurzak/Unreal-MCP/actions/workflows/release.yml)</br>
[![Discord](https://img.shields.io/badge/Discord-Join-7289da?logo=discord&logoColor=white&labelColor=333A41 'Join')](https://discord.gg/cfbdMZX99G)
[![Stars](https://img.shields.io/github/stars/IvanMurzak/Unreal-MCP 'Stars')](https://github.com/IvanMurzak/Unreal-MCP/stargazers)
[![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=License&labelColor=333A41)](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)
[![Stand With Ukraine](https://raw.githubusercontent.com/vshymanskyy/StandWithUkraine/main/badges/StandWithUkraine.svg)](https://stand-with-ukraine.pp.ua)

  <img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/promo/ai-developer-banner.jpg" alt="AI Game Developer" title="AI Game Developer" width="100%">

<!-- TODO(demo): add an animated GIF of the AI Game Developer window driving the Unreal editor -->

  <p>
    <a href="https://claude.ai/download"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://visualstudio.microsoft.com/"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/visual-studio-64.png" alt="Visual Studio" title="Visual Studio" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/anthropics/claude-code"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/open-code-64.png" alt="Open Code" title="Open Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/Kilo-Org/kilocode"><img src="https://github.com/IvanMurzak/Unreal-MCP/raw/main/docs/img/mcp-clients/kilo-code-64.png" alt="Kilo Code" title="Kilo Code" height="36"></a>
  </p>

</div>

`Unreal MCP` is an AI-powered game development assistant **for the Unreal Editor**. Connect **Claude**, **Cursor**, **Copilot**, or any MCP-aware agent to Unreal Engine and let it inspect and drive your project — spawn actors, edit levels, author Blueprints, manage assets, edit and compile C++, capture screenshots, and more.

Unreal-MCP is the Unreal Engine counterpart of [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) and [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP): a **C++ editor plugin** that exposes Unreal Editor operations as **AI Tools** and connects them to an MCP server through the same hosted cloud backend ([ai-game.dev](https://ai-game.dev)) that powers Unity-MCP and Godot-MCP — or your own self-hosted server. The local server is the shared, engine-agnostic [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) (binary `gamedev-mcp-server`) — one server consumed by Unity-MCP, Godot-MCP, and Unreal-MCP; no server source lives in this repo.

Unlike Unity and Godot (C# engines that host the .NET `McpPlugin` in-process), Unreal's editor is **C++** — so the .NET MCP host runs as an auto-managed **sidecar process** (`unreal-mcp-bridge`) that the plugin spawns and talks to over a localhost IPC channel. The full design lives in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (see the §0 system-overview diagram).

> **Status: beta.** The plugin, the .NET sidecar, the `unreal-mcp-cli`, the AI Game Developer editor UI, and **62 built-in tools across 8 families** have shipped and are exercised by CI. The `unreal-mcp-cli` is **published on npm** — install the plugin with it (Option B below); the Fab / Epic Marketplace listing for the precompiled plugin is coming soon (Option A). Pixel-capture (screenshot) tools need a GPU-backed editor; everything else runs headless.

> **[💬 Join our Discord Server](https://discord.gg/cfbdMZX99G)** — Ask questions, showcase your work, and connect with other developers!

## ![Features](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-features.svg?raw=true)

- ✔️ **AI agents** — Use the best agents from **Anthropic**, **OpenAI**, **Google**, or any other provider with no vendor lock-in
- ✔️ **62 built-in Tools** — A wide range of [MCP Tools](#tools) across **8 families** for operating the Unreal Editor
- ✔️ **Blueprint authoring** — Create, edit, and **compile** Blueprints with a structured error/warning feedback loop the AI can act on
- ✔️ **C++ edit & compile** — Read, scaffold, and edit project C++, then compile (Live Coding or UBT) with a structured error report
- ✔️ **Visual feedback** — Capture viewport, game-view, camera, and isolated-actor screenshots the LLM can inspect directly
- ✔️ **Custom / extension AI Tools** — Register **your own** AI Tools from any UE plugin with **no fork** — a public, modular-feature contract ([Customize Tools](#customize-tools))
- ✔️ **Cloud or self-hosted** — Connect to `ai-game.dev` out of the box, or point at your own [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server)
- ✔️ **Per-tool enable / disable** — Flip any tool on or off from the **MCP Tools** window; the toggle is enforced at the execution boundary, not just hidden

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Table of contents

- [Requirements](#requirements)
- [Install](#install)
- [Updating the plugin](#updating-the-plugin)
- [First run](#first-run)
- [Tools](#tools) — all 8 families, 62 tools
- [Per-tool enable / disable](#per-tool-enable--disable)
- [`unreal-mcp-cli`](#unreal-mcp-cli)
- [Customize Tools](#customize-tools)
- [Configuration & environment variables](#configuration--environment-variables)
- [Troubleshooting](#troubleshooting)
- [How Unreal MCP Architecture Works](#how-unreal-mcp-architecture-works)
- [Repo layout](#repo-layout)
- [Links](#links)
- [License](#license)

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Requirements

- **Unreal Engine 5.5+** (developed and CI-tested against **5.7**). The plugin deliberately ships **no `EngineVersion` pin** — UE treats that field as an exact-build match, not a floor, and would refuse to load on newer engines.
- **.NET 9 SDK** to build the bridge sidecar **from source**. (End users of a packaged release do NOT need it — the self-contained sidecar is bundled inside the plugin and auto-spawned, ARCHITECTURE §6. The local MCP server is downloaded as a prebuilt [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) release binary, never built here.)
- **Node.js** `^20.19.0 || >=22.12.0` for the optional `unreal-mcp-cli`.
- A **C++ Unreal project** (the plugin builds an Editor module, so the host project must be able to compile C++).

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Install

There are three ways to install the plugin, in order of preference. **Fab / Epic Marketplace is the recommended channel for end users** — it ships precompiled, per-engine binaries and the Epic Games Launcher keeps them up to date automatically, so there is **zero compile and zero stale-build risk**. Source / `unreal-mcp-cli` installs remain the current path for developers and early adopters until the Fab listing goes live.

## Option A — Fab / Epic Marketplace (recommended; coming soon)

> **Status: not yet live.** The Fab listing is operator-gated and tracked separately; until it publishes, use Option B or C below. The framing here is what the end-user flow will be.

1. Install **Unreal-MCP** from [Fab](https://www.fab.com/) (the Epic Marketplace successor) into your engine via the Epic Games Launcher.
2. Enable the plugin for your project from **Edit → Plugins**.
3. Open the project — UE loads the **precompiled** plugin (no C++ build on your machine). On boot the Output Log prints **`[Unreal-MCP] plugin loaded`**.

Because Fab ships precompiled binaries and the Epic Launcher updates them in place, you never compile the plugin and never have to clear a stale build cache — the most robust path for non-developers.

## Option B — `unreal-mcp-cli` (current / advanced)

The CLI is the recommended path **today**, until the Fab listing is live. Install it from npm — **no repo clone, no build step**. It copies (or, for dev, junctions) the plugin into your project and, on **update**, automatically clears the stale UE build cache so you always get a clean recompile of the new code (see [Updating the plugin](#updating-the-plugin)).

```bash
# 1. Install unreal-mcp-cli (or use `npx unreal-mcp-cli@latest <command>` for a one-off, no install)
npm install -g unreal-mcp-cli

# 2. Install the UnrealMCP plugin into your project
unreal-mcp-cli install-plugin ./YourProject

# 3. Authorize against the cloud server (ai-game.dev)
unreal-mcp-cli login ./YourProject

# 4. Open the Unreal Editor for the project (wires the MCP connection env vars)
unreal-mcp-cli open ./YourProject
```

See [`cli/README.md`](cli/README.md) for the full 16-command reference.

## Option C — manual

1. Copy [`UnrealMCP/`](UnrealMCP/) into `<YourProject>/Plugins/UnrealMCP/` (or create a directory junction / symlink to it for live development).
2. Open the project; UE compiles the `UnrealMcpEditor` module on first launch.
3. On editor boot the Output Log prints **`[Unreal-MCP] plugin loaded`** — that confirms the plugin and its game-thread dispatcher started.

The sidecar binary (`unreal-mcp-bridge`) is **bundled inside the plugin** in a packaged release: a prebuilt, self-contained binary for your platform ships under `UnrealMCP/Binaries/ThirdParty/UnrealMcpBridge/<rid>/` and the editor **auto-spawns it on startup with zero user action** — no .NET install, no env var, no manual launch (ARCHITECTURE §6). The first Cloud OAuth device-code browser approval is the only remaining human step; after that, reconnect on later launches is zero-click (the cloud token is cached in `Saved/Config/UnrealMcp/`).

When you build the plugin **from source** (the only option until the first GitHub Release / Fab listing), the bundled binary is not present — the plugin then resolves the sidecar from the `UNREAL_MCP_BRIDGE_PATH` environment variable instead: point that at a locally built sidecar, or run `unreal-mcp-cli bootstrap-local` to build the bridge from source into `<YourProject>/Intermediate/UnrealMCP/` and set the var to the result. With neither a bundled binary nor the env var resolved, the plugin's TCP listener still starts but logs `[Unreal-MCP] no sidecar binary resolved for rid <rid> …` and spawns nothing.

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Updating the plugin

Updating in place must always leave you running the **new** code. The risk is UE's incremental compiler: if the plugin source changes (new `.cpp` files, a new module) but the old `UnrealMCP/Intermediate/` build cache survives, UE can do a partial recompile against a stale module file-list and silently leave you on old/partial code. Each channel handles this differently:

- **Fab / Epic Marketplace → automatic.** The Epic Games Launcher replaces the precompiled binaries in place; nothing to compile, no cache to clear. This is why Fab is the recommended channel.
- **`unreal-mcp-cli update` → automatic clean rebuild.** `update` re-copies the plugin source and, by **default**, deletes the installed plugin's stale `Intermediate/` and the C++ `Binaries/` so UE performs a clean compile on the next editor launch — no manual steps. The bundled sidecar bridge under `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` is **always preserved** (only the C++ module outputs are cleared). Dev **junction** installs are never cleaned (that would wipe your live source tree's outputs). Pass `--no-clean` to opt out of the cache wipe.

  ```bash
  node bin/unreal-mcp-cli.js update <YourProject>            # default: clean rebuild on version change
  node bin/unreal-mcp-cli.js update <YourProject> --force    # re-copy even when versions match
  node bin/unreal-mcp-cli.js update <YourProject> --no-clean # keep the existing build cache
  ```

- **Manual copy → clear the cache yourself.** If you overwrite `<YourProject>/Plugins/UnrealMCP/` by hand, **close the editor first**, delete `<YourProject>/Plugins/UnrealMCP/Intermediate/` and the C++ `Binaries/` (keep `Binaries/ThirdParty/` if a bundled bridge is present), then relaunch so UE recompiles cleanly.

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# First run

1. Open the **AI Game Developer** main window from the editor's **Tools** menu (the tab is registered under the Tools menu category).
2. **Choose a connection mode:**
   - **Cloud** (default) — connects to [ai-game.dev](https://ai-game.dev). Click **Authorize** to start the OAuth **device-code flow**: the window shows a verification URL and a short user code; open the URL, enter the code, approve, and the editor finishes authorizing. Use **Revoke** to clear the stored cloud token.
   - **Custom** — connects to a local `gamedev-mcp-server` you run (or any compatible server). Enter the server URL and point your AI client at it. (The plugin does not start the local server for you — run `unreal-mcp-cli` or your own process; see [Troubleshooting](#troubleshooting).)
3. The **Connection** section shows a status dot, a status label, and a Connect / Disconnect / Stop button; the bridge status reads `Running (restarts: N)` or `Stopped`. Use them to confirm the sidecar is live.
4. Point your AI client (Claude Code, Cursor, the AI Game Developer app, …) at the server. The **AI agents** section lists the agents currently connected; to write an MCP client config use `unreal-mcp-cli setup-mcp`.

Connection settings persist to `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` (`Saved/` is gitignored by every UE template, so tokens never land in VCS by default).

> That's it. Ask your AI *"Spawn three cubes in a row and a point light above them"* and watch it happen. ✨

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Tools

Unreal-MCP ships **62 built-in ("core") tools** across **8 families**. Tool ids are kebab-case (`actor-create`, `blueprint-compile`), matching the Unity/Godot naming convention. Extensions can add more (see [Customize Tools](#customize-tools)).

> This list is generated from the registration source (`UnrealMCP/Source/UnrealMcpEditor/Private/Tools/UnrealMcp*Tools.cpp`). Counts: actor 13, blueprint 11, asset 11, editor/reflection 9, level 7, source 6, screenshot 4, ping 1 = **62**.

<details>
  <summary><b>Actor &amp; component family (13)</b></summary>

| Tool id | What it does |
| --- | --- |
| `actor-create` | Spawn an actor from a class path (native or Blueprint), with optional name/location/rotation/parent |
| `actor-destroy` | Destroy an actor |
| `actor-duplicate` | Duplicate an actor |
| `actor-find` | Find actors, with scoped reads (`paths`/`viewQuery`) |
| `actor-modify` | Write actor `FProperty` values (including transform) |
| `actor-set-parent` | Attach an actor to a parent |
| `actor-component-add` | Add a component to an actor |
| `actor-component-destroy` | Destroy a component |
| `actor-component-get` | Read a component's data |
| `actor-component-modify` | Modify a component's properties |
| `actor-component-list-all` | List available `UActorComponent` classes (paginated) |
| `object-get-data` | Read any `UObject` by path |
| `object-modify` | Modify any `UObject` by path |

</details>

<details>
  <summary><b>Blueprint family (11) — Unreal's flagship surface</b></summary>

| Tool id | What it does |
| --- | --- |
| `blueprint-create` | Create a new Blueprint class from a parent `UClass` path |
| `blueprint-get` | Graph summary for LLM inspection (variables, components, functions/events, parent chain) |
| `blueprint-add-component` | Add a component via the Simple Construction Script |
| `blueprint-remove-component` | Remove an SCS component |
| `blueprint-add-variable` | Add a typed member variable |
| `blueprint-modify-variable` | Modify a member variable |
| `blueprint-set-default` | Edit a CDO (class-default) property |
| `blueprint-add-function` | Add a function stub (entry/result nodes wired) |
| `blueprint-add-event` | Add/bind an event stub (BeginPlay, Tick, input, …) |
| `blueprint-compile` | Compile the Blueprint and return a **structured error/warning list** (the AI feedback loop) |
| `blueprint-spawn` | Instance the Blueprint into the current level |

</details>

<details>
  <summary><b>Asset / Content-Browser family (11)</b></summary>

| Tool id | What it does |
| --- | --- |
| `asset-find` | Search the AssetRegistry by name/class/path/tags |
| `asset-get-data` | Read an asset's data (scoped reads supported) |
| `asset-create-folder` | Create a Content folder |
| `asset-copy` | Copy an asset |
| `asset-move` | Move / rename an asset |
| `asset-delete` | Delete an asset |
| `asset-refresh` | Rescan asset paths |
| `asset-material-create` | Create a Material Instance from a parent material |
| `asset-material-modify` | Set scalar/vector/texture material-instance parameters |
| `asset-material-get-data` | Read material graph/parameter info (the "shader" analog) |
| `asset-import` | Import FBX/textures via `AssetImportTask` |

</details>

<details>
  <summary><b>Editor / console / reflection family (9)</b></summary>

| Tool id | What it does |
| --- | --- |
| `editor-application-get-state` | Read editor application state (PIE, etc.) |
| `editor-application-set-state` | Start / stop / pause Play-In-Editor |
| `editor-selection-get` | Read the current editor selection |
| `editor-selection-set` | Set the editor selection |
| `console-get-logs` | Read recent editor logs from the `LogCollector` ring buffer |
| `console-clear-logs` | Clear the captured-log ring buffer |
| `console-run-command` | Run a console command / CVar |
| `reflection-method-find` | Discover callable `UFunction`s (returns invocation schemas) |
| `reflection-method-call` | Invoke a `UFunction` (static or instance, incl. `CallInEditor`) |

</details>

<details>
  <summary><b>Level / map family (7)</b></summary>

| Tool id | What it does |
| --- | --- |
| `level-create` | Create a new level |
| `level-open` | Open a level |
| `level-save` | Save the level (save-as via optional path) |
| `level-get-data` | Actor-tree snapshot of a level (scoped reads) |
| `level-list-loaded` | List persistent + streaming sublevels (World-Partition aware, read-only) |
| `level-set-current` | Set the current/active level |
| `level-unload-sublevel` | Unload a streaming sublevel |

</details>

<details>
  <summary><b>Source / C++ family (6)</b></summary>

| Tool id | What it does |
| --- | --- |
| `source-read` | Read a project C++ source file (sliced) |
| `source-create-class` | Scaffold a new C++ class (header + cpp from templates) |
| `source-update` | Edit a source file |
| `source-delete` | Delete a source file |
| `source-list` | List module source files |
| `source-compile` | Compile project C++ (Live Coding when active, else UBT) with a structured error report |

All file operations are jailed to `<Project>/Source/`.

</details>

<details>
  <summary><b>Screenshot / viewport-capture family (4)</b></summary>

| Tool id | What it does |
| --- | --- |
| `screenshot-viewport` | Capture the active editor viewport |
| `screenshot-game-view` | Capture the PIE / game view |
| `screenshot-camera` | Render from a resolved camera actor via `USceneCaptureComponent2D` |
| `screenshot-isolated` | Render an actor in isolation (transient SceneCapture2D + show-only list) |

Captures return a base64 **PNG as MCP image content** so the LLM can inspect the render directly. Dimensions are clamped (default 1024, hard cap 2048 per side). Pixel capture needs a GPU-backed editor; under headless `-nullrhi` these tools return a structured error.

</details>

<details>
  <summary><b>Ping family (1)</b></summary>

| Tool id | What it does |
| --- | --- |
| `ping` | Liveness probe — round-trips the plugin ⇄ sidecar ⇄ server chain |

</details>

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Per-tool enable / disable

Every tool can be individually enabled or disabled from the **MCP Tools** window — the standalone **MCP Tools** tab (registered under the editor's **Tools** menu). The window shows each tool's title, family, and description, plus an "N / M tools enabled" summary line. Disabling a tool:

- **removes it from the served manifest entirely** — it never appears in the MCP `tools/list`; and
- is **enforced at the execution boundary too** — even if a stale `tools/list` is dispatched, a disabled tool is rejected at `Execute()` rather than run.

Two filters combine to decide whether a tool is served (see ARCHITECTURE §7/§8):

- a **whitelist** (`enabledTools`, overridable via `UNREAL_MCP_TOOLS`) — when non-empty, only listed tools are served; empty means "no filter"; **and**
- a **blocklist** (`disabledTools`) — the per-tool toggles you flip in the UI.

A tool is served **iff** it passes the whitelist **and** is not in the blocklist. Both sets are persisted across editor sessions and survive an extension hot-reload (a re-registered tool inherits the retained toggle, so a rebuild can never silently re-enable a tool you disabled).

All connection settings live in the single **AI Game Developer** main window's Connection section (there is no separate Settings tab or Project-Settings page — Unity-MCP parity). The **MCP Prompts** and **MCP Resources** windows are wired but ship empty in this release — each renders a subdued empty-state message (the "N / M enabled" summary is unique to the Tools window).

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# `unreal-mcp-cli`

A cross-platform Node CLI (`unreal-mcp-cli`) that scaffolds projects, installs the plugin, configures connection settings, drives the local server, and invokes tools over HTTP. It is a port of `unity-mcp-cli` / `godot-cli`. Full reference: [`cli/README.md`](cli/README.md).

> Published on npm — install with `npm install -g unreal-mcp-cli`, or run a one-off with `npx unreal-mcp-cli@latest <command>`.

The full 16-command surface:

| Command | What it does |
| --- | --- |
| `create-project` | Scaffold a minimal Unreal Engine C++ project |
| `open` | Launch the Unreal Editor for a project, wiring MCP connection env vars |
| `close` | Terminate the Unreal Editor process running a project |
| `install-plugin` | Install the UnrealMCP plugin into `<project>/Plugins` (copy or `--junction`) |
| `remove-plugin` | Remove the UnrealMCP plugin from `<project>/Plugins` |
| `configure` | Write `UNREAL_MCP_*` values into `<project>/.env` and gitignore `.env` |
| `setup-mcp` | Write an MCP client config snippet for an agent |
| `login` | Authorize against ai-game.dev via the OAuth device-code flow |
| `status` | Report package, project, plugin, and live connection status |
| `wait-for-ready` | Block until the project's MCP server responds to a ping |
| `run-tool` | Invoke an MCP tool via the project's local MCP server (HTTP) |
| `run-system-tool` | Invoke a system tool via the project's local MCP server (HTTP) |
| `bootstrap-local` | Build the bridge from source into `<project>/Intermediate/UnrealMCP` (the server is downloaded by `setup-mcp`, not built) |
| `update` | Update the UnrealMCP plugin installed in a project from the repo source |
| `install-engine` | Detect installed Unreal engines; for a missing version, link to the Epic launcher |
| `setup-skills` | Write a Claude-Code skill stub that drives this project's Unreal MCP server |

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Customize Tools

**This is the headline extensibility feature.** Anyone can register their **own** AI Tools — from any third-party UE plugin — and have them appear in the MCP manifest alongside the 62 built-in tools. **No fork, no link-time coupling, no load-order assumptions.** Your tools are discovered automatically on editor boot (and on late-load / hot-unload), merged in deterministic order, and exposed to every connected AI agent.

You contribute tools through a small, public, **modular-feature-based contract**: implement [`IUnrealMcpToolProvider`](UnrealMCP/Source/UnrealMcpEditor/Public/IUnrealMcpToolProvider.h) and declare your tools with the fluent [`FUnrealMcpToolRegistry`](UnrealMCP/Source/UnrealMcpEditor/Public/UnrealMcpToolRegistry.h) builder:

```cpp
#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

class FMyExtensionProvider : public IUnrealMcpToolProvider
{
public:
    virtual FString GetExtensionId() const override      { return TEXT("com.foo.my-extension"); }
    virtual FText   GetDisplayName() const override      { return NSLOCTEXT("Foo", "Name", "My Extension"); }
    virtual FString GetExtensionVersion() const override { return TEXT("1.0.0"); }

    virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
    {
        Registry.Tool(TEXT("hello-extension"))
            .Title(TEXT("Hello Extension"))
            .Description(TEXT("Returns a friendly greeting."))
            .ParamString(TEXT("name"), TEXT("Who to greet. Defaults to 'world'."))
            .ReadOnlyHint(true)
            .IdempotentHint(true)
            .Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
            {
                const FString Name = Call.Has(TEXT("name")) ? Call.GetString(TEXT("name")) : TEXT("world");
                return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Hello, %s!"), *Name));
            });
    }
};
```

Then register the provider as a **modular feature** in your module's `StartupModule` (and unregister in `ShutdownModule`):

```cpp
// In StartupModule:
IModularFeatures::Get().RegisterModularFeature(
    IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
```

**How it behaves:**

- **Auto-discovery & hot-reload.** Unreal-MCP enumerates every registered `UnrealMcpToolProvider` on boot, and subscribes to register/unregister events — so loading or unloading your plugin at any time triggers a **registry rebuild** and a **manifest revision bump**, and the sidecar diffs the new manifest and adds/removes the affected MCP tools automatically. You never push anything yourself.
- **Deterministic merge.** Providers are merged in ascending `GetExtensionId()` order; within one provider, tools register in declaration order. Your `GetExtensionId()` is stamped onto every tool you contribute (don't call `.ExtensionId(...)` yourself).
- **Per-extension isolation.** Each tool descriptor is validated (kebab-case name, well-formed schema, bound handler). An **invalid** or **duplicate** tool is dropped/rejected and the reason is recorded on **your** extension's record — your other valid tools, and every other extension, are unaffected. (UE builds without C++ exceptions, so isolation is descriptor-level, not tool-body-level; validate inputs and fail gracefully with `FUnrealMcpToolResult::Error(...)`.)
- **Toggles & enable/disable.** Extension-contributed tools are toggled in the same **MCP Tools** window as the built-ins; a disabled extension contributes **no tools to the manifest at all**.

**Learn more:**

- **Full author guide:** [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md) — the contract, the tool builder, lifecycle, ordering, isolation semantics, and versioning.
- **Working sample:** [`samples/UnrealAITemplate/`](samples/UnrealAITemplate) — a complete, buildable plugin with a `hello-extension` tool and a compile-time switch (`UNREAL_AI_TEMPLATE_INVALID_SCHEMA=1`) that demonstrates the isolation behaviour first-hand.

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Configuration & environment variables

The plugin reads configuration with the precedence **process env → `<Project>/.env` → config file → built-in defaults**. The recognized `UNREAL_MCP_*` variables (defined in `UnrealMCP/Source/UnrealMcpEditor/Private/Config/UnrealMcpConfig.cpp`):

| Variable | Purpose |
| --- | --- |
| `UNREAL_MCP_CONNECTION_MODE` | `Cloud` or `Custom`. **Defaults to `Cloud`.** |
| `UNREAL_MCP_HOST` | Server URL (Custom mode), e.g. `http://localhost:<port>` |
| `UNREAL_MCP_CLOUD_URL` | Cloud backend URL (defaults to ai-game.dev) |
| `UNREAL_MCP_TOKEN` | Auth token (sidecar IPC token / server token). **Secret — never commit.** |
| `UNREAL_MCP_AUTH_OPTION` | `none` or `required` (local server auth) |
| `UNREAL_MCP_KEEP_CONNECTED` | Persist/restore the connected state |
| `UNREAL_MCP_TOOLS` | Enabled-tools override (whitelist; empty = no filter) |
| `UNREAL_MCP_START_SERVER` | Parsed and persisted as the `startServer` config flag (Custom mode); auto-spawning the local `gamedev-mcp-server` is **planned, not yet wired** — no code consumes this flag today, so start the server yourself (e.g. `unreal-mcp-cli`). |
| `UNREAL_MCP_TRANSPORT` | `stdio` or `http` |
| `UNREAL_MCP_LOG_LEVEL` | Log verbosity |
| `UNREAL_MCP_BRIDGE_PATH` | Path to a sidecar binary — the **dev/CI override**; wins over the bundled binary (§6). A packaged release auto-resolves the bundled sidecar, so end users never set this; from-source builds use it. |
| `UNREAL_MCP_SERVER_PATH` | Path to a local `gamedev-mcp-server` binary (read by `unreal-mcp-cli`, not the plugin) — skips the server download + version check (§6). |

> **Never commit `.env`.** A project-root `.env` can hold `UNREAL_MCP_TOKEN`, and UE project templates ship no `.gitignore`. `unreal-mcp-cli configure` appends `.env` to the target project's `.gitignore`; this repo's own scaffold already gitignores it. The sidecar IPC token travels over stdin (never argv) and is never logged.

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Troubleshooting

- **`No connected clients. Retrying [1..10]` then HTTP 500 from a local server.** The connection mode defaulted to `Cloud`, so the sidecar dialed ai-game.dev instead of your local server. Set `UNREAL_MCP_CONNECTION_MODE=Custom` (env, `.env`, or the UI toggle).
- **No `[Unreal-MCP] plugin loaded` line at boot.** The editor module failed to load — check the Output Log for a `StartupModule` error or a malformed `.uplugin`.
- **A tool body returns `'x' is required.` when invoked over the REST passthrough.** Send `-H "Content-Type: application/json"`; without it the server drops the JSON body.
- **Screenshot tools return a structured error.** Pixel capture needs a GPU-backed editor — they cannot render under headless `-nullrhi`.
- **No sidecar / sidecar keeps restarting.** Check the bridge status in the **Connection** section (`Running (restarts: N)` / `Stopped`). If the log shows `no sidecar binary resolved for rid <rid> …`, neither a bundled binary nor `UNREAL_MCP_BRIDGE_PATH` resolved — in a packaged release this means the plugin was packaged without your platform's bridge (reinstall the correct build); in a from-source build, set `UNREAL_MCP_BRIDGE_PATH` or run `unreal-mcp-cli bootstrap-local` to build one from source. (The sidecar is bundled inside packaged releases and auto-spawned, §6 — there is no download-on-first-run.)
- **Ports.** IPC uses a deterministic per-project port in `30000–39999` and probes forward (then an ephemeral port) on a collision; the local server uses a deterministic hash port in `20000–29999` with no probing — the CLI derives the same number without reading any config, and the server binds the exact requested port. The exact number is a debugging nicety, not a requirement.
- **Logs.** Use the main window's **Open log file** action, the `console-get-logs` tool, or the editor Output Log (`LogUnrealMcp` category).

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# How Unreal MCP Architecture Works

Unreal-MCP is a bridge between LLMs and the Unreal Editor. It exposes and explains Unreal's tools to the LLM, which then understands the interface and uses the tools according to your requests.

Because the editor is **C++**, the .NET `McpPlugin` host doesn't run in-process (as it does in Unity/Godot). Instead the plugin **listens** on a localhost TCP port and spawns an auto-managed **sidecar process** (`unreal-mcp-bridge`) that **dials** it, authenticating with a one-shot token delivered over stdin. The sidecar relays IPC ⇄ SignalR to the MCP server (cloud `ai-game.dev` by default, or a local `gamedev-mcp-server`). The AI Tools the plugin registers are then callable by any MCP-aware AI agent. The authoritative design — IPC protocol, dynamic tool registration, schema generation, the game-thread dispatcher, extensions, sidecar lifecycle, UI, and config — lives in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (start at the §0 system-overview diagram).

## What is `MCP`

MCP — Model Context Protocol. In a few words, it is `USB Type-C` for AI, specifically for LLMs (Large Language Models). It teaches the LLM how to use external features — such as the Unreal Engine in this case, or even your own custom C++ tool. [Official documentation](https://modelcontextprotocol.io/).

## What is an `AI agent`

It is an application with a chat window. It may have smart agents to operate better, and embedded advanced MCP Tools. A well-built MCP client is 50% of the AI success in executing a task — which is why it is important to choose a good one.

## What is the `MCP Server`

It is the bridge between the `MCP Client` and "something else" — in this case the Unreal Editor. In **Cloud** mode this is the hosted `ai-game.dev` backend; in **Custom** mode it is the shared [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) host you run yourself.

## What is an `MCP Tool`

An `MCP Tool` is a function the LLM can call to interact with Unreal. These tools are the bridge between natural-language requests and actual Unreal operations. When you ask the AI to "spawn an actor" or "compile this Blueprint," it uses MCP Tools to execute the action. Tools have typed, described parameters; return structured results; and run on the editor game thread via the dispatcher (the IPC reader thread never executes tool bodies).

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Repo layout

| Path | What it is |
| --- | --- |
| [`UnrealMCP/`](UnrealMCP/) | The UE editor plugin (C++, module `UnrealMcpEditor`, UE 5.5+ floor, developed against 5.7) |
| [`bridge/`](bridge/) | The .NET 9 sidecar (`unreal-mcp-bridge`) — McpPlugin host, IPC ⇄ SignalR relay |
| [`cli/`](cli/) | `unreal-mcp-cli` npm package (TypeScript) — 16 commands |
| [`samples/UnrealAITemplate/`](samples/UnrealAITemplate/) | Extension template plugin (`hello-extension`) |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | The authoritative architecture design |
| [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md) | Extension author guide |
| [`docs/RELEASING.md`](docs/RELEASING.md) | CI/CD + operator release runbook |

# Links

- [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) — the Unity sibling
- [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) — the Godot sibling
- [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) — the shared local MCP server (`gamedev-mcp-server`)
- [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) — the shared .NET MCP plugin/server core (`com.IvanMurzak.McpPlugin`)
- [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet) — the shared reflection/serialization core
- [ai-game.dev](https://ai-game.dev) — the cloud backend
- [Model Context Protocol](https://modelcontextprotocol.io/)

# License

[Apache-2.0](LICENSE) © Ivan Murzak
