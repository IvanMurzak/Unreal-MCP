# Unreal-MCP

**Model Context Protocol (MCP) integration for [Unreal Engine](https://www.unrealengine.com/).**

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![test-pull-request](https://github.com/IvanMurzak/Unreal-MCP/actions/workflows/test_pull_request.yml/badge.svg)](https://github.com/IvanMurzak/Unreal-MCP/actions/workflows/test_pull_request.yml)

> **Status: beta.** The plugin, the .NET sidecar, the `unreal-mcp-cli`, the AI Game
> Developer editor UI, and **62 built-in tools across 8 families** have shipped and are exercised by
> CI. Nothing is published to a package registry yet — install from source (below). Pixel-capture
> (screenshot) tools need a GPU-backed editor; everything else runs headless.

Unreal-MCP is the Unreal Engine counterpart of
[Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) and
[Godot-MCP](https://github.com/IvanMurzak/Godot-MCP): a C++ editor plugin that exposes Unreal
Editor operations as **AI Tools** and connects them to an MCP server, so an AI assistant
(Claude, Cursor, Copilot, …) can inspect and drive your Unreal project — spawn actors, edit levels,
author Blueprints, manage assets, edit and compile C++, capture screenshots, and more — through the
same cloud backend ([ai-game.dev](https://ai-game.dev)) that powers Unity-MCP and Godot-MCP, or
through a local server you run yourself. The local server is the shared, engine-agnostic
[GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) (binary
`gamedev-mcp-server`) — one server consumed by Unity-MCP, Godot-MCP, and Unreal-MCP; no server
source lives in this repo.

Unlike Unity and Godot (C# engines that host the .NET `McpPlugin` in-process), Unreal's editor is
C++ — so the .NET MCP host runs as an auto-managed **sidecar process** (`unreal-mcp-bridge`) that
the plugin spawns and talks to over a localhost IPC channel. The full design lives in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (see the §0 system-overview diagram).

## Table of contents

- [Requirements](#requirements)
- [Install](#install)
- [First run](#first-run)
- [Tools](#tools) — all 8 families, 62 tools
- [Per-tool enable / disable & Settings](#per-tool-enable--disable--settings)
- [`unreal-mcp-cli`](#unreal-mcp-cli)
- [Writing an extension](#writing-an-extension)
- [Configuration & environment variables](#configuration--environment-variables)
- [Troubleshooting](#troubleshooting)
- [Repo layout](#repo-layout)
- [Links](#links)
- [License](#license)

## Requirements

- **Unreal Engine 5.5+** (developed and CI-tested against **5.7**). The plugin deliberately ships
  **no `EngineVersion` pin** — UE treats that field as an exact-build match, not a floor, and would
  refuse to load on newer engines.
- **.NET 9 SDK** to build the bridge sidecar. (End users who download release binaries do not
  need it — they ship self-contained. The local MCP server is downloaded as a prebuilt
  [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) release binary, never
  built here.)
- **Node.js** `^20.19.0 || >=22.12.0` for the optional `unreal-mcp-cli`.
- A C++ Unreal project (the plugin builds an Editor module, so the host project must be able to
  compile C++).

## Install

> No package-registry release exists yet. Until the first GitHub Release / Fab listing, install the
> plugin from source.

### Option A — `unreal-mcp-cli` (recommended)

```bash
# From a clone of this repo (see "unreal-mcp-cli" below for the npm story once published):
cd cli && npm install && npm run build

# Copy or junction the plugin into <YourProject>/Plugins/UnrealMCP, then build the editor target.
# install-plugin takes the project directory as a positional argument:
node bin/unreal-mcp-cli.js install-plugin <YourProject> --junction
```

### Option B — manual

1. Copy [`UnrealMCP/`](UnrealMCP/) into `<YourProject>/Plugins/UnrealMCP/` (or create a directory
   junction / symlink to it for live development).
2. Open the project; UE compiles the `UnrealMcpEditor` module on first launch.
3. On editor boot the Output Log prints **`[Unreal-MCP] plugin loaded`** — that confirms the plugin
   and its game-thread dispatcher started.

The sidecar binary (`unreal-mcp-bridge`) is **not** bundled. Today the plugin resolves it from the
`UNREAL_MCP_BRIDGE_PATH` environment variable: point that at a sidecar binary, or run
`unreal-mcp-cli bootstrap-local` to build the bridge from source into
`<YourProject>/Intermediate/UnrealMCP/` and set the var to the result. With no path resolved, the
plugin's TCP listener still starts but logs `[Unreal-MCP] no sidecar binary resolved (set
UNREAL_MCP_BRIDGE_PATH)` and spawns nothing — launch the sidecar manually for the live e2e.
Automatic download-on-first-connect from GitHub Releases is **planned** (ARCHITECTURE §6) but **not
yet shipped**.

## First run

1. Open the **AI Game Developer** main window from the editor's **Tools** menu (the tab is
   registered under the Tools menu category).
2. **Choose a connection mode:**
   - **Cloud** (default) — connects to [ai-game.dev](https://ai-game.dev). Click **Authorize** to
     start the OAuth **device-code flow**: the window shows a verification URL and a short user
     code; open the URL, enter the code, approve, and the editor finishes authorizing. Use
     **Revoke** to clear the stored cloud token.
   - **Custom** — connects to a local `gamedev-mcp-server` you run (or any compatible server). Enter
     the server URL and point your AI client at it. (The plugin does not start the local server for
     you — run `unreal-mcp-cli` or your own process; see Troubleshooting.)
3. The **Connection** section shows a status dot, a status label, and a Connect / Disconnect / Stop
   button; the bridge status reads `Running (restarts: N)` or `Stopped`. Use them to confirm the
   sidecar is live.
4. Point your AI client (Claude Code, Cursor, the AI Game Developer app, …) at the server. The
   **AI agents** section lists the agents currently connected; to write an MCP client config use
   `unreal-mcp-cli setup-mcp`.

Connection settings persist to `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json`
(`Saved/` is gitignored by every UE template, so tokens never land in VCS by default).

## Tools

Unreal-MCP ships **62 built-in ("core") tools** across **8 families**. Tool ids are kebab-case
(`actor-create`, `blueprint-compile`), matching the Unity/Godot naming convention. Extensions can
add more (see [Writing an extension](#writing-an-extension)).

> This list is generated from the registration source
> (`UnrealMCP/Source/UnrealMcpEditor/Private/Tools/UnrealMcp*Tools.cpp`). Counts: actor 13,
> blueprint 11, asset 11, editor/reflection 9, level 7, source 6, screenshot 4, ping 1 = **62**.

### Actor & component family (13)

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

### Blueprint family (11) — Unreal's flagship surface

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

### Asset / Content-Browser family (11)

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

### Editor / console / reflection family (9)

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

### Level / map family (7)

| Tool id | What it does |
| --- | --- |
| `level-create` | Create a new level |
| `level-open` | Open a level |
| `level-save` | Save the level (save-as via optional path) |
| `level-get-data` | Actor-tree snapshot of a level (scoped reads) |
| `level-list-loaded` | List persistent + streaming sublevels (World-Partition aware, read-only) |
| `level-set-current` | Set the current/active level |
| `level-unload-sublevel` | Unload a streaming sublevel |

### Source / C++ family (6)

| Tool id | What it does |
| --- | --- |
| `source-read` | Read a project C++ source file (sliced) |
| `source-create-class` | Scaffold a new C++ class (header + cpp from templates) |
| `source-update` | Edit a source file |
| `source-delete` | Delete a source file |
| `source-list` | List module source files |
| `source-compile` | Compile project C++ (Live Coding when active, else UBT) with a structured error report |

All file operations are jailed to `<Project>/Source/`.

### Screenshot / viewport-capture family (4)

| Tool id | What it does |
| --- | --- |
| `screenshot-viewport` | Capture the active editor viewport |
| `screenshot-game-view` | Capture the PIE / game view |
| `screenshot-camera` | Render from a resolved camera actor via `USceneCaptureComponent2D` |
| `screenshot-isolated` | Render an actor in isolation (transient SceneCapture2D + show-only list) |

Captures return a base64 **PNG as MCP image content** so the LLM can inspect the render directly.
Dimensions are clamped (default 1024, hard cap 2048 per side). Pixel capture needs a GPU-backed
editor; under headless `-nullrhi` these tools return a structured error.

### Ping family (1)

| Tool id | What it does |
| --- | --- |
| `ping` | Liveness probe — round-trips the plugin ⇄ sidecar ⇄ server chain |

## Per-tool enable / disable & Settings

Every tool can be individually enabled or disabled from the **MCP Tools** window — the standalone
**MCP Tools** tab (registered under the editor's **Tools** menu). The window shows each tool's
title, family, and description, plus an "N / M tools enabled" summary line. Disabling a tool:

- **removes it from the served manifest entirely** — it never appears in the MCP `tools/list`; and
- is **enforced at the execution boundary too** — even if a stale `tools/list` is dispatched, a
  disabled tool is rejected at `Execute()` rather than run.

Two filters combine to decide whether a tool is served (see ARCHITECTURE §7/§8):

- a **whitelist** (`enabledTools`, overridable via `UNREAL_MCP_TOOLS`) — when non-empty, only listed
  tools are served; empty means "no filter"; **and**
- a **blocklist** (`disabledTools`) — the per-tool toggles you flip in the UI.

A tool is served **iff** it passes the whitelist **and** is not in the blocklist. Both sets are
persisted across editor sessions and survive an extension hot-reload (a re-registered tool inherits
the retained toggle, so a rebuild can never silently re-enable a tool you disabled).

The **Settings** page is reachable both as an aux tab and via
**Project Settings → Plugins → AI Game Developer** (`ISettingsModule`). The **MCP Prompts** and
**MCP Resources** windows are wired but ship empty in this release — each renders a subdued
empty-state message (the "N / M enabled" summary is unique to the Tools window).

## `unreal-mcp-cli`

A cross-platform Node CLI (`unreal-mcp-cli`) that scaffolds projects, installs the plugin, configures
connection settings, drives the local server, and invokes tools over HTTP. It is a port of
`unity-mcp-cli` / `godot-cli`. Full reference: [`cli/README.md`](cli/README.md).

> The npm package is `private: true` until the first publish gate. Until then, build it from source
> (`cd cli && npm install && npm run build`) and invoke `node bin/unreal-mcp-cli.js <command>`.

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

## Writing an extension

Any third-party UE plugin can contribute its own MCP tools through a small, public,
modular-feature-based contract — no fork, no link-time coupling, no load-order assumptions.
Implement [`IUnrealMcpToolProvider`](UnrealMCP/Source/UnrealMcpEditor/Public/IUnrealMcpToolProvider.h)
and register it as a modular feature:

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
            .ReadOnlyHint(true).IdempotentHint(true)
            .Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult { /* … */ });
    }
};

// In StartupModule:
IModularFeatures::Get().RegisterModularFeature(
    IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
```

Unreal-MCP discovers every registered provider on boot (and on late-load / hot-unload), merges
their tools in deterministic `ExtensionId` order, and surfaces the merged manifest to the sidecar.
Invalid or duplicate tools are dropped/rejected per-extension without affecting others. Individual
tools (including those contributed by extensions) are toggled in the **MCP Tools** window; there is
no separate per-extension UI toggle in this release.

- **Full author guide:** [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md)
- **Working sample:** [`samples/UnrealAITemplate/`](samples/UnrealAITemplate) — a complete,
  buildable plugin with a `hello-extension` tool and a compile-time switch
  (`UNREAL_AI_TEMPLATE_INVALID_SCHEMA=1`) that demonstrates the isolation behaviour.

## Configuration & environment variables

The plugin reads configuration with the precedence **process env → `<Project>/.env` → config file
→ built-in defaults**. The recognized `UNREAL_MCP_*` variables (defined in
`UnrealMCP/Source/UnrealMcpEditor/Private/Config/UnrealMcpConfig.cpp`):

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
| `UNREAL_MCP_BRIDGE_PATH` | Path to a sidecar binary. **Currently the only way the plugin resolves a sidecar** (auto-download is planned §6, not yet shipped). |
| `UNREAL_MCP_SERVER_PATH` | Path to a local `gamedev-mcp-server` binary (read by `unreal-mcp-cli`, not the plugin) — skips the server download + version check (§6). |

> **Never commit `.env`.** A project-root `.env` can hold `UNREAL_MCP_TOKEN`, and UE project
> templates ship no `.gitignore`. `unreal-mcp-cli configure` appends `.env` to the target project's
> `.gitignore`; this repo's own scaffold already gitignores it. The sidecar IPC token travels over
> stdin (never argv) and is never logged.

## Troubleshooting

- **`No connected clients. Retrying [1..10]` then HTTP 500 from a local server.** The connection
  mode defaulted to `Cloud`, so the sidecar dialed ai-game.dev instead of your local server. Set
  `UNREAL_MCP_CONNECTION_MODE=Custom` (env, `.env`, or the UI toggle).
- **No `[Unreal-MCP] plugin loaded` line at boot.** The editor module failed to load — check the
  Output Log for a `StartupModule` error or a malformed `.uplugin`.
- **A tool body returns `'x' is required.` when invoked over the REST passthrough.** Send
  `-H "Content-Type: application/json"`; without it the server drops the JSON body.
- **Screenshot tools return a structured error.** Pixel capture needs a GPU-backed editor — they
  cannot render under headless `-nullrhi`.
- **No sidecar / sidecar keeps restarting.** Check the bridge status in the **Connection** section
  (`Running (restarts: N)` / `Stopped`). If the log shows `no sidecar binary resolved (set
  UNREAL_MCP_BRIDGE_PATH)`, no sidecar path was resolved — set `UNREAL_MCP_BRIDGE_PATH` or run
  `unreal-mcp-cli bootstrap-local` to build one from source. (Automatic download and the version-skew
  redownload/alert are planned §6 behavior, not yet shipped.)
- **Ports.** IPC uses a deterministic per-project port in `30000–39999` and probes forward (then an
  ephemeral port) on a collision; the local server uses a deterministic hash port in `20000–29999`
  with no probing — the CLI derives the same number without reading any config, and the server binds
  the exact requested port. The exact number is a debugging nicety, not a requirement.
- **Logs.** Use the main window's **Open log file** action, the `console-get-logs` tool, or the
  editor Output Log (`LogUnrealMcp` category).

## Repo layout

| Path | What it is |
| --- | --- |
| [`UnrealMCP/`](UnrealMCP/) | The UE editor plugin (C++, module `UnrealMcpEditor`, UE 5.5+ floor, developed against 5.7) |
| [`bridge/`](bridge/) | The .NET 9 sidecar (`unreal-mcp-bridge`) — McpPlugin host, IPC ⇄ SignalR relay |
| [`cli/`](cli/) | `unreal-mcp-cli` npm package (TypeScript) — 16 commands |
| [`samples/UnrealAITemplate/`](samples/UnrealAITemplate/) | Extension template plugin (`hello-extension`) |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | The authoritative architecture design |
| [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md) | Extension author guide |
| [`docs/RELEASING.md`](docs/RELEASING.md) | CI/CD + operator release runbook |

## Links

- [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) — the Unity sibling
- [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) — the Godot sibling
- [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) — the shared local MCP server (`gamedev-mcp-server`)
- [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) — the shared .NET MCP plugin/server core (`com.IvanMurzak.McpPlugin`)
- [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet) — the shared reflection/serialization core
- [ai-game.dev](https://ai-game.dev) — the cloud backend
- [Model Context Protocol](https://modelcontextprotocol.io/)

## License

[Apache-2.0](LICENSE) © Ivan Murzak
