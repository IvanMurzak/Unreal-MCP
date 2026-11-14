<div align="center">

<img src="docs/img/readme/hero.svg" alt="Unreal MCP — let Claude, Cursor, Copilot or any MCP agent inspect and drive the Unreal Editor" width="100%"/>

<br/>

[![Release](https://img.shields.io/github/v/release/IvanMurzak/Unreal-MCP?label=release&color=2F6F9F&logo=github&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/releases) [![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.5%E2%80%935.8-0E7490?logo=unrealengine&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.unrealengine.com/) [![Release build](https://img.shields.io/github/actions/workflow/status/IvanMurzak/Unreal-MCP/release.yml?label=release%20build&color=15803D&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/actions/workflows/release.yml)<br/>[![Docs](https://img.shields.io/badge/docs-ai--game.dev-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev) [![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://discord.gg/cfbdMZX99G) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](LICENSE) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

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

</div>

**Unreal MCP** (AI Game Developer for Unreal) connects any MCP-aware AI agent to the **Unreal Editor**. Ask in plain words and the agent spawns actors, edits levels, authors and compiles Blueprints, edits and compiles C++, manages assets and checks its work with screenshots. It is the Unreal sibling of [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) and [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP), and uses the same cloud backend ([ai-game.dev](https://ai-game.dev)) or your own [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server).

<p align="center">
  <img src="docs/img/readme/architecture.svg" alt="An AI agent talks MCP to the server; the server relays over SignalR to the unreal-mcp-bridge sidecar, which reaches the UnrealMCP C++ plugin in the Unreal Editor over loopback TCP" width="100%"/>
</p>

Unreal's editor is C++, so the .NET MCP host runs next to it as a small **sidecar** (`unreal-mcp-bridge`) that the plugin starts, watches and restarts for you. Design details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Features

✅ **RQL data layer** — query and change actors, assets and levels in one language ([RQL](docs/rql.md))\
✅ **Blueprints** — create, edit and compile, with structured errors the agent can fix\
✅ **C++** — scaffold, edit and compile project source (Live Coding or UBT)\
✅ **Visual feedback** — viewport, game-view, camera and isolated-actor screenshots\
✅ **Python** — run editor Python with the full `unreal` API\
✅ **Fab** — search Fab, list your library, add owned content\
✅ **Extensions** — GAS, Niagara, PCG, Landscape, Control Rig, Enhanced Input\
✅ **Your own tools** — add tools, prompts and resources from any plugin ([guide](docs/EXTENSIONS.md))\
✅ **Runtime mode** — opt in to drive a running game ([docs/RUNTIME.md](docs/RUNTIME.md))\
✅ **Cloud or local** — `ai-game.dev` out of the box, or a self-hosted `gamedev-mcp-server`

30 built-in tools in 8 families; the full list is in [docs/TOOLS.md](docs/TOOLS.md).

## Requirements

- **Unreal Engine 5.5 – 5.8** (the release pipeline validates each of them) and a **C++ project**: the plugin compiles editor modules in your project.
- **Node.js 22.14+** for `unreal-mcp-cli`.
- Windows, macOS or Linux. No .NET install is needed: the sidecar ships inside the plugin. (.NET 9 SDK only to build the bridge from source.)

## Quick start

<p align="center">
  <img src="docs/img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, open the editor" width="100%"/>
</p>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` downloads the signed plugin source release that matches the CLI version. In the editor, open **Tools → AI Game Developer** to see the connection, the bridge status and the connected agents. Then ask your agent:

> *"Spawn three cubes in a row and a point light above them, then take a screenshot."*

Other ways to install:

- **Fab / Epic Marketplace**: coming soon. The Fab build cannot bundle executables, so there the window's **Get bridge** button walks you through installing the sidecar once.
- **Manual**: copy [`UnrealMCP/`](UnrealMCP/) into `<YourProject>/Plugins/UnrealMCP/`. A source checkout has no bundled sidecar: run `unreal-mcp-cli bootstrap-local` or set `UNREAL_MCP_BRIDGE_PATH`.

Updating (`unreal-mcp-cli update`), connection modes, environment variables and troubleshooting: [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

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

Add an engine extension (each one extends RQL, so no new tools appear in `tools/list`):

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

Extensions: [AI GAS](extensions/UnrealAIGAS/README.md), [AI Niagara](extensions/UnrealAINiagara/README.md), [AI PCG](extensions/UnrealAIPCG/README.md), [AI Landscape](extensions/UnrealAILandscape/README.md), [AI ControlRig](extensions/UnrealAIControlRig/README.md), [AI EnhancedInput](extensions/UnrealAIEnhancedInput/README.md). They need Unreal Engine 5.7+ and compile from source in your project.

All commands and options: [cli/README.md](cli/README.md) or `unreal-mcp-cli commands`.

## Documentation

| Page | What is in it |
|---|---|
| [docs/TOOLS.md](docs/TOOLS.md) | Every built-in and system tool, extensions, per-tool visibility |
| [docs/rql.md](docs/rql.md) | The RQL query language: sources, members, mutations, play testing |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | First run, updating, environment variables, troubleshooting |
| [docs/EXTENSIONS.md](docs/EXTENSIONS.md) | Write your own RQL modules, tools, prompts and resources |
| [docs/RUNTIME.md](docs/RUNTIME.md) | Runtime (in-game) mode, its security gates, keeping the plugin out of shipped games |
| [cli/README.md](cli/README.md) | `unreal-mcp-cli` command reference and exit codes |
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

[Apache-2.0](LICENSE) © Ivan Murzak
