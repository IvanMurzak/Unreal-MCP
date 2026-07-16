/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Text.Json.Serialization;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Ipc
{
    /// <summary>
    /// IPC message payloads for the mcp-authorize g5/g6 <b>local-server launch-arg</b> consolidation. Unity and
    /// Godot host McpPlugin in-process and call <c>ServerLaunchArguments.BuildCommandLine(...)</c> directly; the
    /// Unreal C++ editor cannot call the C# builder, so <c>FUnrealMcpServerManager</c> DELEGATES: it forwards the
    /// resolved connection facts and the sidecar calls the SHARED
    /// <c>com.IvanMurzak.McpPlugin.ServerLaunch.ServerLaunchArguments</c> builder (none/oauth/token), answering with
    /// a <see cref="ServerLaunchArgsResultMessage"/>. This is why the C++ side holds NO duplicate of the arg logic.
    ///
    /// The plugin sends a <see cref="ServerLaunchArgsRequestMessage"/> carrying the auth mode + the mode-specific
    /// credentials (token for <c>token</c>; issuer + public-url for <c>oauth</c>); the sidecar composes the argv
    /// string and returns it verbatim for <c>FPlatformProcess::CreateProc</c>.
    /// </summary>

    /// <summary>plugin → sidecar: compose the local-server launch-arg string for the given connection facts.</summary>
    public sealed class ServerLaunchArgsRequestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ServerLaunchArgs;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        /// <summary>The derived per-project local-server port the server listens on (the port the plugin already resolved via project-config).</summary>
        [JsonPropertyName("port")] public int Port { get; set; }
        /// <summary>Plugin timeout in milliseconds (the server's <c>plugin-timeout</c> launch arg).</summary>
        [JsonPropertyName("pluginTimeoutMs")] public int PluginTimeoutMs { get; set; } = IpcProtocol.DefaultToolTimeoutMs;
        /// <summary>The auth mode, lowercase: <c>none</c> / <c>oauth</c> / <c>token</c> (the shared AuthOption enum names). Unknown → none (crash-safe loopback default).</summary>
        [JsonPropertyName("authMode")] public string AuthMode { get; set; } = "none";
        /// <summary>The offline shared secret for <c>token</c> mode (NEVER logged). Ignored in none/oauth.</summary>
        [JsonPropertyName("token")] public string? Token { get; set; }
        /// <summary>The OAuth issuer base URL for <c>oauth</c> mode (e.g. https://ai-game.dev). Ignored in none/token.</summary>
        [JsonPropertyName("authIssuer")] public string? AuthIssuer { get; set; }
        /// <summary>The exact loopback resource URL for <c>oauth</c> mode (http://localhost:&lt;port&gt;/mcp/p/&lt;pin&gt;). Ignored in none/token.</summary>
        [JsonPropertyName("publicUrl")] public string? PublicUrl { get; set; }
    }

    /// <summary>
    /// sidecar → plugin: the composed launch-arg string (or a failure reason). Correlated to the request by
    /// <see cref="RequestId"/>. On <see cref="Ok"/> the <see cref="Args"/> string is the exact argv the plugin passes
    /// to <c>FPlatformProcess::CreateProc</c>; otherwise <see cref="Error"/> carries a short human reason (never a secret —
    /// the token is never echoed back).
    /// </summary>
    public sealed class ServerLaunchArgsResultMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ServerLaunchArgsResult;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("ok")] public bool Ok { get; set; }
        /// <summary>A short human-readable failure reason on <c>ok == false</c> (never a secret).</summary>
        [JsonPropertyName("error")] public string? Error { get; set; }
        /// <summary>The composed launch-arg string on <c>ok == true</c> (e.g. <c>port=20123 plugin-timeout=10000 client-transport=streamableHttp auth=token token=…</c>).</summary>
        [JsonPropertyName("args")] public string? Args { get; set; }
    }
}
