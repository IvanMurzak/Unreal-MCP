<div align="center">

<img src="docs/img/readme/hero.svg" alt="Unreal MCP — let Claude, Cursor, Copilot or any MCP agent inspect and drive the Unreal Editor" width="100%"/>

<br/>

[![Release](https://img.shields.io/github/v/release/IvanMurzak/Unreal-MCP?label=release&color=2F6F9F&logo=github&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/releases) [![Fab](https://img.shields.io/badge/Fab-AI%20Game%20Developer-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) [![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.5%E2%80%935.8-0E7490?logo=unrealengine&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.unrealengine.com/)<br/>[![Release build](https://img.shields.io/github/actions/workflow/status/IvanMurzak/Unreal-MCP/release.yml?label=release%20build&logo=github&color=15803D&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/actions/workflows/release.yml) [![Docs](https://img.shields.io/badge/docs-ai--game.dev-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev) [![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://discord.gg/cfbdMZX99G) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](LICENSE) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

  <p>
    <a href="https://claude.ai/download"><img src="docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://visualstudio.microsoft.com/"><img src="docs/img/mcp-clients/visual-studio-64.png" alt="Visual Studio" title="Visual Studio" height="36"></a>&nbsp;&nbsp;
    <a href="https://opencode.ai/"><img src="docs/img/mcp-clients/open-code-64.png" alt="OpenCode" title="OpenCode" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="docs/img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/Kilo-Org/kilocode"><img src="docs/img/mcp-clients/kilo-code-64.png" alt="Kilo Code" title="Kilo Code" height="36"></a>
  </p>

  <b>[English](README.md) | [中文](docs/README.zh-CN.md) | [日本語](docs/README.ja.md) | [Español](docs/README.es.md) | [فارسی](docs/README.fa.md) | [हिन्दी](docs/README.hi.md) | [العربية](docs/README.ar.md)</b>

</div>

**Unreal MCP** (AI Game Developer for Unreal) connects any MCP-aware AI agent to the **Unreal Editor**. Ask in plain words and the agent spawns actors, edits levels, authors and compiles Blueprints, edits and compiles C++, manages assets and checks its work with screenshots. It is the Unreal sibling of [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) and [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP), and uses the same cloud backend ([ai-game.dev](https://ai-game.dev)) or your own [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server).

<p align="center">
  <img src="docs/img/readme/architecture.svg" alt="An AI agent talks MCP to the server; the server relays over SignalR to the unreal-mcp-bridge sidecar, which reaches the UnrealMCP C++ plugin in the Unreal Editor over loopback TCP" width="100%"/>
</p>

Unreal's editor is C++, so the .NET MCP host runs next to it as a small **sidecar** (`unreal-mcp-bridge`) that the plugin starts, watches and restarts for you. Design details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

**Status:** [latest release](https://github.com/IvanMurzak/Unreal-MCP/releases/latest) (plugin, sidecar and CLI share one version) · Unreal Engine 5.5–5.8 · sidecar builds for Windows x64, macOS (Intel and Apple silicon) and Linux x64 · each release is tested on Windows (UE 5.5–5.8) and macOS (UE 5.7, 5.8) · available on [Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) and [GitHub Releases](https://github.com/IvanMurzak/Unreal-MCP/releases).

## Features

✅ **C++ authoring** — scaffold classes, edit source, compile; errors come back as file, line, severity\
✅ **Blueprints** — create, edit and compile, with structured errors the agent can fix\
✅ **RQL data layer** — query and change actors, assets and levels in one language ([RQL](docs/rql.md))\
✅ **Visual feedback** — viewport, game-view, camera and isolated-actor screenshots\
✅ **Python** — run editor Python with the full `unreal` API\
✅ **Fab** — search Fab, list your library, add owned content\
✅ **Extensions** — GAS, Niagara, PCG, Landscape, Control Rig, Enhanced Input\
✅ **Your own tools** — add tools, prompts and resources from any plugin ([guide](docs/EXTENSIONS.md))\
✅ **Runtime mode** — opt in to drive a running game ([docs/RUNTIME.md](docs/RUNTIME.md))\
✅ **Cloud or local** — `ai-game.dev` out of the box, or a self-hosted `gamedev-mcp-server`

**Coverage:** the agent can do virtually anything you can do in the Unreal Editor. Tool groups: the RQL data layer, Blueprints, C++ source, screenshots, Fab, the console, editor Python and the tool list, plus the extensions. Every tool: [docs/TOOLS.md](docs/TOOLS.md).

**Efficiency:** the tools are compact by design, so the agent needs fewer calls and fewer tokens per task and works faster and cheaper. For the RQL data layer this is measured in RQL's own evaluation (a sandbox game world, same tasks, model and grader): RQL needed about 9–18× fewer tool calls and 17–19× fewer tokens per task than conventional find / dump-as-JSON / set reflection tools ([results](https://github.com/IvanMurzak/RQL/blob/main/RESULTS.md#33-rql-vs-conventional-reflection-tools)).

### What you can ask

| Ask your agent | Tools it uses |
| --- | --- |
| "Add an `AHealthPickup` actor class in C++, compile it and fix any errors" | `source-create-class`, `source-update`, `source-compile` |
| "Make a door Blueprint that opens on overlap, compile it, place it in the level" | `blueprint-*` |
| "Count the trees in this level and move every point light 1 m up" | `rql-query`, `rql-mutate` |
| "Take a screenshot from the player camera and check the lighting" | `screenshot-*` |
| "Find a free rock pack on Fab and add it to the project" | `fab-search`, `fab-add` |
| "Batch-rename the selected assets with a Python script" | `python-execute` |

**C++ is a first-class loop.** `source-compile` builds with Live Coding when it is active, else UnrealBuildTool, and returns every diagnostic with its file, line and severity, so the agent fixes errors without reading the build log. It says when a change needs a full build and an editor restart (new classes, changed `UCLASS` / `UPROPERTY` / `UFUNCTION` declarations). File operations stay inside `<Project>/Source/`. From a terminal, `unreal-mcp-cli compile` prints `file:line:col` diagnostics without an editor.

**RQL is learned from the editor itself:** `rql-schema` lists every source, type and member, and the `unreal-rql` skill teaches the syntax with recipes (`unreal-mcp-cli setup-skills` writes it for your agent).

## Requirements

- **Unreal Engine 5.5 – 5.8** (the release pipeline validates each of them).
- **A C++ project**, because the plugin ships as source and Unreal compiles its modules for your project with your platform's C++ toolchain. A Blueprint-only project becomes a C++ project in one step: in the editor, **Tools → New C++ Class…** and create any class. A new project: `unreal-mcp-cli create-project ./MyGame` scaffolds a minimal C++ one. The Fab build needs no C++ project: the Epic Games Launcher installs it precompiled.
- **No .NET install.** The MCP host is a self-contained .NET program (`unreal-mcp-bridge`) bundled inside the plugin for your platform; the plugin starts and restarts it. (The Fab build has no executables: its **Get bridge** button opens the release page, and you extract the bridge into the plugin's bridge folder once.) Only building the bridge from source needs the .NET 9 SDK.
- **Node.js 22.14+**, only for `unreal-mcp-cli` (an npm package; 22.14 is its minimum Node version).
- **Windows or macOS** (every release is tested on both). **Linux x64:** the sidecar is built and shipped, but no release test runs on Linux.

## Quick start

**1. Install the plugin**, one of:

- **[Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)** (no terminal): add **AI Game Developer** to your engine, enable it under **Edit → Plugins**, open your project.
- **Two commands** (Node.js 22.14+):

  ```bash
  npx unreal-mcp-cli install-plugin ./MyGame   # plugin + bundled bridge into MyGame/Plugins
  npx unreal-mcp-cli open ./MyGame             # build if needed and open the editor
  ```

**2. Connect, in the editor:** open **Tools → AI Game Developer**.

- Fab install only: install the `unreal-mcp-bridge` sidecar once (the Fab build carries no executables): click **Get bridge** to open the release page, extract the zip for your platform into the folder **Open bridge folder** shows, then click **Connect**.
- **Cloud** (default): click **Authorize** and approve the sign-in in your browser. **Local**: switch to **Custom** and click **Start** next to **MCP server**; the editor downloads the checksum-verified `gamedev-mcp-server` (with a Fab install, install that server by hand: the log names the download).
- Under **AI agent**, pick yours (Claude Code, Cursor, Codex, …) and click **Configure**.

**3. Ask your agent:** *"Spawn three cubes in a row and a point light above them, then take a screenshot."*

Local or Cloud? Both are fully supported; you can switch later. **Cloud is the default** because it needs no open ports and avoids port conflicts with other programs; the connection is free. **Local** runs without an account or internet access, for offline and air-gapped machines.

| | **Local** | **Cloud** |
| --- | --- | --- |
| Account | none | ai-game.dev sign-in |
| MCP server | `gamedev-mcp-server` on your machine, started by the editor or by your agent | ai-game.dev relay |
| Traffic | stays on `localhost` | passes through ai-game.dev ([what it logs](#what-the-cloud-relay-sees)) |
| Auth | none on loopback, or a shared token (one flag) | OAuth or a per-project key |
| Good for | offline and air-gapped work, CI, private projects | no open ports or port conflicts; one account across several projects and machines |

<details>
<summary><b>Everything from the terminal</b> (CLI: Cloud, Local, token auth)</summary>

<p align="center">
  <img src="docs/img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, open the editor" width="100%"/>
</p>

**Cloud** (the plugin's default mode):

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

**Local** (no account):

```bash
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin ./MyGame
unreal-mcp-cli configure -p ./MyGame --mode Custom                 # plugin: local server
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --transport stdio # agent starts it
unreal-mcp-cli open ./MyGame
```

`setup-mcp --transport stdio` downloads the checksum-verified `gamedev-mcp-server` release into the project and points the agent at it. Both sides use the same per-project port on `localhost`.

**Turn on token auth (local):** add the flags to the `configure` line, then run `setup-mcp` again; the plugin and the server then both require the secret.

```bash
unreal-mcp-cli configure -p ./MyGame --mode Custom --auth required --token <secret>
```

Why no auth is the local default: the server binds to loopback only and rejects requests from non-local browser origins, so only programs on your own machine can reach it. Use a token when you do not trust every program or user on that machine. The secret lands in the project's `.env` (gitignored by `configure`) and in the agent's config file, so keep that file out of version control.

</details>

<a id="what-the-cloud-relay-sees"></a>**What the cloud relay sees.** In Cloud mode, tool calls pass through the ai-game.dev MCP relay. The relay is stateless: it routes tool calls between your AI client and the editor without inspecting or persisting their contents, and it does not collect your conversations, source code, project files, assets or scene data. It logs request metadata (client IP address, timestamp and duration, status code and request path, tool name, request and response size) and MCP event metadata (event type, timing, status, session identifiers, the tool, prompt or resource name, and error details). Request logs and MCP events are used primarily within a rolling 90-day window; the policy does not guarantee automatic deletion at a fixed age. Details: [Privacy Policy](https://ai-game.dev/legal/privacy).

`install-plugin` downloads the signed plugin source release that matches the CLI version.

Other ways to install:

- **Fab-format zip**: besides the [Fab listing](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33), each GitHub release also carries a Fab-format build (`unreal-mcp-plugin-fab-<version>.zip`) without executables; with either, the window's **Get bridge** button walks you through installing the sidecar once.
- **Manual**: copy [`UnrealMCP/`](UnrealMCP/) into `<YourProject>/Plugins/UnrealMCP/`. A source checkout has no bundled sidecar: run `unreal-mcp-cli bootstrap-local` or set `UNREAL_MCP_BRIDGE_PATH`.

Updating (`unreal-mcp-cli update`), connection modes, environment variables and troubleshooting: [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

### Agent setup

`unreal-mcp-cli setup-mcp <agent> -p ./MyGame` writes the config for you (`setup-mcp --list` shows all supported agents). What it writes:

<details>
<summary><b>Claude Code</b>: <code>.mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp claude-code -p ./MyGame
```

```json
{
  "mcpServers": {
    "unreal-mcp": {
      "type": "http",
      "url": "https://ai-game.dev/mcp/p/<pin>",
      "headers": { "Authorization": "Bearer agd_pk_…" }
    }
  }
}
```

The `<pin>` routes to this project's editor. The header is this project's key when the machine is signed in; `--oauth` writes a URL-only config and the agent signs in itself. With `--transport stdio` the entry runs the local server instead: `"command"` is `gamedev-mcp-server` under `<project>/Intermediate/UnrealMCP/server/`, `"args"` set `port=<port>`, `client-transport=stdio` and `authorization=none`.

</details>

<details>
<summary><b>Cursor</b>: <code>.cursor/mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp cursor -p ./MyGame
```

The same `mcpServers` → `unreal-mcp` entry as Claude Code: `"type": "http"` with the URL, or `"type": "stdio"` with `command` and `args` for the local server.

</details>

<details>
<summary><b>Codex</b>: <code>.codex/config.toml</code></summary>

```bash
unreal-mcp-cli setup-mcp codex -p ./MyGame
```

```toml
[mcp_servers.unreal-mcp]
enabled = true
url = "https://ai-game.dev/mcp/p/<pin>"
tool_timeout_sec = 300
startup_timeout_sec = 30
```

With a project key the section also carries `http_headers` with the `Authorization` header.

</details>

## Usage

Most of the time you just talk to your agent. The CLI also calls the same tools directly, starting a headless editor when none is running:

```bash
# Query the level with RQL
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'

# Change it: set / call goes to rql-mutate (add --dry-run to preview)
unreal-mcp-cli rql -p ./MyGame 'editor | call SpawnActor("PointLight")'

# Run editor Python
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"

# List every tool with its inputs, build C++ without the editor, run Automation tests
unreal-mcp-cli tools -p ./MyGame
unreal-mcp-cli compile ./MyGame
unreal-mcp-cli test ./MyGame
```

All commands and options: [cli/docs/commands.md](cli/docs/commands.md) or `unreal-mcp-cli commands`.

## Security

- **Plugin ↔ sidecar:** the plugin listens on a loopback TCP port; the sidecar dials it with a one-shot token sent over stdin (never on the command line, never logged).
- **Local server:** `gamedev-mcp-server` binds to loopback by default and answers `403` to a request whose `Origin` is not allowed (DNS-rebinding defense). On loopback the plugin starts it without authentication unless you choose a token (`UNREAL_MCP_AUTH_OPTION`, `UNREAL_MCP_TOKEN`). Server options: [hosting and authentication](https://github.com/IvanMurzak/GameDev-MCP-Server/blob/main/docs/hosting.md).
- **Cloud:** the agent signs in with OAuth or uses a per-project key you can replace with `setup-mcp --regenerate-key`. The relay is stateless and does not persist tool-call contents; it records request and MCP event metadata ([details](#what-the-cloud-relay-sees)).
- **What the tools can do:** edit files under `<Project>/Source/`, run console commands and editor Python, change any reflected property.
- **Controls over them:**
  - **Whitelist** (hard limit): `UNREAL_MCP_TOOLS` serves only the listed tools; any other tool is neither listed nor callable.
  - **Hide** (soft): the **Visible** checkbox in the **MCP Tools** window removes a tool from `tools/list`, but a client that calls it by name still runs it.
  - **Bounded changes:** C++ file tools stay inside `<Project>/Source/`; an `rql-mutate` call is all-or-nothing, one undo step, previewable with `dry_run`, and refused above 200 targets unless the call raises `max_targets` (up to 1000); `python-execute` stops after `timeout` (30 s by default); every tool call has a time limit (`UNREAL_MCP_TOOL_TIMEOUT`, 5 min by default).
  - **Not for the AI:** the system tools (`ping`, skill generation, `editor-quit`) are absent from `tools/list`, and `editor-quit` refuses unless the CLI session's nonce matches.
  - Details: [docs/TOOLS.md](docs/TOOLS.md).
- **Runtime mode is off by default:** a game never connects on its own. It needs the `bRuntimeMcpEnabled` setting (default off) and an explicit `Connect()` call, accepts only a loopback host unless you allow a remote one, and is refused in Shipping builds unless compiled in. Keep the plugin out of packaged games with a `TargetDenyList` ([docs/RUNTIME.md](docs/RUNTIME.md)).

## Extensions

Extensions are optional plugins that extend RQL; they add no tools of their own.

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

| Extension | Adds | Requires |
| --- | --- | --- |
| [AI&nbsp;ControlRig](extensions/UnrealAIControlRig/README.md) | Procedural animation rigs and their Anim Blueprint wiring | `ControlRig`, `RigVM`, UE 5.7+ |
| [AI&nbsp;EnhancedInput](extensions/UnrealAIEnhancedInput/README.md) | Input actions, mapping contexts, triggers and modifiers | `EnhancedInput`, UE 5.7+ |
| [AI&nbsp;GAS](extensions/UnrealAIGAS/README.md) | Gameplay Effects, ability Blueprints and attribute sets | `GameplayAbilities`, `EnhancedInput`, `GameplayTagsEditor`, UE 5.7+ |
| [AI&nbsp;Landscape](extensions/UnrealAILandscape/README.md) | Landscapes, oceans, lakes and rivers | `Water`, UE 5.7+ |
| [AI&nbsp;Niagara](extensions/UnrealAINiagara/README.md) | Niagara systems, emitters, modules and collision | `Niagara`, `ChaosNiagara`, UE 5.7+ |
| [AI&nbsp;PCG](extensions/UnrealAIPCG/README.md) | PCG graphs, volumes and generated scatters | `PCG`, UE 5.7+ |

Extensions compile from source in your project (on the next editor open, or now with `--build`); the installer enables the engine plugins they need.

## Documentation

| Page | What is in it |
|---|---|
| [docs/TOOLS.md](docs/TOOLS.md) | Every built-in and system tool, extensions, per-tool visibility |
| [docs/rql.md](docs/rql.md) | The RQL query language: sources, members, mutations, play testing |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | First run, updating, environment variables, troubleshooting |
| [docs/EXTENSIONS.md](docs/EXTENSIONS.md) | Write your own RQL modules, tools, prompts and resources |
| [docs/RUNTIME.md](docs/RUNTIME.md) | Runtime (in-game) mode, its security gates, keeping the plugin out of shipped games |
| [cli/docs/commands.md](cli/docs/commands.md) | `unreal-mcp-cli` command reference and exit codes |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | The authoritative design (IPC, registration, sidecar, UI, config) |
| [docs/RELEASING.md](docs/RELEASING.md) | Maintainers: releases, CI runners, secrets |
| [docs/EXTENSION-MONOREPO.md](docs/EXTENSION-MONOREPO.md) | Maintainers: testing and releasing the extensions |

## Repository layout

| Path | What it is |
|---|---|
| [`UnrealMCP/`](UnrealMCP/) | The Unreal plugin (C++): `UnrealMcpRuntime`, `UnrealMcpRql`, `UnrealMcpEditor` |
| [`bridge/`](bridge/) | `unreal-mcp-bridge`, the .NET 9 sidecar |
| [`cli/`](cli/) | `unreal-mcp-cli`, the npm package |
| [`extensions/`](extensions/) | Six optional engine extensions with their own releases |
| [`samples/`](samples/) | An editor extension template and a runtime (in-game) tool sample |

## Links

- [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G) · [Issues](https://github.com/IvanMurzak/Unreal-MCP/issues)
- Siblings: [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) · [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP)
- Shared pieces: [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) · [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) · [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet)
- [Model Context Protocol](https://modelcontextprotocol.io/)

## License

**Free for game developers, commercial games included.** Unreal MCP is [Apache-2.0](LICENSE) © Ivan Murzak. Two bundled components, RQL and Code-Exec, use the [PolyForm Shield 1.0.0](https://polyformproject.org/licenses/shield/1.0.0) license, which restricts only companies that build competing AI tools for game development; you can use all of it to build, ship and sell your games. Notices: [`UnrealMCP/ThirdPartyNotices.txt`](UnrealMCP/ThirdPartyNotices.txt).
