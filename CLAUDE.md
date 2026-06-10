# CLAUDE.md — Unreal-MCP

Unreal-MCP bridges LLMs (Claude, Cursor, Copilot, …) with the [Unreal Engine](https://www.unrealengine.com/)
editor via the [Model Context Protocol](https://modelcontextprotocol.io/) — the Unreal-engine sibling of
Unity-MCP and Godot-MCP. **Status: pre-alpha scaffold** — the skeleton compiles and the plugin loads, but
tool families, the IPC bridge, and the UI are not implemented yet.

**The authoritative design is [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).** Read it before implementing
anything non-trivial — it locks the IPC protocol (§1), dynamic tool registration (§2), schema generation
(§3), the game-thread dispatcher (§4), extensions (§5), sidecar lifecycle (§6), UI (§7), config (§8),
repo/versioning/CI (§9), and the tool-family roadmap (§10).

## Layout

| Path | What it is |
| --- | --- |
| `UnrealMCP/` | The UE **editor plugin**. `.uplugin` `VersionName` is the version single-source. Modules: `UnrealMcpEditor` (Type Editor, LoadingPhase Default) + `UnrealMcpEditorTests` (Automation specs, filter prefix `UnrealMcp.`) |
| `bridge/` | .NET 9 **sidecar** `com.IvanMurzak.Unreal.MCP.Bridge` (binary `unreal-mcp-bridge`) — the McpPlugin host the plugin spawns; IPC ⇄ SignalR relay. xUnit tests in `bridge/tests/` |
| `Unreal-MCP-Server/` | Thin ASP.NET Core host around `com.IvanMurzak.McpPlugin.Server` (binary `unreal-mcp-server`) — clone of Godot-MCP-Server, **no Unreal-specific server code** |
| `cli/` | `unreal-cli` npm package (TypeScript, commander, vitest). `private: true` until the publish gate |
| `samples/UnrealAITemplate/` | Extension template plugin (placeholder — lands with the extensions task) |
| `commands/bump-version.ps1` | Rewrites the version across .uplugin / bridge / server / cli (see Versioning) |

Unlike Unity/Godot, the editor is C++: the .NET `McpPlugin` host lives in the **sidecar process**, not
in-process. The plugin LISTENS on a localhost TCP port; the sidecar DIALS, authenticating with a one-shot
token delivered over **stdin** (never argv — see §1.4). Two child processes exist and must not be conflated:
`unreal-mcp-bridge` (always required) and `unreal-mcp-server` (Custom/local mode only).

## Build / test

```bash
# UE plugin — needs an UE 5.7 install and a host project (the infra repo's Unreal-Test-Project
# junctions Plugins/UnrealMCP -> this repo's UnrealMCP/):
#   UnrealBuildTool.exe UnrealTestProjectEditor Win64 Development -project=<path>\UnrealTestProject.uproject -WaitMutex
# Headless boot smoke (expect "[Unreal-MCP] plugin loaded" in the log):
#   UnrealEditor-Cmd.exe <uproject> -nullrhi -nosplash -unattended -ExecCmds="QUIT_EDITOR" -log
# Automation specs (filter prefix UnrealMcp.):
#   UnrealEditor-Cmd.exe <uproject> -nullrhi -nosplash -unattended -ExecCmds="Automation RunTests UnrealMcp; Quit" -ReportExportPath=<dir> -log

# bridge (.NET 9)
dotnet build bridge/Unreal-MCP-Bridge.sln
dotnet test  bridge/Unreal-MCP-Bridge.sln

# server (.NET 8)
dotnet build Unreal-MCP-Server/com.IvanMurzak.Unreal.MCP.Server.csproj

# cli (Node 20.19+/22.12+)
cd cli && npm install && npm run build && npm test
```

## Versioning

Single semver shared by plugin / bridge / server / cli, starting at **0.1.0**
(design §9.2). Bump with:

```powershell
.\commands\bump-version.ps1 -NewVersion "0.2.0"        # add -WhatIf to preview
```

It rewrites: `UnrealMCP/UnrealMCP.uplugin` `VersionName`, both csproj `<Version>`s,
`Unreal-MCP-Server/server.json`, `cli/package.json` (+ lock). Never hand-edit one of them alone.

**Frozen NuGet pins** (lockstep with Godot-MCP; owned by the upstream release pipelines — NEVER bump here):
`com.IvanMurzak.ReflectorNet` **5.3.1**, `com.IvanMurzak.McpPlugin` **6.7.0** (bridge),
`com.IvanMurzak.McpPlugin.Server` **6.7.0** (server). The §2.3 ProxyTool API will arrive as a minor
upstream bump (6.8.0) through its own pipeline.

## Conventions

- **Naming (design §0):** plugin `UnrealMCP`, module `UnrealMcpEditor`; C++ prefixes `FUnrealMcp*` /
  `UUnrealMcp*` / `SUnrealMcp*` (Slate) / `IUnrealMcp*` (interfaces). .NET root namespaces
  `com.IvanMurzak.Unreal.MCP.Bridge` / `.Server`. Tool ids are kebab-case (`actor-create`,
  `blueprint-compile`) matching Unity/Godot.
- C++ follows Unreal style: tabs, braces on new lines, UE types (`FString`, `TArray`), log via the
  dedicated `LogUnrealMcp` category.
- Every `.cs` starts with the ASCII-art Apache-2.0 header (copy from a neighbouring file). C++ files use
  the `// Copyright (c) 2026 Ivan Murzak ...` one-liner.
- All Unreal API calls from tool handlers must run on the game thread via the dispatcher (§4); the IPC
  reader thread never executes tool bodies. No modal UI and no synchronous waits on bridge state inside
  tool bodies.
- Tests live per design §9.1 — NO top-level test folder: `UnrealMCP/Source/UnrealMcpEditorTests/`
  (Automation specs behind `WITH_DEV_AUTOMATION_TESTS`, names under the `UnrealMcp.` filter prefix),
  `bridge/tests/` (xUnit), `cli/tests/` (vitest).
- Secrets: `.env` is gitignored and must stay that way (it can hold `UNREAL_MCP_TOKEN`); the sidecar
  IPC token travels via stdin, never argv, and is never logged.
- Commits: `<type>(<scope>): <description>` conventional commits; reference issues with `Closes #N`.
  Never `git add -A`; never commit `Binaries/`/`Intermediate/`/`bin/`/`obj/`/`node_modules/`/`dist/`.
