/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Text.Json;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Ipc
{
    /// <summary>
    /// IPC protocol constants for the plugin ⇄ sidecar channel (docs/ARCHITECTURE.md §1.3).
    /// Every message is a UTF-8 JSON object with a <c>type</c> field, one message per
    /// <c>\n</c>-terminated line (NDJSON, §1.2).
    /// </summary>
    public static class IpcProtocol
    {
        /// <summary>
        /// IPC wire-protocol version (§9.2). Bumped only on a breaking framing/schema change; the
        /// handshake (§1.3) carries it so a future version can negotiate without breaking old pairs.
        /// </summary>
        public const int IpcVersion = 1;

        /// <summary>Max NDJSON line length (§1.2). A peer aborts the connection on violation.</summary>
        public const int MaxLineBytes = 64 * 1024 * 1024;

        /// <summary>Heartbeat cadence (§1.3): ping/pong every 5 s.</summary>
        public const int HeartbeatIntervalMs = 5000;

        /// <summary>A peer is declared dead after this much silence (§1.3).</summary>
        public const int HeartbeatTimeoutMs = 15000;

        /// <summary>Default per-tool-call timeout when the message omits one (§4).</summary>
        public const int DefaultToolTimeoutMs = 30000;

        /// <summary>
        /// Grace added to a tool-call's <c>timeoutMs</c> for the sidecar-side local backstop. The plugin
        /// honours <c>timeoutMs</c> and should answer with a terminal tool-response first; the local
        /// deadline is slightly longer so that response normally wins, and only fires when no response ever
        /// arrives (e.g. a call silently dropped pre-handshake) so the pending call cannot hang forever.
        /// </summary>
        public const int CallTimeoutGraceMs = 5000;

        /// <summary>Message <c>type</c> discriminator values (§1.3).</summary>
        public static class Type
        {
            // sidecar → plugin
            public const string Handshake = "handshake";
            public const string ToolCall = "tool-call";
            public const string ToolCancel = "tool-cancel";
            public const string Status = "status";
            public const string DeviceAuth = "device-auth";

            // plugin → sidecar
            public const string HandshakeAck = "handshake-ack";
            public const string ToolManifest = "tool-manifest";
            public const string ToolResponse = "tool-response";
            public const string Config = "config";
            public const string AuthStart = "auth-start";
            public const string AuthCancel = "auth-cancel";
            public const string AuthRevoke = "auth-revoke";
            public const string Shutdown = "shutdown";

            // either direction
            public const string Ping = "ping";
            public const string Pong = "pong";
            public const string Log = "log";
        }

        /// <summary>Terminal tool-response statuses (§1.3 — IPC status enum is terminal-only).</summary>
        public static class Status
        {
            public const string Success = "success";
            public const string Error = "error";
        }

        /// <summary>
        /// Shared <see cref="JsonSerializerOptions"/> for IPC: camelCase, no indentation (NDJSON is
        /// one compact line per message), and tolerant property-name matching on read.
        /// </summary>
        public static readonly JsonSerializerOptions JsonOptions = new()
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            PropertyNameCaseInsensitive = true,
            WriteIndented = false,
            DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull,
        };
    }
}
