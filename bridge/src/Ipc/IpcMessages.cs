/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Collections.Generic;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Ipc
{
    /// <summary>
    /// Strongly-typed IPC message payloads (docs/ARCHITECTURE.md §1.3). Every message carries a
    /// <c>type</c> discriminator (<see cref="IpcProtocol.Type"/>). Inbound messages are routed by
    /// reading <c>type</c> off the parsed JSON, then deserializing the line into the matching record.
    /// </summary>

    /// <summary>sidecar → plugin: first message after connect. Carries the one-shot auth token (§1.4).</summary>
    public sealed class HandshakeMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.Handshake;
        [JsonPropertyName("ipcVersion")] public int IpcVersion { get; set; } = IpcProtocol.IpcVersion;
        [JsonPropertyName("sidecarVersion")] public string? SidecarVersion { get; set; }
        [JsonPropertyName("token")] public string? Token { get; set; }
    }

    /// <summary>plugin → sidecar: handshake acknowledgement + effective environment/config (§1.3).</summary>
    public sealed class HandshakeAckMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.HandshakeAck;
        [JsonPropertyName("ipcVersion")] public int IpcVersion { get; set; }
        [JsonPropertyName("pluginVersion")] public string? PluginVersion { get; set; }
        [JsonPropertyName("engineVersion")] public string? EngineVersion { get; set; }
        [JsonPropertyName("projectPath")] public string? ProjectPath { get; set; }
        [JsonPropertyName("config")] public JsonObject? Config { get; set; }
    }

    /// <summary>One entry in <see cref="ToolManifestMessage.Tools"/> — shaped to fill <c>IRunTool</c> 1:1 (§2.2).</summary>
    public sealed class ToolDescriptor
    {
        [JsonPropertyName("name")] public string Name { get; set; } = string.Empty;
        [JsonPropertyName("title")] public string? Title { get; set; }
        [JsonPropertyName("description")] public string? Description { get; set; }
        [JsonPropertyName("skillDescription")] public string? SkillDescription { get; set; }
        [JsonPropertyName("skillBody")] public string? SkillBody { get; set; }
        [JsonPropertyName("inputSchema")] public JsonNode? InputSchema { get; set; }
        [JsonPropertyName("outputSchema")] public JsonNode? OutputSchema { get; set; }
        [JsonPropertyName("readOnlyHint")] public bool? ReadOnlyHint { get; set; }
        [JsonPropertyName("destructiveHint")] public bool? DestructiveHint { get; set; }
        [JsonPropertyName("idempotentHint")] public bool? IdempotentHint { get; set; }
        [JsonPropertyName("openWorldHint")] public bool? OpenWorldHint { get; set; }
        [JsonPropertyName("enabled")] public bool Enabled { get; set; } = true;
        [JsonPropertyName("extensionId")] public string? ExtensionId { get; set; }
        [JsonPropertyName("schemaHash")] public string? SchemaHash { get; set; }
    }

    /// <summary>plugin → sidecar: full tool-set snapshot. The sidecar diffs it against the last applied (§2.2).</summary>
    public sealed class ToolManifestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ToolManifest;
        [JsonPropertyName("revision")] public int Revision { get; set; }
        [JsonPropertyName("tools")] public List<ToolDescriptor> Tools { get; set; } = new();
    }

    /// <summary>sidecar → plugin: invoke a tool. The plugin runs the body on the game thread (§4).</summary>
    public sealed class ToolCallMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ToolCall;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("tool")] public string Tool { get; set; } = string.Empty;
        [JsonPropertyName("arguments")] public JsonObject? Arguments { get; set; }
        [JsonPropertyName("timeoutMs")] public int TimeoutMs { get; set; } = IpcProtocol.DefaultToolTimeoutMs;
    }

    /// <summary>plugin → sidecar: terminal tool result, mirroring <c>ResponseCallTool</c> (§1.3).</summary>
    public sealed class ToolResponseMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ToolResponse;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("status")] public string Status { get; set; } = IpcProtocol.Status.Success;
        [JsonPropertyName("content")] public JsonArray? Content { get; set; }
        [JsonPropertyName("structured")] public JsonNode? Structured { get; set; }
    }

    // ── Prompts/Resources (IPC v2, docs/ARCHITECTURE.md §A.1) ────────────────────────────────────────
    // These message families are exchanged ONLY on a link whose negotiated IPC version is
    // >= IpcProtocol.PromptsResourcesMinVersion (a tools-only/v1 link never sees them). They mirror the
    // tool families 1:1 — a revision-guarded full-snapshot manifest the sidecar diffs, plus a per-request
    // get/read round-trip correlated by requestId (reusing PendingCallRegistry + the generic tool-cancel).

    /// <summary>
    /// One entry in <see cref="PromptManifestMessage.Prompts"/> — shaped to fill <c>IRunPrompt</c> 1:1
    /// (§A.1). <c>inputSchema</c> is a JSON Schema the manager flattens to the prompt's arguments[];
    /// <c>role</c> is the message author role (<c>"user"</c> | <c>"assistant"</c>).
    /// </summary>
    public sealed class PromptDescriptor
    {
        [JsonPropertyName("name")] public string Name { get; set; } = string.Empty;
        [JsonPropertyName("title")] public string? Title { get; set; }
        [JsonPropertyName("description")] public string? Description { get; set; }
        /// <summary>The prompt message author role — <c>"user"</c> or <c>"assistant"</c> (§A.1).</summary>
        [JsonPropertyName("role")] public string? Role { get; set; }
        [JsonPropertyName("inputSchema")] public JsonNode? InputSchema { get; set; }
        [JsonPropertyName("enabled")] public bool Enabled { get; set; } = true;
        [JsonPropertyName("extensionId")] public string? ExtensionId { get; set; }
        [JsonPropertyName("schemaHash")] public string? SchemaHash { get; set; }
    }

    /// <summary>plugin → sidecar: full prompt-set snapshot (v2, §A.1). The sidecar diffs it against the last applied.</summary>
    public sealed class PromptManifestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.PromptManifest;
        [JsonPropertyName("revision")] public int Revision { get; set; }
        [JsonPropertyName("prompts")] public List<PromptDescriptor> Prompts { get; set; } = new();
    }

    /// <summary>
    /// One entry in <see cref="ResourceManifestMessage.Resources"/> — shaped to fill <c>IRunResource</c>
    /// 1:1 (§A.1). <c>uri</c> is the resource's MCP URI (the manager's route); MVP resources are
    /// static fixed-URI (templated URIs are deferred — see §A.1).
    /// </summary>
    public sealed class ResourceDescriptor
    {
        [JsonPropertyName("uri")] public string Uri { get; set; } = string.Empty;
        [JsonPropertyName("name")] public string? Name { get; set; }
        [JsonPropertyName("description")] public string? Description { get; set; }
        [JsonPropertyName("mimeType")] public string? MimeType { get; set; }
        [JsonPropertyName("enabled")] public bool Enabled { get; set; } = true;
        [JsonPropertyName("extensionId")] public string? ExtensionId { get; set; }
        [JsonPropertyName("schemaHash")] public string? SchemaHash { get; set; }
    }

    /// <summary>plugin → sidecar: full resource-set snapshot (v2, §A.1). The sidecar diffs it against the last applied.</summary>
    public sealed class ResourceManifestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ResourceManifest;
        [JsonPropertyName("revision")] public int Revision { get; set; }
        [JsonPropertyName("resources")] public List<ResourceDescriptor> Resources { get; set; } = new();
    }

    /// <summary>sidecar → plugin: fetch one prompt's rendered messages (v2, §A.1). Round-trips like a tool-call.</summary>
    public sealed class PromptGetMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.PromptGet;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("prompt")] public string Prompt { get; set; } = string.Empty;
        [JsonPropertyName("arguments")] public JsonObject? Arguments { get; set; }
        [JsonPropertyName("timeoutMs")] public int TimeoutMs { get; set; } = IpcProtocol.DefaultToolTimeoutMs;
    }

    /// <summary>One rendered prompt message (≈ <c>ResponseGetPrompt.Messages[]</c>, §A.1): a role + content text.</summary>
    public sealed class PromptResponseEntry
    {
        [JsonPropertyName("role")] public string? Role { get; set; }
        [JsonPropertyName("text")] public string? Text { get; set; }
    }

    /// <summary>plugin → sidecar: terminal prompt-get result (v2, §A.1), correlated by requestId (≈ ResponseGetPrompt).</summary>
    public sealed class PromptResponseMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.PromptResponse;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("status")] public string Status { get; set; } = IpcProtocol.Status.Success;
        [JsonPropertyName("description")] public string? Description { get; set; }
        [JsonPropertyName("messages")] public List<PromptResponseEntry>? Messages { get; set; }
        /// <summary>A short human-readable reason on an <c>error</c> status (never a secret).</summary>
        [JsonPropertyName("error")] public string? Error { get; set; }
    }

    /// <summary>sidecar → plugin: read one resource's content by uri (v2, §A.1). Round-trips like a tool-call.</summary>
    public sealed class ResourceReadMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ResourceRead;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("uri")] public string Uri { get; set; } = string.Empty;
        [JsonPropertyName("timeoutMs")] public int TimeoutMs { get; set; } = IpcProtocol.DefaultToolTimeoutMs;
    }

    /// <summary>
    /// One resource content block (≈ <c>ResponseResourceContent</c>, §A.1): <c>text</c> XOR
    /// <c>blob</c> (base64) + <c>mimeType</c> — binary rides as base64 like screenshot images.
    /// </summary>
    public sealed class ResourceContentEntry
    {
        [JsonPropertyName("uri")] public string? Uri { get; set; }
        [JsonPropertyName("mimeType")] public string? MimeType { get; set; }
        [JsonPropertyName("text")] public string? Text { get; set; }
        /// <summary>Base64-encoded binary content (XOR with <see cref="Text"/>).</summary>
        [JsonPropertyName("blob")] public string? Blob { get; set; }
    }

    /// <summary>plugin → sidecar: terminal resource-read result (v2, §A.1), correlated by requestId.</summary>
    public sealed class ResourceResponseMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ResourceResponse;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("status")] public string Status { get; set; } = IpcProtocol.Status.Success;
        [JsonPropertyName("contents")] public List<ResourceContentEntry>? Contents { get; set; }
        /// <summary>A short human-readable reason on an <c>error</c> status (never a secret).</summary>
        [JsonPropertyName("error")] public string? Error { get; set; }
    }

    /// <summary>sidecar → plugin: cooperative cancellation of an in-flight call (§4).</summary>
    public sealed class ToolCancelMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.ToolCancel;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
    }

    /// <summary>Either direction: heartbeat probe / reply (§1.3).</summary>
    public sealed class HeartbeatMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.Ping;
    }

    /// <summary>Either direction: structured log forwarding for unified output (§1.3). Never carries a secret.</summary>
    public sealed class LogMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.Log;
        [JsonPropertyName("level")] public string? Level { get; set; }
        [JsonPropertyName("message")] public string? Message { get; set; }
    }

    /// <summary>
    /// sidecar → plugin: device-code authorization progress (§1.3 / §7 Cloud auth). Carries the verification
    /// URL + user code (for the UI to display/open) and a coarse <c>state</c> the view-model maps to its
    /// device-auth indicator. On success it carries the issued cloud bearer so the plugin can store it via the
    /// §8 config store. The token is part of the success payload but is NEVER written to a log line (§8).
    /// </summary>
    public sealed class DeviceAuthMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.DeviceAuth;
        /// <summary>One of <c>pending</c> / <c>authorized</c> / <c>failed</c> / <c>denied</c> / <c>expired</c>.</summary>
        [JsonPropertyName("state")] public string State { get; set; } = "pending";
        [JsonPropertyName("verificationUrl")] public string? VerificationUrl { get; set; }
        [JsonPropertyName("userCode")] public string? UserCode { get; set; }
        /// <summary>The issued cloud bearer — present only on the terminal <c>authorized</c> state.</summary>
        [JsonPropertyName("token")] public string? Token { get; set; }
        /// <summary>A short human-readable reason on a <c>failed</c> state (never a secret).</summary>
        [JsonPropertyName("message")] public string? Message { get; set; }
    }

    /// <summary>
    /// sidecar → plugin: live SignalR state for the §7 UI (§1.3 <c>status</c>). The plugin's view-model reads
    /// <c>connectionState</c> / <c>keepConnected</c> / <c>cloudAuthState</c> / <c>aiAgents</c>.
    /// </summary>
    public sealed class StatusMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.Status;
        /// <summary>One of <c>Connected</c> / <c>Connecting</c> / <c>Disconnected</c> / <c>Degraded</c>.</summary>
        [JsonPropertyName("connectionState")] public string ConnectionState { get; set; } = "Disconnected";
        [JsonPropertyName("keepConnected")] public bool KeepConnected { get; set; }
        /// <summary>One of <c>Authorized</c> / <c>Unauthorized</c> (Cloud mode only; null in Custom).</summary>
        [JsonPropertyName("cloudAuthState")] public string? CloudAuthState { get; set; }
        [JsonPropertyName("aiAgents")] public List<string> AiAgents { get; set; } = new();
    }
}
