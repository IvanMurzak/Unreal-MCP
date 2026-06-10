# Unreal-MCP

**Model Context Protocol (MCP) integration for [Unreal Engine](https://www.unrealengine.com/).**

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

> **Status: pre-alpha.** This repository is a scaffold under active development — the plugin
> compiles and loads, but no MCP tools are implemented yet. Nothing here is released or
> production-ready.

Unreal-MCP is the Unreal Engine counterpart of
[Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) and
[Godot-MCP](https://github.com/IvanMurzak/Godot-MCP): a C++ editor plugin that exposes Unreal
Editor operations as **AI Tools** and connects them to an MCP server, so an AI assistant can
inspect and drive your Unreal project — spawn actors, edit levels, author Blueprints, manage
assets, capture screenshots, and more — through the same cloud backend
([ai-game.dev](https://ai-game.dev)) that powers Unity-MCP and Godot-MCP.

Unlike Unity and Godot (C# engines that host the .NET `McpPlugin` in-process), Unreal's editor
is C++ — so the .NET MCP host runs as an auto-managed **sidecar process** (`unreal-mcp-bridge`)
that the plugin spawns and talks to over a localhost IPC channel. The full design lives in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (see the §0 system-overview diagram).

## Repo layout

| Path | What it is |
| --- | --- |
| [`UnrealMCP/`](UnrealMCP/) | The UE editor plugin (C++, module `UnrealMcpEditor`, UE 5.5+ floor, developed against 5.7) |
| [`bridge/`](bridge/) | The .NET 9 sidecar (`unreal-mcp-bridge`) — McpPlugin host, IPC ⇄ SignalR relay |
| [`Unreal-MCP-Server/`](Unreal-MCP-Server/) | Thin local MCP server host (`unreal-mcp-server`), analog of Godot-MCP-Server |
| [`cli/`](cli/) | `unreal-cli` npm package (TypeScript) |
| [`samples/UnrealAITemplate/`](samples/UnrealAITemplate/) | Extension template plugin (placeholder) |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | The authoritative architecture design |

## Requirements

- **Unreal Engine 5.5+** (developed and CI-tested against **5.7**)
- **.NET 9 SDK** (bridge), **.NET 8 SDK** (server)
- **Node.js** `^20.19.0 || >=22.12.0` (cli)

## Build (current scaffold)

```bash
# UE plugin — put UnrealMCP/ under <YourProject>/Plugins/ (or junction it) and build the
# editor target; on editor boot the Output Log prints: [Unreal-MCP] plugin loaded

# bridge
dotnet build bridge/Unreal-MCP-Bridge.sln && dotnet test bridge/Unreal-MCP-Bridge.sln

# server
dotnet build Unreal-MCP-Server/com.IvanMurzak.Unreal.MCP.Server.csproj

# cli
cd cli && npm install && npm run build && npm test
```

## Links

- [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) — the Unity sibling
- [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) — the Godot sibling
- [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) — the shared .NET MCP plugin/server core (`com.IvanMurzak.McpPlugin`)
- [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet) — the shared reflection/serialization core
- [ai-game.dev](https://ai-game.dev) — the cloud backend
- [Model Context Protocol](https://modelcontextprotocol.io/)

## License

[Apache-2.0](LICENSE) © Ivan Murzak
