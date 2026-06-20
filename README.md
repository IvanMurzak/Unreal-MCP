<div align="center" width="100%">

  <h1>✨ AI Game Developer — <i>Unreal MCP</i></h1>

[![MCP](https://badge.mcpx.dev 'MCP Server')](https://modelcontextprotocol.io/introduction)
[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.5%2B-0E1128?style=flat&logo=unrealengine&logoColor=white&labelColor=333A41 'Unreal Engine 5.5+ — CI-tested against 5.7 and 5.8')](https://www.unrealengine.com/)
[![Unreal Editor](https://img.shields.io/badge/Editor-X?style=flat&logo=unrealengine&logoColor=white&labelColor=333A41&color=2A2A2A 'Unreal Editor supported')](https://www.unrealengine.com/)
[![Unreal Runtime](https://img.shields.io/badge/Runtime-X?style=flat&logo=unrealengine&logoColor=white&labelColor=333A41&color=2A2A2A 'Unreal Runtime supported')](https://www.unrealengine.com/)
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
- ✔️ **Custom Tools, Prompts & Resources** — Register **your own** AI Tools, prompt templates, and resources from any UE plugin with **no fork** — one public, modular-feature contract ([Customize Tools, Prompts & Resources](#customize-tools-prompts--resources))
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
- [Customize Tools, Prompts & Resources](#customize-tools-prompts--resources)
- [Runtime usage (in-game)](#runtime-usage-in-game)
  - [Editor-only — exclude Unreal-MCP from packaged games](#editor-only-exclude-from-packaged-games)
- [Configuration & environment variables](#configuration--environment-variables)
- [Troubleshooting](#troubleshooting)
- [How Unreal MCP Architecture Works](#how-unreal-mcp-architecture-works)
- [Repo layout](#repo-layout)
- [Links](#links)
- [License](#license)

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Requirements

- **Unreal Engine 5.5+** (developed and CI-tested against **5.7**, and verified to build & run on **5.8**). The plugin deliberately ships **no `EngineVersion` pin** — UE treats that field as an exact-build match, not a floor, and would refuse to load on newer engines.
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

Unreal-MCP ships **62 built-in ("core") tools** across **8 families**. Tool ids are kebab-case (`actor-create`, `blueprint-compile`), matching the Unity/Godot naming convention. Extensions can add more (see [Customize Tools, Prompts & Resources](#customize-tools-prompts--resources)).

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

# Customize Tools, Prompts & Resources

**This is the headline extensibility feature.** Anyone can register their **own** AI Tools, **prompts**, and **resources** — from any third-party UE plugin — and have them appear in the MCP manifest alongside the built-ins. **No fork, no link-time coupling, no load-order assumptions.** Your contributions are discovered automatically on editor boot (and on late-load / hot-unload), merged in deterministic order, and exposed to every connected AI agent.

All three kinds use the **same** small, public, **modular-feature-based contract** — a provider interface plus a fluent registry builder, both living in the `UnrealMcpRuntime` module (re-exported by `UnrealMcpEditor`, so the same contract serves editor and [runtime](#runtime-usage-in-game) extensions). The full author guide is [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md).

## Tools

Implement [`IUnrealMcpToolProvider`](UnrealMCP/Source/UnrealMcpRuntime/Public/IUnrealMcpToolProvider.h) and declare your tools with the fluent [`FUnrealMcpToolRegistry`](UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpToolRegistry.h) builder:

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

## Prompts

Prompts are reusable, parameterized prompt templates the agent fetches via `prompts/get`. Implement [`IUnrealMcpPromptProvider`](UnrealMCP/Source/UnrealMcpRuntime/Public/IUnrealMcpPromptProvider.h) and declare prompts with [`FUnrealMcpPromptRegistry`](UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpPromptRegistry.h). Prompt arguments reuse the **same** `Param*` helpers as the tool builder; a handler returns role-tagged messages:

```cpp
#include "IUnrealMcpPromptProvider.h"
#include "UnrealMcpPromptRegistry.h"

class FMyPromptProvider : public IUnrealMcpPromptProvider
{
public:
    virtual FString GetExtensionId() const override      { return TEXT("com.foo.my-extension"); }
    virtual FText   GetDisplayName() const override      { return NSLOCTEXT("Foo", "Name", "My Extension"); }
    virtual FString GetExtensionVersion() const override { return TEXT("1.0.0"); }

    virtual void RegisterPrompts(FUnrealMcpPromptRegistry& Registry) override
    {
        Registry.Prompt(TEXT("level-design-brief"))
            .Title(TEXT("Level Design Brief"))
            .Description(TEXT("Generate a level design brief from a single 'theme' argument."))
            .Role(EUnrealMcpPromptRole::User)
            .ParamString(TEXT("theme"), TEXT("The level theme (e.g. 'haunted forest')."),
                         EUnrealMcpParamRequirement::Required)
            .Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpPromptResult
            {
                const FString Theme = Call.GetString(TEXT("theme"));
                if (Theme.IsEmpty())
                    return FUnrealMcpPromptResult::Error(TEXT("theme is required."));
                const FString Text = FString::Printf(
                    TEXT("Draft a level design brief for a \"%s\"-themed level."), *Theme);
                return FUnrealMcpPromptResult::Success(Text, EUnrealMcpPromptRole::User);
            });
    }
};

// Register under the prompt modular-feature name (and unregister in ShutdownModule):
IModularFeatures::Get().RegisterModularFeature(
    IUnrealMcpPromptProvider::GetModularFeatureName(), PromptProvider.Get());
```

`level-design-brief` is the shipped **core** prompt — see [`UnrealMCP/Source/UnrealMcpRuntime/Private/Prompts/UnrealMcpCorePrompts.cpp`](UnrealMCP/Source/UnrealMcpRuntime/Private/Prompts/UnrealMcpCorePrompts.cpp).

## Resources

Resources are addressable, readable content the agent fetches via `resources/read` — the resource's **URI is its identity**. Implement [`IUnrealMcpResourceProvider`](UnrealMCP/Source/UnrealMcpRuntime/Public/IUnrealMcpResourceProvider.h) and declare resources with [`FUnrealMcpResourceRegistry`](UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpResourceRegistry.h). A read returns content blocks — **text XOR a base64 blob** + a mime type:

```cpp
#include "IUnrealMcpResourceProvider.h"
#include "UnrealMcpResourceRegistry.h"

virtual void RegisterResources(FUnrealMcpResourceRegistry& Registry) override
{
    Registry.Resource(TEXT("unreal://project/levels"))                  // JSON (text) resource
        .Name(TEXT("Project Levels"))
        .Description(TEXT("A JSON snapshot of the active world and its levels."))
        .MimeType(TEXT("application/json"))
        .Read([](const FString& Uri) -> FUnrealMcpResourceResult
        {
            return FUnrealMcpResourceResult::Text(Uri, BuildLevelsJson(), TEXT("application/json"));
        });

    Registry.Resource(TEXT("unreal://project/icon"))                    // binary (blob) resource
        .Name(TEXT("Project Icon"))
        .Description(TEXT("A small PNG, returned as a base64 blob."))
        .MimeType(TEXT("image/png"))
        .Read([](const FString& Uri) -> FUnrealMcpResourceResult
        {
            const FString Base64 = FBase64::Encode(IconBytes, sizeof(IconBytes));
            return FUnrealMcpResourceResult::Blob(Uri, Base64, TEXT("image/png"));
        });
}

// Register under the resource modular-feature name (and unregister in ShutdownModule):
IModularFeatures::Get().RegisterModularFeature(
    IUnrealMcpResourceProvider::GetModularFeatureName(), ResourceProvider.Get());
```

`unreal://project/levels` and `unreal://project/icon` are the shipped **core** resources — see [`UnrealMCP/Source/UnrealMcpRuntime/Private/Resources/UnrealMcpCoreResources.cpp`](UnrealMCP/Source/UnrealMcpRuntime/Private/Resources/UnrealMcpCoreResources.cpp). Only **static, fixed-URI** resources are supported today (templated / parameterized URIs are deferred). A blob is base64 on the Unreal side and the IPC wire; a known **upstream** quirk in the shared GameDev-MCP-Server / MCP-Plugin-dotnet can mis-emit blob bytes on the final MCP wire to the client (text resources round-trip cleanly end-to-end) — that is an upstream issue, not the Unreal plugin or bridge.

**How it behaves** (identical for tools, prompts, and resources):

- **Auto-discovery & hot-reload.** Unreal-MCP enumerates every registered provider (`UnrealMcpToolProvider` / `UnrealMcpPromptProvider` / `UnrealMcpResourceProvider`) on boot, and subscribes to register/unregister events — so loading or unloading your plugin at any time triggers a **registry rebuild** and a **manifest revision bump**, and the sidecar diffs the new manifest and adds/removes the affected tools / prompts / resources automatically. You never push anything yourself.
- **Deterministic merge.** Providers are merged in ascending `GetExtensionId()` order; within one provider, entries register in declaration order. Your `GetExtensionId()` is stamped onto everything you contribute (don't call `.ExtensionId(...)` yourself).
- **Per-extension isolation.** Each descriptor is validated (a tool/prompt needs a kebab-case name + well-formed schema + bound handler; a resource needs a non-empty URI + handler). An **invalid** or **duplicate** entry (duplicate tool/prompt **name**, or duplicate resource **URI**) is dropped/rejected and the reason is recorded on **your** extension's record — your other valid entries, and every other extension, are unaffected. (UE builds without C++ exceptions, so isolation is descriptor-level, not handler-body-level; validate inputs and fail gracefully with the `Error(...)` result helpers.)
- **Toggles & enable/disable.** Extension-contributed tools are toggled in the **MCP Tools** window like the built-ins; a disabled extension contributes nothing to any manifest. (The **MCP Prompts** / **MCP Resources** windows are wired but still render empty in this release — see [Per-tool enable / disable](#per-tool-enable--disable) — so prompt/resource toggling from the UI is a follow-up; registration and use work today.)

**Learn more:**

- **Full author guide:** [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md) — the contract for tools, prompts, and resources, the builders, lifecycle, ordering, isolation semantics, and versioning.
- **Design:** [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §5 (tools) + **§A** (the prompt/resource registration path).
- **Working samples:** [`samples/UnrealAITemplate/`](samples/UnrealAITemplate) — a complete, buildable **editor** extension plugin with a `hello-extension` tool and a compile-time switch (`UNREAL_AI_TEMPLATE_INVALID_SCHEMA=1`) that demonstrates the isolation behaviour first-hand; [`samples/UnrealAIRuntimeSample/`](samples/UnrealAIRuntimeSample) — the **runtime (in-game)** counterpart, a `Type=Runtime` plugin whose `game-time-dilation` tool reads/sets the live world's time dilation, callable in a running game over a runtime MCP connection ([`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §12.9; see EXTENSIONS.md "Runtime usage"). For prompts and resources, the shipped **core** families ([`UnrealMcpCorePrompts.cpp`](UnrealMCP/Source/UnrealMcpRuntime/Private/Prompts/UnrealMcpCorePrompts.cpp) `level-design-brief`, [`UnrealMcpCoreResources.cpp`](UnrealMCP/Source/UnrealMcpRuntime/Private/Resources/UnrealMcpCoreResources.cpp) `unreal://project/levels` + `unreal://project/icon`) are the runnable reference.

![AI Game Developer — Unreal MCP](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/img/promo/hazzard-divider.svg?raw=true)

# Runtime usage (in-game)

Everything above drives the **editor**. Unreal-MCP can also run **inside a running game** — PIE, Standalone, or a packaged **Development** build — so an AI assistant can drive your game live. This is the Unreal counterpart of [Unity-MCP's runtime (in-game) support](https://github.com/IvanMurzak/Unity-MCP#runtime-usage-in-game): the UE analog of Unity's `UnityMcpPluginRuntime.Initialize().Build().Connect()` and its `[AiTool]` Chess-bot sample.

The runtime entry point is a `UGameInstanceSubsystem`, [`UUnrealMcpRuntimeSubsystem`](UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpRuntimeSubsystem.h) (in the plugin's `UnrealMcpRuntime` **runtime module**). It is auto-instantiated once per `UGameInstance` but **never auto-connects** — a connection is always an explicit, opt-in call (see [the security contract](#runtime-security-contract) below).

## Connect three ways

All three reach the same `UUnrealMcpRuntimeSubsystem::Connect(Host, Token, Mode, bAllowRemoteHost)`; the connection mode defaults to **Custom** (a developer-supplied loopback server).

**1. From C++** (e.g. your `GameMode::BeginPlay`):

```cpp
#include "UnrealMcpRuntimeSubsystem.h"

void AMyGameMode::BeginPlay()
{
    Super::BeginPlay();

    if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
        Mcp->Connect(TEXT("http://localhost:8080"), TEXT("my-token")); // Custom mode, loopback
    // ... and, when you are done:
    //  Mcp->Disconnect();
}
```

`Get(WorldContext)` is a static `BlueprintPure` helper that returns the subsystem for the context's game instance (or null). `Connect` returns `false` (and connects nothing) if any security gate rejects — see below.

**2. From Blueprint** — `Get Unreal MCP Runtime Subsystem` (the `Get` node, `WorldContext`-aware) → **`Connect`** (a `BlueprintCallable` node under the **Unreal MCP** category; `Token` / `Mode` / `bAllowRemoteHost` are advanced pins). Pair it with the **`Disconnect`** node and the **`Is Connected`** pure node for status.

**3. From the console** (QA convenience, no recompile) — registered while the subsystem is alive:

```
UnrealMcp.Connect <host> [token]
UnrealMcp.Disconnect
```

The console path always uses loopback + Custom mode.

## Your own in-game tools, prompts & resources

A game ships its **own** gameplay tools (and, if useful, prompts and resources) and the AI drives them live — the UE analog of Unity's `WithToolsFromAssembly` / `[AiTool]` Chess-bot. You author all three exactly as for an editor extension (the [Customize Tools, Prompts & Resources](#customize-tools-prompts--resources) section above), with two changes for a game module: make it **`Type=Runtime`** and depend on **`UnrealMcpRuntime`** (not `UnrealMcpEditor`). Register your providers at module startup, or use the subsystem's discoverable wrappers — one register/unregister pair per kind:

```cpp
if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
{
    Mcp->RegisterToolProvider(MyToolProvider);          // tools merge in; manifest re-pushed
    Mcp->RegisterPromptProvider(MyPromptProvider);      // prompts merge in; manifest re-pushed
    Mcp->RegisterResourceProvider(MyResourceProvider);  // resources merge in; manifest re-pushed
}
// ... before destroying the providers, call the matching Unregister*Provider(...) for each.
```

The complete, buildable example is [`samples/UnrealAIRuntimeSample/`](samples/UnrealAIRuntimeSample) — a `Type=Runtime` plugin whose **`game-time-dilation`** tool reads/sets the live world's `AWorldSettings::TimeDilation` (slow-motion / fast-forward), callable in a running game over a runtime MCP connection. (It demonstrates the tool path; prompts and resources register through the same three-wrapper API shown above and the shipped core families are the runnable reference.) The author-side details (the contract, lifecycle, ordering, isolation) are in [`docs/EXTENSIONS.md` → Runtime usage](docs/EXTENSIONS.md#runtime-usage-in-game-extensions).

## Runtime tool set vs editor-only

A runtime connection ships exactly **one** built-in tool: **`ping`** (a liveness probe + a non-empty manifest). Everything a runtime AI agent can do beyond `ping` is **bring-your-own**: register your own tools via the extension bus above (`RegisterToolProvider`), and they appear on top of `ping`.

All of the engine-development families — the actor / component family, `object-get-data` / `object-modify`, `level-get-data`, the console / reflection tools, every screenshot tool, plus Blueprint authoring, asset / Content-Browser operations, C++ source edit & compile, level create/open/save, and editor-application state — are **editor-only** (the [62 editor tools](#tools)). They drive the editor and several are RCE-class (e.g. `reflection-method-call`, `console-run-command`), so they are not compiled into a shipped game by default. There is no editor in a packaged game, so the runtime built-in surface is intentionally just `ping` + whatever tools your game registers.

## <a id="editor-only-exclude-from-packaged-games"></a>Editor-only — exclude Unreal-MCP from packaged games

If you only want Unreal-MCP for **AI tooling inside the Unreal Editor** and intend to ship a packaged game with **zero Unreal-MCP footprint** — no runtime module, no bundled `.NET` sidecar binary — pin the plugin to the editor with a **`TargetDenyList`** in your **consumer project's own `.uproject`** (not the plugin's `.uplugin`):

```jsonc
// <YourProject>.uproject — "Plugins" array
{
    "Name": "UnrealMCP",
    "Enabled": true,
    "TargetDenyList": [ "Game", "Client", "Server" ]
}
```

UE honours `TargetDenyList` / `TargetAllowList` on a plugin reference (`PluginReferenceDescriptor::IsEnabledForTarget`): the plugin stays active in the **Editor** target, but is **excluded from packaged `Game` / `Client` / `Server` builds** — so neither the runtime infra module nor the bundled `unreal-mcp-bridge` sidecar is staged or compiled into your shipped product. Editor-type modules (`UnrealMcpEditor`) are stripped from a game build regardless; the deny-list also drops the `UnrealMcpRuntime` infra module and its sidecar `RuntimeDependencies` (§12.5), giving you a **zero compiled footprint**.

> **Caveat — do not create a direct module dependency.** A `TargetDenyList` only excludes the plugin's *own* modules. If one of your **game** modules adds `UnrealMcpRuntime` (or any `UnrealMcp*` module) to its `*.Build.cs` `PublicDependencyModuleNames` / `PrivateDependencyModuleNames`, or `#include`s an `UnrealMcp*` header, that direct dependency **overrides the deny-list** and UBT compiles the runtime module into your game anyway. A pure editor-only project references nothing from the plugin, so it stays clean by construction — only opt into a module dependency when you actually want [runtime (in-game) usage](#runtime-usage-in-game).

For **runtime (in-game)** usage instead, leave the deny-list **off**: the `UnrealMcpRuntime` infra module + the `ping` built-in ship into the packaged build (§12.5), and you register your own gameplay tools via `IUnrealMcpToolProvider` — see [Runtime usage (in-game)](#runtime-usage-in-game) and [Customize Tools, Prompts & Resources](#customize-tools-prompts--resources). The two are mutually exclusive: deny-list for an editor-only project, no deny-list for a game that hosts a live MCP connection.

## <a id="runtime-security-contract"></a>Security contract

A runtime connection is **remote control of a running game** (`actor-create`, `object-modify`, arbitrary CVars via `console-run-command`, arbitrary `UFunction`s via `reflection-method-call`) — RCE-class if it were reachable in a shipped product. The runtime surface is therefore locked down by **five layered mitigations** ([`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §12.8), all enforced inside `Connect()`:

1. **Opt-in only.** The subsystem auto-instantiates but **never auto-connects** — there is no auto-dial on load and no config-asset auto-connect. A connection only ever happens through an explicit `Connect()` call.
2. **Kill switch, default OFF.** `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled` (Project Settings → Plugins → **Unreal MCP (Runtime)**) defaults to **`false`**. While it is off, every `Connect()` is rejected and no sidecar is spawned. It is a **Game** config setting (`DefaultGame.ini`, section `[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]`), so it travels into the packaged build where the gate must take effect. Turning it on does **not** auto-connect — an explicit `Connect()` is still required.
3. **Shipping gate.** The `bUnrealMcpAllowShipping` `*.Build.cs` flag defaults to **`false`** (→ `UNREAL_MCP_ALLOW_SHIPPING=0`); in a **Shipping** build `Connect()` logs and returns `false` unless that flag was deliberately compiled in. Development / PIE builds are unaffected.
4. **Loopback-host default.** `Connect()` rejects any non-loopback `Host` unless the caller explicitly passes `bAllowRemoteHost = true`. The default keeps the connection on `localhost` / `127.0.0.0/8` / `::1`.
5. **Loopback IPC + one-shot stdin token.** The plugin ⇄ sidecar IPC is loopback-only and authenticated with a one-shot token delivered over **stdin, never argv**, and never logged (same model as the editor, ARCHITECTURE §1.4).

Additionally, the runtime path is **Desktop-only** (Win64 / Mac / Linux): spawning the .NET `unreal-mcp-bridge` sidecar process is not possible on console or mobile platforms, so those platforms have no runtime MCP surface at all.

> **End-to-end runbook.** The full operator walkthrough — enabling the kill switch, connecting from PIE and a packaged Development build, and exercising tools over the live connection — is in [`docs/RUNTIME-E2E.md`](docs/RUNTIME-E2E.md). The deterministic half (security gates, world-resolver switching, connect/disconnect/orphan-safety) is locked by headless Automation specs.

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
| [`UnrealMCP/`](UnrealMCP/) | The UE editor plugin (C++, module `UnrealMcpEditor`, UE 5.5+ floor, developed against 5.7, verified on 5.8) |
| [`bridge/`](bridge/) | The .NET 9 sidecar (`unreal-mcp-bridge`) — McpPlugin host, IPC ⇄ SignalR relay |
| [`cli/`](cli/) | `unreal-mcp-cli` npm package (TypeScript) — 16 commands |
| [`samples/UnrealAITemplate/`](samples/UnrealAITemplate/) | Editor extension template plugin (`hello-extension`) |
| [`samples/UnrealAIRuntimeSample/`](samples/UnrealAIRuntimeSample/) | Runtime (in-game) extension sample (`game-time-dilation`, `Type=Runtime`) |
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
