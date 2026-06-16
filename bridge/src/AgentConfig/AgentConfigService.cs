/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Microsoft.Extensions.Logging;
using McpAuthOption = com.IvanMurzak.McpPlugin.Common.Consts.MCP.Server.AuthOption;
using McpConnectionMode = com.IvanMurzak.McpPlugin.AgentConfig.ConnectionMode;
using McpTransport = com.IvanMurzak.McpPlugin.Common.Consts.MCP.Server.TransportMethod;

namespace com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig
{
    /// <summary>
    /// The sidecar's AI-agent configuration service (docs/ARCHITECTURE.md §7). Serves the plugin's thin Slate
    /// panel by delegating EVERY configurator decision to the shared, engine-agnostic
    /// <c>com.IvanMurzak.McpPlugin.AgentConfig</c> library (MCP-Plugin-dotnet 6.8.0): it lists the available
    /// agents, describes one agent for a transport (the UI DTO), writes/removes the agent's MCP config file, and
    /// resolves the agent's skills folder. The plugin no longer carries any C++ configurator logic — this service
    /// IS the single implementation, shared with Unity/Godot through the same library.
    ///
    /// Stateless about connection config: each request carries the resolved facts inline (<see cref="AgentSettingsDto"/>),
    /// which this service maps through <c>AgentConfiguratorSettings.CreateForHost(...)</c> so the host OS is
    /// auto-detected (the library has runtime per-OS path detection). Pure (no IPC dependency) so the xUnit suite
    /// drives it directly with a hand-built request and asserts the produced result/DTO.
    ///
    /// Skill-file GENERATION stays plugin-side on purpose: the per-tool SKILL.md bodies are sourced from the C++
    /// tool registry (the live tool docs), which the sidecar does not hold; this service only resolves WHERE the
    /// files belong (<see cref="HandleSkillsPath"/>), mirroring the SidecarHost's <c>GenerateSkillFiles = false</c>.
    /// </summary>
    public sealed class AgentConfigService
    {
        private readonly ILogger? _logger;

        public AgentConfigService(ILogger? logger = null)
        {
            _logger = logger;
        }

        // --- Request handlers (one per plugin → sidecar request type). Each returns the terminal result the
        //     caller frames as an `agent-config-result` and sends back over IPC. None throw on bad input — a
        //     missing agent / settings becomes an `ok == false` result with a reason, never a torn read loop. ---

        /// <summary>Enumerate every available agent with its status for the requested transport.</summary>
        public AgentConfigResultMessage HandleList(AgentsListRequestMessage request)
        {
            var result = NewResult(request.RequestId, IpcProtocol.Type.AgentsList);
            if (request.Settings == null)
                return Fail(result, "No settings provided for agents-list.");

            var settings = MapSettings(request.Settings);
            var transport = ParseTransport(request.Transport);

            result.Agents = AiAgentConfiguratorRegistry.All
                .Select(c => Describe(c, settings, transport))
                .ToList();
            result.Ok = true;
            return result;
        }

        /// <summary>Describe one agent for the requested transport (the per-agent UI DTO).</summary>
        public AgentConfigResultMessage HandleStatus(AgentStatusRequestMessage request)
        {
            var result = NewResult(request.RequestId, IpcProtocol.Type.AgentStatus);
            if (request.Settings == null)
                return Fail(result, "No settings provided for agent-status.");

            var configurator = AiAgentConfiguratorRegistry.GetByAgentId(request.AgentId);
            if (configurator == null)
                return Fail(result, $"Unknown agent id '{request.AgentId}'.");

            var settings = MapSettings(request.Settings);
            var transport = ParseTransport(request.Transport);
            result.Description = Describe(configurator, settings, transport);
            result.Ok = true;
            return result;
        }

        /// <summary>Write/merge the MCP entry into the agent's config file for the requested transport.</summary>
        public AgentConfigResultMessage HandleConfigure(AgentConfigureRequestMessage request)
        {
            var result = NewResult(request.RequestId, IpcProtocol.Type.AgentConfigure);
            if (request.Settings == null)
                return Fail(result, "No settings provided for agent-configure.");

            var configurator = AiAgentConfiguratorRegistry.GetByAgentId(request.AgentId);
            if (configurator == null)
                return Fail(result, $"Unknown agent id '{request.AgentId}'.");

            // The Custom agent's editable config path arrives as the EditableField write-back. Apply it before the
            // configure so the (no-op for Custom) write + the refreshed description reflect the edited value.
            ApplyEditableValue(configurator, request.EditableValue);

            var settings = MapSettings(request.Settings);
            var transport = ParseTransport(request.Transport);
            try
            {
                var config = transport == McpTransport.stdio
                    ? configurator.GetStdioConfig(settings, _logger)
                    : configurator.GetHttpConfig(settings, _logger);
                result.Ok = config.Configure();
                if (!result.Ok)
                    result.Error = $"Configure returned false for '{request.AgentId}' ({transport}).";
            }
            catch (NotImplementedException)
            {
                // The Custom agent has no auto-detectable config — there is nothing to write; the user copies the
                // snippet from the description. Surface that as a clear (non-fatal) reason, not a crash.
                return Fail(result, $"Agent '{request.AgentId}' has no writable config file (copy the snippet instead).");
            }
            catch (Exception ex)
            {
                return Fail(result, $"Configure failed for '{request.AgentId}': {ex.Message}");
            }

            // Return the refreshed description so the panel updates without a follow-up status round-trip.
            result.Description = Describe(configurator, settings, transport);
            return result;
        }

        /// <summary>Remove the MCP entry from BOTH transports' config for the agent (a single Remove clears it).</summary>
        public AgentConfigResultMessage HandleRemove(AgentRemoveRequestMessage request)
        {
            var result = NewResult(request.RequestId, IpcProtocol.Type.AgentRemove);
            if (request.Settings == null)
                return Fail(result, "No settings provided for agent-remove.");

            var configurator = AiAgentConfiguratorRegistry.GetByAgentId(request.AgentId);
            if (configurator == null)
                return Fail(result, $"Unknown agent id '{request.AgentId}'.");

            var settings = MapSettings(request.Settings);
            var anyRemoved = false;
            foreach (var transport in new[] { McpTransport.stdio, McpTransport.streamableHttp })
            {
                try
                {
                    var config = transport == McpTransport.stdio
                        ? configurator.GetStdioConfig(settings, _logger)
                        : configurator.GetHttpConfig(settings, _logger);
                    anyRemoved |= config.Unconfigure();
                }
                catch (NotImplementedException)
                {
                    // Custom agent — nothing to remove on disk; not an error.
                }
                catch (Exception ex)
                {
                    _logger?.LogDebug("Unconfigure({Transport}) for '{Agent}' failed: {Message}", transport, request.AgentId, ex.Message);
                }
            }

            result.Ok = true; // remove is idempotent — "nothing to remove" is still success
            if (!anyRemoved)
                result.Error = null;
            // Refreshed description for the requested transport so the panel re-renders the now-removed state.
            result.Description = Describe(configurator, settings, ParseTransport(request.Transport));
            return result;
        }

        /// <summary>
        /// Resolve the agent's skills folder to an absolute path (the plugin then writes the per-tool SKILL.md
        /// files there, sourcing the bodies from its C++ tool registry — the sidecar holds no tool docs).
        /// </summary>
        public AgentConfigResultMessage HandleSkillsPath(AgentSkillsPathRequestMessage request)
        {
            var result = NewResult(request.RequestId, IpcProtocol.Type.AgentSkillsPath);

            var configurator = AiAgentConfiguratorRegistry.GetByAgentId(request.AgentId);
            if (configurator == null)
                return Fail(result, $"Unknown agent id '{request.AgentId}'.");

            ApplyEditableValue(configurator, request.CustomSkillsPath);

            var folder = configurator.SkillsPath;
            if (string.IsNullOrEmpty(folder))
                return Fail(result, $"Agent '{request.AgentId}' does not support skills.");

            var projectRoot = request.Settings?.ProjectRootPath ?? string.Empty;
            result.SkillsPath = ResolveAbsolute(projectRoot, folder!);
            result.Ok = true;
            return result;
        }

        // --- Mapping helpers --------------------------------------------------------------------------

        /// <summary>Build the engine-agnostic per-agent UI DTO from the shared library's description + configurator.</summary>
        private static AgentConfiguratorDescriptionDto Describe(
            AiAgentConfigurator configurator, AgentConfiguratorSettings settings, McpTransport transport)
        {
            var description = configurator.Describe(settings, transport);
            return new AgentConfiguratorDescriptionDto
            {
                AgentName = description.AgentName,
                AgentId = description.AgentId,
                IconName = description.IconName,
                IsConfigured = description.IsConfigured,
                IsInstalled = description.IsInstalled,
                SupportsSkills = configurator.SupportsSkills,
                DownloadUrl = configurator.DownloadUrl,
                TutorialUrl = configurator.TutorialUrl,
                Sections = description.Sections
                    .Select(s => new AgentSectionDto
                    {
                        Heading = s.Heading,
                        ExpandedFirst = s.ExpandedFirst,
                        Items = s.Items
                            .Select(i => new AgentItemDto { Kind = i.Kind.ToString(), Text = i.Text })
                            .ToList(),
                    })
                    .ToList(),
            };
        }

        /// <summary>Map the inline IPC settings DTO into the shared library's host-aware settings.</summary>
        private static AgentConfiguratorSettings MapSettings(AgentSettingsDto dto)
        {
            var connectionMode = string.Equals(dto.ConnectionMode, "Cloud", StringComparison.OrdinalIgnoreCase)
                ? McpConnectionMode.Cloud
                : McpConnectionMode.Local;
            var authOption = dto.AuthRequired ? McpAuthOption.required : McpAuthOption.none;
            return AgentConfiguratorSettings.CreateForHost(
                projectRootPath: dto.ProjectRootPath,
                executableFullPath: dto.ExecutableFullPath,
                port: dto.Port,
                timeoutMs: dto.TimeoutMs,
                host: dto.Host,
                token: string.IsNullOrEmpty(dto.Token) ? null : dto.Token,
                connectionMode: connectionMode,
                authOption: authOption);
        }

        private static McpTransport ParseTransport(string? transport) =>
            string.Equals(transport, "stdio", StringComparison.OrdinalIgnoreCase)
                ? McpTransport.stdio
                : McpTransport.streamableHttp;

        /// <summary>Push the Custom agent's edited path into its configurator (no-op for built-in agents).</summary>
        private static void ApplyEditableValue(AiAgentConfigurator configurator, string? editableValue)
        {
            if (!string.IsNullOrEmpty(editableValue) && configurator is global::com.IvanMurzak.McpPlugin.AgentConfig.Impl.CustomConfigurator custom)
                custom.EditableSkillsPath = editableValue!;
        }

        /// <summary>
        /// Resolve a (possibly project-relative) folder to an absolute path with forward slashes. Mirrors the
        /// shared library's <c>AiAgentConfigurator.ResolveAbsoluteSkillsPath</c> (which is protected, so the logic
        /// is duplicated here) so the plugin gets the same location it would have computed locally.
        /// </summary>
        internal static string ResolveAbsolute(string projectRootPath, string folder)
        {
            if (string.IsNullOrEmpty(folder))
                return folder;
            var abs = Path.IsPathRooted(folder)
                ? folder
                : Path.GetFullPath(Path.Combine(projectRootPath ?? string.Empty, folder));
            return abs.Replace('\\', '/');
        }

        private static AgentConfigResultMessage NewResult(string requestId, string op) =>
            new() { RequestId = requestId, Op = op, Ok = false };

        private AgentConfigResultMessage Fail(AgentConfigResultMessage result, string reason)
        {
            result.Ok = false;
            result.Error = reason;
            _logger?.LogDebug("agent-config {Op} request {RequestId} failed: {Reason}", result.Op, result.RequestId, reason);
            return result;
        }
    }
}
