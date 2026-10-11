# bridge/ — the .NET 9 sidecar `unreal-mcp-bridge` (McpPlugin host; IPC ⇄ SignalR relay)

- `Unreal-MCP-Bridge.sln` — hand-authored, tracked solution (bridge + tests).
- `src/` — `com.IvanMurzak.Unreal.MCP.Bridge.csproj`; `Ipc/` (NDJSON protocol + messages), `Host/`
  (`SidecarHost`), `Sidecar/` (lifecycle), `Tools/` (manifest registrars, `ProxyTool`/`ProxyPrompt`/
  `ProxyResource`, surface sinks, `SkillGenerateTool`), `AgentConfig/`, `Auth/`.
- `tests/` — xUnit; one `<Subject>Tests.cs` per area, shared fakes in `Fakes.cs`.

**Rules**
- Every `.cs` starts with the ASCII-art Apache-2.0 header (copy from a neighbour).
- NuGet pins `com.IvanMurzak.ReflectorNet` / `com.IvanMurzak.McpPlugin` move in lockstep with Godot-MCP and
  are bumped only by the upstream release flow — never here. The csproj is the live source for their values.

**Publish** (ARCHITECTURE §6 bundle model): self-contained single-file per RID, trimming OFF (reflection-heavy).
`bash bridge/publish.sh [Release <rid>] [--no-zip]` (all 4 RIDs by default: win-x64, linux-x64, osx-x64,
osx-arm64; PowerShell: `publish.ps1 [-Platforms <rid>...] [-NoZip]`). Output `bridge/publish/<rid>/` holds
one apphost, no `.pdb`/loose DLLs. Self-contained engages only when a RID is given, so plain
`dotnet build`/`test` yields `bin/<cfg>/net9.0/unreal-mcp-bridge.exe`, the path the live e2e
(`UNREAL_MCP_BRIDGE_PATH`) uses. Release stages signed slices into
`UnrealMCP/Source/ThirdParty/UnrealMcpBridge/<rid>/`.
