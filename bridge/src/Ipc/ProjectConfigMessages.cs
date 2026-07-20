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
    /// IPC message payloads for the mcp-authorize PR 4 project-config channel (design docs 04/06). Unlike Unity
    /// and Godot — which host McpPlugin in-process and derive the pin/port in C# right where they need it — the
    /// Unreal C++ plugin cannot host the .NET stack, so the deterministic <c>ProjectIdentity</c> derivation lives
    /// only in the sidecar. When the C++ plugin needs the derived per-project <b>local-server port</b> (to start
    /// <c>FUnrealMcpServerManager</c>), it asks the sidecar for it over this channel instead of duplicating the
    /// SHA-256 derivation math in C++.
    ///
    /// The plugin sends a <see cref="ProjectConfigRequestMessage"/> (carrying its project root); the sidecar
    /// resolves the identity via <c>ProjectConnectionResolver</c> (byte-for-byte <c>ProjectIdentity</c> golden-vector
    /// parity for the pin) and answers with a <see cref="ProjectConfigResultMessage"/>.
    /// <see cref="ProjectConfigResultMessage.Port"/> arrives fully resolved by the three-level precedence the shared
    /// McpPlugin config writer also applies (auth-fixes T1 / defect A, owner ruling 2026-07-19): the project
    /// marker's <c>portOverride</c>, else a port the user typed into the Custom-mode loopback host, else the
    /// deterministic derivation. Whether the winner was a USER choice (either of the first two) rather than the
    /// derivation is surfaced via <see cref="ProjectConfigResultMessage.PortIsOverridden"/> for logging.
    /// </summary>

    /// <summary>plugin → sidecar: request the resolved {pin, derived port, serverTarget} for the plugin's project.</summary>
    public sealed class ProjectConfigRequestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ProjectConfig;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        /// <summary>
        /// The plugin's project root (<c>FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())</c>). The sidecar
        /// resolves the identity from this; when empty it falls back to the root the handshake-ack reported (cached
        /// in <c>SidecarHost.ApplyProjectIdentity</c>). Carrying it makes the request race-free against the ack.
        /// </summary>
        [JsonPropertyName("projectPath")] public string? ProjectPath { get; set; }
    }

    /// <summary>
    /// sidecar → plugin: the resolved connection identity for the requested project (mcp-authorize PR 4). Correlated
    /// to the request by <see cref="RequestId"/>. On <see cref="Ok"/> the fields are populated; otherwise
    /// <see cref="Error"/> carries a short human-readable reason (never a secret).
    /// </summary>
    public sealed class ProjectConfigResultMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ProjectConfigResult;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("ok")] public bool Ok { get; set; }
        /// <summary>A short human-readable failure reason on <c>ok == false</c> (never a secret).</summary>
        [JsonPropertyName("error")] public string? Error { get; set; }
        /// <summary>The routing pin — first 8 hex chars of the ProjectIdentity SHA-256 (D14/D15).</summary>
        [JsonPropertyName("pin")] public string? Pin { get; set; }
        /// <summary>The resolved per-project local-server bind port: the marker <c>portOverride</c> if set, else a port explicitly typed into the Custom-mode loopback host, else the SHA-derived 20000–29999 port.</summary>
        [JsonPropertyName("port")] public int Port { get; set; }
        /// <summary>Whether <see cref="Port"/> is a USER choice — the marker's <c>portOverride</c> or a port typed into the loopback host — rather than the deterministic derivation (informational; the winner is already applied to <see cref="Port"/>).</summary>
        [JsonPropertyName("portIsOverridden")] public bool PortIsOverridden { get; set; }
        /// <summary>The enrolled server target (hosted vs local) from the project marker, or null when unenrolled.</summary>
        [JsonPropertyName("serverTarget")] public string? ServerTarget { get; set; }
    }
}
