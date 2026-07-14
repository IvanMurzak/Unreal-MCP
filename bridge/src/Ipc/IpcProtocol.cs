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
        ///
        /// <para>
        /// <b>v2</b> (M16 P0) adds the prompt/resource message families — the
        /// <c>prompt-manifest</c>/<c>resource-manifest</c>, <c>prompt-get</c>/<c>resource-read</c>, and
        /// <c>prompt-response</c>/<c>resource-response</c> discriminators. The version is NEGOTIATED on
        /// the handshake (<see cref="NegotiateVersion"/>): two v2 peers exchange prompts/resources; a v1
        /// peer paired with a v2 peer negotiates down to v1 and the link stays TOOLS-ONLY (the v2 peer
        /// simply never pushes/serves the prompt/resource families). The tool path is byte-identical
        /// across both versions, so a version mismatch never breaks tools.
        /// </para>
        /// </summary>
        public const int IpcVersion = 2;

        /// <summary>
        /// The lowest IPC version at which the prompt/resource message families (v2) are exchanged. When
        /// the negotiated version (<see cref="NegotiateVersion"/>) is below this, the link stays tools-only.
        /// </summary>
        public const int PromptsResourcesMinVersion = 2;

        /// <summary>
        /// Negotiate the effective IPC version for a link from the two peers' advertised versions: the
        /// MINIMUM of the local and remote versions (the highest both understand). A peer must not use a
        /// message family the negotiated version does not cover, so a v2↔v1 pair negotiates to 1 and stays
        /// tools-only, while a v2↔v2 pair negotiates to 2 and may exchange prompts/resources. Pure +
        /// static so the negotiation matrix is unit-testable without a live socket. A non-positive or
        /// absent remote version (a malformed/legacy handshake that omitted the field) is treated as 1.
        /// </summary>
        public static int NegotiateVersion(int localVersion, int remoteVersion)
        {
            var remote = remoteVersion > 0 ? remoteVersion : 1;
            var local = localVersion > 0 ? localVersion : 1;
            return local < remote ? local : remote;
        }

        /// <summary>
        /// Whether a link whose negotiated IPC version is <paramref name="negotiatedVersion"/> exchanges
        /// the v2 prompt/resource message families. False on a tools-only (v1-negotiated) link.
        /// </summary>
        public static bool SupportsPromptsResources(int negotiatedVersion) =>
            negotiatedVersion >= PromptsResourcesMinVersion;

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
            // sidecar → plugin: the terminal result of one agent-config request (§7 AI-agent configurators).
            // Correlated to the originating request by requestId; carries the engine-agnostic DTO.
            public const string AgentConfigResult = "agent-config-result";
            // sidecar → plugin: the resolved connection identity for the current project (mcp-authorize PR 4,
            // design 04/06) — {pin, derived local-server port, portIsOverridden, serverTarget}. The terminal
            // answer to a plugin `project-config` request, correlated by requestId.
            public const string ProjectConfigResult = "project-config-result";

            // sidecar → plugin: fetch one prompt's rendered messages / read one resource's content (v2, §A.1).
            // Both round-trip through PendingCallRegistry by requestId, exactly like a tool-call, and reuse the
            // generic `tool-cancel` (by requestId) for cooperative cancellation — no new cancel verbs.
            public const string PromptGet = "prompt-get";
            public const string ResourceRead = "resource-read";

            // plugin → sidecar
            public const string HandshakeAck = "handshake-ack";
            public const string ToolManifest = "tool-manifest";
            public const string ToolResponse = "tool-response";
            // plugin → sidecar: full prompt/resource-set snapshots (v2, §A.1) — diffed against the last applied
            // by the prompt/resource manifest registrars, mirroring `tool-manifest`. Only exchanged when the
            // negotiated IPC version is >= PromptsResourcesMinVersion (a tools-only link never sees these).
            public const string PromptManifest = "prompt-manifest";
            public const string ResourceManifest = "resource-manifest";
            // plugin → sidecar: terminal prompt-get / resource-read results (v2, §A.1), correlated by requestId.
            public const string PromptResponse = "prompt-response";
            public const string ResourceResponse = "resource-response";
            public const string Config = "config";
            public const string AuthStart = "auth-start";
            public const string AuthCancel = "auth-cancel";
            public const string AuthRevoke = "auth-revoke";
            public const string Shutdown = "shutdown";
            // plugin → sidecar: AI-agent configurator requests (§7). The sidecar serves these against the
            // shared com.IvanMurzak.McpPlugin.AgentConfig library and answers with an `agent-config-result`.
            public const string AgentsList = "agents-list";       // enumerate the available agents (+ status)
            public const string AgentStatus = "agent-status";     // describe ONE agent for a transport (the UI DTO)
            public const string AgentConfigure = "agent-configure"; // write/merge the MCP entry into the agent's config file
            public const string AgentRemove = "agent-remove";     // remove the MCP entry (both transports)
            public const string AgentSkillsPath = "agent-skills-path"; // resolve the agent's skills folder (plugin writes the files)
            public const string AgentGenerateSkills = "agent-generate-skills"; // sidecar resolves the path + writes the SKILL.md files
            // plugin → sidecar: request THIS project's resolved connection identity (mcp-authorize PR 4). The
            // sidecar computes {pin, derived local-server port, serverTarget} from McpPlugin's ProjectIdentity +
            // the project marker and answers with a `project-config-result`. Carries the project root to resolve from.
            public const string ProjectConfig = "project-config";

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
