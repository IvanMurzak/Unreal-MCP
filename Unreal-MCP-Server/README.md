# Unreal-MCP-Server

A thin [ASP.NET Core](https://learn.microsoft.com/aspnet/core/) host around the
[Model Context Protocol](https://modelcontextprotocol.io/) server core
(`com.IvanMurzak.McpPlugin.Server`). It bridges MCP clients (Claude, Cursor,
Copilot, etc.) and the Unreal Editor / Unreal games via the
[`UnrealMCP`](../UnrealMCP) plugin's sidecar bridge (`unreal-mcp-bridge`) over
SignalR.

This is the Unreal analog of `Unity-MCP-Server` / `Godot-MCP-Server`. The server
logic lives entirely in the `McpPlugin.Server` NuGet package — there is no
Unreal-specific server code here. `src/Program.cs` simply wires Kestrel /
SignalR / NLog and delegates to the `McpPlugin.Server` extension methods.

It is used in **Custom/local connection mode** only; in Cloud mode the plugin's
sidecar talks to [ai-game.dev](https://ai-game.dev) instead (see
[`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) §0).

## Build / run

```bash
# Build
dotnet build com.IvanMurzak.Unreal.MCP.Server.csproj

# Run (HTTP transport on port 8080)
dotnet run --project com.IvanMurzak.Unreal.MCP.Server.csproj -- --client-transport streamableHttp --port 8080

# Run (STDIO transport — for local MCP clients)
dotnet run --project com.IvanMurzak.Unreal.MCP.Server.csproj -- --client-transport stdio
```

`build-all.sh` / `build-all.ps1` produce self-contained single-file binaries for
win/linux/osx RIDs under `./publish/` plus `unreal-mcp-server-<rid>.zip` archives.

## NuGet pins

`com.IvanMurzak.McpPlugin.Server` **6.7.0** and `com.IvanMurzak.ReflectorNet`
**5.3.1** are frozen and kept in lockstep with the bridge and with Godot-MCP.
They are owned by the upstream release pipelines — never bump them here.
