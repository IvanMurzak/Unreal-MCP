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
using System.Text.Json.Serialization;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Ipc
{
    /// <summary>
    /// IPC message payloads for the §7 AI-agent configurators (docs/ARCHITECTURE.md §1.3 / §7). The plugin's
    /// thin Slate panel sends a request (<c>agents-list</c> / <c>agent-status</c> / <c>agent-configure</c> /
    /// <c>agent-remove</c> / <c>agent-skills-path</c>); the sidecar serves it against the shared
    /// <c>com.IvanMurzak.McpPlugin.AgentConfig</c> library and answers with an <see cref="AgentConfigResultMessage"/>
    /// carrying the engine-agnostic <see cref="AgentConfiguratorDescriptionDto"/>. The plugin never re-implements
    /// the configurator logic — it parses these DTOs into its existing Slate rich-content structs and renders them.
    ///
    /// The connection facts the configurators need (server path / port / host / token / mode / auth) travel INLINE
    /// in <see cref="AgentSettingsDto"/> on every request: the plugin already resolves them (§8), so the sidecar
    /// service stays stateless about connection config and just maps the DTO into an
    /// <c>AgentConfiguratorSettings.CreateForHost(...)</c>.
    /// </summary>

    /// <summary>
    /// The connection facts an agent configurator needs, resolved plugin-side and forwarded verbatim. Mirrors the
    /// shared library's <c>AgentConfiguratorSettings</c> constructor args; the sidecar maps it through
    /// <c>AgentConfiguratorSettings.CreateForHost(...)</c> so the host OS is auto-detected.
    /// </summary>
    public sealed class AgentSettingsDto
    {
        /// <summary>Absolute path to the consuming project's root (where project-local config files live).</summary>
        [JsonPropertyName("projectRootPath")] public string ProjectRootPath { get; set; } = string.Empty;
        /// <summary>Absolute path to the local MCP server executable (stdio transport <c>command</c>); may be empty.</summary>
        [JsonPropertyName("executableFullPath")] public string ExecutableFullPath { get; set; } = string.Empty;
        /// <summary>The MCP server port (stdio transport arg).</summary>
        [JsonPropertyName("port")] public int Port { get; set; }
        /// <summary>Plugin timeout in milliseconds (stdio transport arg).</summary>
        [JsonPropertyName("timeoutMs")] public int TimeoutMs { get; set; } = IpcProtocol.DefaultToolTimeoutMs;
        /// <summary>The streamableHttp server URL (http transport <c>url</c>).</summary>
        [JsonPropertyName("host")] public string Host { get; set; } = string.Empty;
        /// <summary>
        /// The access token for the "Advanced: use access token" escape hatch (Flow C), or empty/null otherwise.
        /// NEVER logged (§8). No longer required input on the default path (mcp-authorize PR 5): with the device-flow
        /// machine credential store (PR 2) the default config is credential-free (native MCP OAuth), so this is
        /// carried ONLY when <see cref="UseAccessToken"/> is set.
        /// </summary>
        [JsonPropertyName("token")] public string? Token { get; set; }
        /// <summary>One of <c>Local</c> / <c>Cloud</c> (case-insensitive). Cloud always requires auth.</summary>
        [JsonPropertyName("connectionMode")] public string ConnectionMode { get; set; } = "Local";
        /// <summary>Whether bearer-token auth is required (independent of cloud enforcement).</summary>
        [JsonPropertyName("authRequired")] public bool AuthRequired { get; set; }
        /// <summary>
        /// The "Advanced: use access token" escape hatch opt-in (mcp-authorize PR 5, design 06, Flow C). Default
        /// <c>false</c> → the credential-free native-OAuth path (D11): HTTP configs carry <c>type</c> + <c>url</c>
        /// only, no embedded bearer. When <c>true</c> AND a non-empty <see cref="Token"/> is supplied, the HTTP
        /// config is written with the legacy Bearer shape (<c>HttpCredentialMode.AccessToken</c>) for the few
        /// clients that cannot do MCP OAuth (<see cref="AgentConfiguratorDescriptionDto.SupportsOAuth"/> == false).
        /// </summary>
        [JsonPropertyName("useAccessToken")] public bool UseAccessToken { get; set; }
    }

    /// <summary>plugin → sidecar: enumerate the available agents and (optionally) their status for a transport.</summary>
    public sealed class AgentsListRequestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.AgentsList;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        /// <summary>The transport the status (IsConfigured) should be computed for: <c>stdio</c> / <c>streamableHttp</c>.</summary>
        [JsonPropertyName("transport")] public string Transport { get; set; } = "streamableHttp";
        [JsonPropertyName("settings")] public AgentSettingsDto? Settings { get; set; }
    }

    /// <summary>plugin → sidecar: describe ONE agent for a transport (the per-agent UI DTO).</summary>
    public sealed class AgentStatusRequestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.AgentStatus;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("agentId")] public string AgentId { get; set; } = string.Empty;
        [JsonPropertyName("transport")] public string Transport { get; set; } = "streamableHttp";
        [JsonPropertyName("settings")] public AgentSettingsDto? Settings { get; set; }
    }

    /// <summary>plugin → sidecar: write/merge the MCP entry into the agent's config file for a transport.</summary>
    public sealed class AgentConfigureRequestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.AgentConfigure;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("agentId")] public string AgentId { get; set; } = string.Empty;
        [JsonPropertyName("transport")] public string Transport { get; set; } = "streamableHttp";
        /// <summary>For the Custom agent: the edited skills/config path written back over the EditableField (optional).</summary>
        [JsonPropertyName("editableValue")] public string? EditableValue { get; set; }
        [JsonPropertyName("settings")] public AgentSettingsDto? Settings { get; set; }
    }

    /// <summary>plugin → sidecar: remove the MCP entry (both transports) from the agent's config file.</summary>
    public sealed class AgentRemoveRequestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.AgentRemove;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("agentId")] public string AgentId { get; set; } = string.Empty;
        [JsonPropertyName("transport")] public string Transport { get; set; } = "streamableHttp";
        [JsonPropertyName("settings")] public AgentSettingsDto? Settings { get; set; }
    }

    /// <summary>
    /// plugin → sidecar: resolve the agent's skills folder (absolute). A read-only query used by the panel to show
    /// where SKILL.md files would land; the WRITE is <see cref="AgentGenerateSkillsRequestMessage"/>. The shared
    /// library owns the per-agent skills path; the sidecar resolves it to an absolute folder.
    /// </summary>
    public sealed class AgentSkillsPathRequestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.AgentSkillsPath;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("agentId")] public string AgentId { get; set; } = string.Empty;
        /// <summary>For the Custom agent: the user-edited (project-relative or absolute) skills path to resolve.</summary>
        [JsonPropertyName("customSkillsPath")] public string? CustomSkillsPath { get; set; }
        [JsonPropertyName("settings")] public AgentSettingsDto? Settings { get; set; }
    }

    /// <summary>
    /// plugin → sidecar: GENERATE the per-tool SKILL.md files for an agent (§7, issue #101). The sidecar resolves
    /// the agent's skills folder, then writes one <c>&lt;skillsRoot&gt;/&lt;tool&gt;/SKILL.md</c> per tool, sourced
    /// from the tool manifest the plugin already pushed over IPC (the ProxyTool catalog) — no C++ generation logic.
    /// </summary>
    public sealed class AgentGenerateSkillsRequestMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.AgentGenerateSkills;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        [JsonPropertyName("agentId")] public string AgentId { get; set; } = string.Empty;
        /// <summary>For the Custom agent: the user-edited (project-relative or absolute) skills path to write under.</summary>
        [JsonPropertyName("customSkillsPath")] public string? CustomSkillsPath { get; set; }
        [JsonPropertyName("settings")] public AgentSettingsDto? Settings { get; set; }
    }

    /// <summary>One UI item inside an <see cref="AgentSectionDto"/> (or a top-level <see cref="AgentConfiguratorDescriptionDto.Links"/>
    /// entry) — engine-agnostic, no UI handle.</summary>
    public sealed class AgentItemDto
    {
        /// <summary>One of <c>Description</c> / <c>Warning</c> / <c>Alert</c> / <c>ReadOnlyField</c> / <c>EditableField</c> / <c>Link</c>.</summary>
        [JsonPropertyName("kind")] public string Kind { get; set; } = "Description";
        [JsonPropertyName("text")] public string Text { get; set; } = string.Empty;
        /// <summary>The open-URL target for a <c>Link</c>-kind item (e.g. download / tutorial); empty for non-link kinds.</summary>
        [JsonPropertyName("url")] public string Url { get; set; } = string.Empty;
    }

    /// <summary>One collapsible UI section the plugin renders as a foldout.</summary>
    public sealed class AgentSectionDto
    {
        [JsonPropertyName("heading")] public string Heading { get; set; } = string.Empty;
        [JsonPropertyName("expandedFirst")] public bool ExpandedFirst { get; set; }
        [JsonPropertyName("items")] public List<AgentItemDto> Items { get; set; } = new();
    }

    /// <summary>
    /// The engine-agnostic description of one configurator for one transport — the 1:1 IPC mirror of the shared
    /// library's <c>AgentConfiguratorDescription</c>. The plugin maps this into its existing Slate rich-content
    /// structs (icon NAME resolved to a brush plugin-side; the sidecar never carries icon bytes).
    /// </summary>
    public sealed class AgentConfiguratorDescriptionDto
    {
        [JsonPropertyName("agentName")] public string AgentName { get; set; } = string.Empty;
        [JsonPropertyName("agentId")] public string AgentId { get; set; } = string.Empty;
        /// <summary>Icon file NAME only (e.g. <c>claude-64.png</c>), never the bytes; null when no icon.</summary>
        [JsonPropertyName("iconName")] public string? IconName { get; set; }
        [JsonPropertyName("isConfigured")] public bool IsConfigured { get; set; }
        /// <summary>
        /// The tri-state configuration status (6.9.0): <c>NotConfigured</c> / <c>Configured</c> / <c>ReconfigureNeeded</c>.
        /// <c>ReconfigureNeeded</c> means a config entry exists but its connection settings are stale — the library
        /// also prepends a "Reconfiguration Required" Alert section to <see cref="Sections"/> in that case. The plugin
        /// uses it to label the action button (Configure / Reconfigure). Sent as the enum NAME (verbatim).
        /// </summary>
        [JsonPropertyName("status")] public string Status { get; set; } = "NotConfigured";
        [JsonPropertyName("isInstalled")] public bool IsInstalled { get; set; }
        [JsonPropertyName("supportsSkills")] public bool SupportsSkills { get; set; }
        /// <summary>
        /// Whether this agent can complete native MCP OAuth against the server URL (mcp-authorize PR 5, design 06).
        /// Default <c>true</c> — the plugin writes the credential-free config and the client's own authorize flow
        /// completes the loop. A <c>false</c> value is the signal for the editor UI to offer the "Advanced: use
        /// access token" (PAT) escape hatch (Flow C) for that agent. Forwarded verbatim from the shared
        /// configurator's <c>SupportsOAuth</c>.
        /// </summary>
        [JsonPropertyName("supportsOAuth")] public bool SupportsOAuth { get; set; } = true;
        [JsonPropertyName("downloadUrl")] public string? DownloadUrl { get; set; }
        [JsonPropertyName("tutorialUrl")] public string? TutorialUrl { get; set; }
        /// <summary>
        /// The top-level clickable links for this agent (6.9.0) — <c>Link</c>-kind items carrying a label
        /// (<see cref="AgentItemDto.Text"/>) and an <see cref="AgentItemDto.Url"/> (download / tutorial / docs). The
        /// plugin renders each as a hyperlink that opens the URL. Supersedes the single
        /// <see cref="DownloadUrl"/>/<see cref="TutorialUrl"/> pair (kept for back-compat); prefer this collection.
        /// </summary>
        [JsonPropertyName("links")] public List<AgentItemDto> Links { get; set; } = new();
        [JsonPropertyName("sections")] public List<AgentSectionDto> Sections { get; set; } = new();
    }

    /// <summary>
    /// sidecar → plugin: the terminal result of one agent-config request (§7). Correlated by <see cref="RequestId"/>.
    /// <see cref="Op"/> echoes the originating request type. On a list request <see cref="Agents"/> is populated; on a
    /// single-agent request <see cref="Description"/> is populated; on configure/remove both the (post-write) refreshed
    /// <see cref="Description"/> is returned so the panel updates without a follow-up round-trip. <see cref="SkillsPath"/>
    /// carries the resolved absolute folder for a skills-path request.
    /// </summary>
    public sealed class AgentConfigResultMessage
    {
        [JsonPropertyName("type")] public string Type { get; set; } = IpcProtocol.Type.AgentConfigResult;
        [JsonPropertyName("requestId")] public string RequestId { get; set; } = string.Empty;
        /// <summary>The originating request type (e.g. <c>agent-configure</c>) so the plugin routes the result.</summary>
        [JsonPropertyName("op")] public string Op { get; set; } = string.Empty;
        [JsonPropertyName("ok")] public bool Ok { get; set; }
        /// <summary>A short human-readable failure reason on <c>ok == false</c> (never a secret).</summary>
        [JsonPropertyName("error")] public string? Error { get; set; }
        /// <summary>The agent set (list request only).</summary>
        [JsonPropertyName("agents")] public List<AgentConfiguratorDescriptionDto>? Agents { get; set; }
        /// <summary>The single agent's description (status / configure / remove requests).</summary>
        [JsonPropertyName("description")] public AgentConfiguratorDescriptionDto? Description { get; set; }
        /// <summary>The resolved absolute skills folder (skills-path AND generate-skills requests).</summary>
        [JsonPropertyName("skillsPath")] public string? SkillsPath { get; set; }
        /// <summary>Count of SKILL.md files written (generate-skills request only).</summary>
        [JsonPropertyName("filesWritten")] public int? FilesWritten { get; set; }
        /// <summary>Count of stale generator-owned folders pruned (generate-skills request only).</summary>
        [JsonPropertyName("filesPruned")] public int? FilesPruned { get; set; }
    }
}
