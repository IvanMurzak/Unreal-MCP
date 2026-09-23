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
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Microsoft.Extensions.Logging;
using CustomConfigurator = com.IvanMurzak.McpPlugin.AgentConfig.Impl.CustomConfigurator;
using McpAuthOption = com.IvanMurzak.McpPlugin.Common.Consts.MCP.Server.AuthOption;
using McpConnectionMode = com.IvanMurzak.McpPlugin.AgentConfig.ConnectionMode;
using McpHttpCredentialMode = com.IvanMurzak.McpPlugin.AgentConfig.HttpCredentialMode;
using McpTransport = com.IvanMurzak.McpPlugin.Common.Consts.MCP.Server.TransportMethod;

namespace com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig
{
    /// <summary>
    /// The sidecar's AI-agent configuration service (docs/ARCHITECTURE.md §7). Serves the plugin's thin Slate
    /// panel by delegating EVERY configurator decision to the shared, engine-agnostic
    /// <c>com.IvanMurzak.McpPlugin.AgentConfig</c> library (MCP-Plugin-dotnet 6.9.0): it lists the available
    /// agents, describes one agent for a transport (the UI DTO), writes/removes the agent's MCP config file, and
    /// resolves the agent's skills folder. The plugin no longer carries any C++ configurator logic — this service
    /// IS the single implementation, shared with Unity/Godot through the same library.
    ///
    /// Stateless about connection config: each request carries the resolved facts inline (<see cref="AgentSettingsDto"/>),
    /// which this service maps through <c>AgentConfiguratorSettings.CreateForHost(...)</c> so the host OS is
    /// auto-detected (the library has runtime per-OS path detection). Pure (no IPC dependency) so the xUnit suite
    /// drives it directly with a hand-built request and asserts the produced result/DTO.
    ///
    /// Skill-file GENERATION also lives sidecar-side (<see cref="HandleGenerateSkills"/>): the per-tool SKILL.md
    /// bodies are sourced from the tool manifest the C++ plugin already pushes over IPC (the ProxyTool catalog —
    /// Name/Title/Description/hints/input+output schema), so the sidecar has the source data without any new
    /// C++→sidecar payload. <see cref="SkillFileGenerator"/> does the writing; this service resolves the path.
    /// (Unrelated to the SignalR host's <c>GenerateSkillFiles = false</c>, which governs the .NET host's own
    /// auto-generation of skills for ITS static tools — the sidecar has none.)
    ///
    /// <para><b>Project keys</b> (project-keys contract §7): in Cloud mode an HTTP Configure writes
    /// <c>Authorization: Bearer agd_pk_…</c> for EVERY agent — a non-expiring key bound to this project's pin,
    /// obtained from <see cref="ProjectKeyProvider.GetOrMintAsync"/> (reused from the cache or minted with the
    /// sidecar's machine credential). Not signed in, or the mint fails (e.g. the server's 404 while the feature is
    /// off) ⇒ the URL-only OAuth config, never an error. Status/list requests attach only the CACHED key (no
    /// network), so the status they report compares against what Configure writes. <see cref="HandleRegenerateKeyAsync"/>
    /// mints a fresh key and rewrites every agent configured for this project. The get-or-mint may block on the
    /// cross-process cache lock, which is why it only runs on the async (off-reader-thread) handlers.</para>
    /// </summary>
    public sealed class AgentConfigService
    {
        /// <summary>The engine id recorded on a minted project key (contract §2).</summary>
        public const string ProjectKeyEngine = "unreal";

        private readonly ILogger? _logger;
        private readonly SkillFileGenerator _skillGenerator;
        private readonly Func<ProjectKeyProvider?>? _projectKeys;

        /// <param name="logger">Optional diagnostics sink (never receives a secret).</param>
        /// <param name="projectKeys">Returns the project-key provider for the signed-in machine account, or
        /// <c>null</c> when this machine is not signed in (Cloud configs then stay URL-only).</param>
        public AgentConfigService(ILogger? logger = null, Func<ProjectKeyProvider?>? projectKeys = null)
        {
            _logger = logger;
            _skillGenerator = new SkillFileGenerator(logger);
            _projectKeys = projectKeys;
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

            var transport = ParseTransport(request.Transport);
            var settings = AttachCachedProjectKey(MapSettings(request.Settings), transport, result);

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

            var transport = ParseTransport(request.Transport);
            var settings = AttachCachedProjectKey(MapSettings(request.Settings), transport, result);
            result.Description = Describe(configurator, settings, transport);
            result.Ok = true;
            return result;
        }

        /// <summary>
        /// Write/merge the MCP entry into the agent's config file for the requested transport. In Cloud mode the
        /// HTTP entry carries the project key (get-or-mint; URL-only when none can be obtained).
        /// </summary>
        public async Task<AgentConfigResultMessage> HandleConfigureAsync(AgentConfigureRequestMessage request, CancellationToken cancellationToken = default)
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

            var transport = ParseTransport(request.Transport);
            // The Custom agent has no file to write, so it never needs a freshly minted key — the cached one (if any)
            // is enough to render its snippet with the header present.
            // The Custom agent has no file to write, so it never needs a freshly minted key.
            var settings = configurator is CustomConfigurator
                ? AttachCachedProjectKey(MapSettings(request.Settings), transport, result)
                : await AttachProjectKeyAsync(MapSettings(request.Settings), transport, regenerate: false, result, cancellationToken).ConfigureAwait(false);
            try
            {
                // Cloud + project key ⇒ the Bearer shape for every agent (contract §7). Otherwise the default is native
                // MCP OAuth (credential-free, HttpCredentialMode.Oauth); only the explicit "Advanced: use access token"
                // opt-in (with a token) writes the legacy PAT Bearer shape (Flow C).
                var config = transport == McpTransport.stdio
                    ? configurator.GetStdioConfig(settings, _logger)
                    : configurator.GetHttpConfig(settings, _logger, ResolveCredentialMode(settings, request.Settings));
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

        /// <summary>
        /// "Regenerate key" (project-keys contract §7, Cloud only): mint a fresh project key for this project's pin
        /// (the provider overwrites the cache entry and revokes the key it replaced), then rewrite every agent whose
        /// HTTP config is set up for this project so none keeps the old key. Returns the selected agent's refreshed
        /// description plus the rewritten agent ids. Not signed in / mint failure ⇒ <c>ok == false</c> and nothing
        /// changes (the old key stays cached and valid).
        /// </summary>
        public async Task<AgentConfigResultMessage> HandleRegenerateKeyAsync(AgentRegenerateKeyRequestMessage request, CancellationToken cancellationToken = default)
        {
            var result = NewResult(request.RequestId, IpcProtocol.Type.AgentRegenerateKey);
            if (request.Settings == null)
                return Fail(result, "No settings provided for agent-regenerate-key.");

            var baseSettings = MapSettings(request.Settings);
            if (baseSettings.ConnectionMode != McpConnectionMode.Cloud)
                return Fail(result, "Project keys are used only in Cloud mode.");

            // Every agent holding an HTTP entry routed to this project (whatever key it carries, or none) — the
            // configs that must not keep the old key.
            var targets = AiAgentConfiguratorRegistry.All.Where(c => HasPinnedHttpEntry(c, baseSettings)).ToList();

            var settings = await AttachProjectKeyAsync(baseSettings, McpTransport.streamableHttp, regenerate: true, result, cancellationToken).ConfigureAwait(false);
            if (!settings.HasProjectKey)
            {
                return Fail(result, result.ProjectKeyStatus == ProjectKeyStatus.SignedOut
                    ? "Sign in to ai-game.dev first — a project key is minted with this machine's sign-in."
                    : "Could not create a new project key; the current key is unchanged.");
            }

            var rewritten = new List<string>();
            var failed = new List<string>();
            foreach (var configurator in targets)
            {
                try
                {
                    if (configurator.GetHttpConfig(settings, _logger, settings.ResolveHttpCredentialMode()).Configure())
                        rewritten.Add(configurator.AgentId);
                    else
                    {
                        failed.Add(configurator.AgentName);
                        _logger?.LogWarning("Rewriting '{Agent}' with the regenerated project key failed.", configurator.AgentId);
                    }
                }
                catch (Exception ex)
                {
                    failed.Add(configurator.AgentName);
                    _logger?.LogWarning("Rewriting '{Agent}' with the regenerated project key failed: {Message}", configurator.AgentId, ex.Message);
                }
            }
            result.RewrittenAgents = rewritten;
            result.Ok = true;
            // RegenerateAsync revokes the replaced key before returning, so an agent whose rewrite failed may now
            // carry a dead key — say so in the key row instead of reporting a clean success.
            if (failed.Count > 0)
                result.ProjectKeyHint = $"Could not update: {string.Join(", ", failed)}. Their config still holds the previous (revoked) key — select each and press Configure.";

            var selected = AiAgentConfiguratorRegistry.GetByAgentId(request.AgentId);
            if (selected != null)
            {
                var transport = ParseTransport(request.Transport);
                result.Description = Describe(selected, transport == McpTransport.stdio ? baseSettings : settings, transport);
            }
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

        /// <summary>
        /// Generate the per-tool SKILL.md files for an agent into its resolved skills folder (§7, issue #101). The
        /// sidecar resolves WHERE (the agent's skills path) and WRITES the files from <paramref name="tools"/> — the
        /// tool catalog the C++ plugin already pushed over the IPC manifest — so no C++ generation logic remains.
        /// </summary>
        public AgentConfigResultMessage HandleGenerateSkills(AgentGenerateSkillsRequestMessage request, IReadOnlyList<ToolDescriptor> tools)
        {
            var result = NewResult(request.RequestId, IpcProtocol.Type.AgentGenerateSkills);

            var configurator = AiAgentConfiguratorRegistry.GetByAgentId(request.AgentId);
            if (configurator == null)
                return Fail(result, $"Unknown agent id '{request.AgentId}'.");

            ApplyEditableValue(configurator, request.CustomSkillsPath);

            var folder = configurator.SkillsPath;
            if (string.IsNullOrEmpty(folder))
                return Fail(result, $"Agent '{request.AgentId}' does not support skills.");

            var projectRoot = request.Settings?.ProjectRootPath ?? string.Empty;
            var skillsRoot = ResolveAbsolute(projectRoot, folder!);
            result.SkillsPath = skillsRoot;

            var genResult = _skillGenerator.Generate(tools ?? Array.Empty<ToolDescriptor>(), skillsRoot);
            result.Ok = genResult.Success;
            result.FilesWritten = genResult.FilesWritten;
            result.FilesPruned = genResult.FilesPruned;
            if (!genResult.Success)
                result.Error = genResult.Error ?? "Skill generation failed.";
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
                // 6.9.0: forward the tri-state status so the plugin can label the action button
                // (Reconfigure when ReconfigureNeeded) and so a "Reconfiguration Required" Alert renders.
                Status = description.Status.ToString(),
                IsInstalled = description.IsInstalled,
                SupportsSkills = configurator.SupportsSkills,
                // mcp-authorize PR 5 (design 06): forward the native-OAuth capability so the editor UI knows which
                // agents need the "Advanced: use access token" (PAT) escape hatch — SupportsOAuth == false clients.
                SupportsOAuth = configurator.SupportsOAuth,
                DownloadUrl = configurator.DownloadUrl,
                TutorialUrl = configurator.TutorialUrl,
                // 6.9.0: the top-level Link items (download / tutorial / docs), carried with their Url so the plugin
                // renders clickable hyperlinks. Each is a Link-kind ConfigurationItem.
                Links = description.Links
                    .Select(MapItem)
                    .ToList(),
                Sections = description.Sections
                    .Select(s => new AgentSectionDto
                    {
                        Heading = s.Heading,
                        ExpandedFirst = s.ExpandedFirst,
                        Items = s.Items
                            .Select(MapItem)
                            .ToList(),
                    })
                    .ToList(),
            };
        }

        /// <summary>Map one shared-library <c>ConfigurationItem</c> to the IPC <see cref="AgentItemDto"/>, carrying the
        /// 6.9.0 <c>Url</c> (non-empty only for <c>Link</c>-kind items).</summary>
        private static AgentItemDto MapItem(global::com.IvanMurzak.McpPlugin.AgentConfig.ConfigurationItem item) =>
            new() { Kind = item.Kind.ToString(), Text = item.Text, Url = item.Url ?? string.Empty };

        /// <summary>
        /// Map the inline IPC settings DTO into the shared library's host-aware settings.
        ///
        /// <para>
        /// mcp-authorize PR 5 (design 06, D11): the token is NO LONGER required input on the default path. With the
        /// device-flow machine credential store (PR 2) the client authorizes natively (MCP OAuth) and the written
        /// config is credential-free — the shared library's <c>GetHttpConfig</c> defaults to
        /// <c>HttpCredentialMode.Oauth</c> and never embeds a bearer. So <c>authOption</c> + <c>token</c> are carried
        /// ONLY for the explicit "Advanced: use access token" escape hatch (Flow C); <c>dto.AuthRequired</c> alone
        /// (which Cloud enforcement sets) no longer forces a bearer-carrying config. This drops the previous
        /// "AuthRequired ⇒ authOption.required" gating that made the default path token-required.
        /// </para>
        /// </summary>
        private static AgentConfiguratorSettings MapSettings(AgentSettingsDto dto)
        {
            var connectionMode = string.Equals(dto.ConnectionMode, "Cloud", StringComparison.OrdinalIgnoreCase)
                ? McpConnectionMode.Cloud
                : McpConnectionMode.Local;
            var useAccessToken = UsesAccessToken(dto);
            var authOption = useAccessToken ? McpAuthOption.required : McpAuthOption.none;
            return AgentConfiguratorSettings.CreateForHost(
                projectRootPath: dto.ProjectRootPath,
                executableFullPath: dto.ExecutableFullPath,
                port: dto.Port,
                timeoutMs: dto.TimeoutMs,
                host: dto.Host,
                token: useAccessToken ? dto.Token : null,
                connectionMode: connectionMode,
                authOption: authOption);
        }

        /// <summary>
        /// Whether a request opts into the "Advanced: use access token" escape hatch (mcp-authorize PR 5, design 06,
        /// Flow C): the plugin explicitly set <see cref="AgentSettingsDto.UseAccessToken"/> AND supplied a non-empty
        /// <see cref="AgentSettingsDto.Token"/>. Everything else is the default credential-free native-OAuth path.
        /// </summary>
        private static bool UsesAccessToken(AgentSettingsDto? dto) =>
            dto != null && dto.UseAccessToken && !string.IsNullOrEmpty(dto.Token);

        /// <summary>
        /// Resolve the HTTP credential mode for a request. A Cloud snapshot carrying a project key writes the Bearer
        /// shape for every agent (project-keys contract §7). Otherwise the default is native MCP OAuth
        /// (<see cref="McpHttpCredentialMode.Oauth"/>, URL-only — no embedded bearer); only the "Advanced: use access
        /// token" opt-in (see <see cref="UsesAccessToken"/>) selects <see cref="McpHttpCredentialMode.AccessToken"/>
        /// so the legacy Bearer shape is written for clients that cannot do MCP OAuth.
        /// </summary>
        private static McpHttpCredentialMode ResolveCredentialMode(AgentConfiguratorSettings settings, AgentSettingsDto? dto) =>
            UsesAccessToken(dto) ? McpHttpCredentialMode.AccessToken : settings.ResolveHttpCredentialMode();

        // --- Project keys (contract §7) ----------------------------------------------------------------

        /// <summary>Project keys apply only to a Cloud HTTP config (stdio and the local server are unchanged).</summary>
        private static bool UsesProjectKey(AgentConfiguratorSettings settings, McpTransport transport) =>
            settings.ConnectionMode == McpConnectionMode.Cloud && transport != McpTransport.stdio;

        /// <summary>
        /// Attach the CACHED key (synchronous file read — no network, no mint) so a status/list report compares against
        /// what Configure writes. Records the key status on <paramref name="result"/>.
        /// </summary>
        private AgentConfiguratorSettings AttachCachedProjectKey(
            AgentConfiguratorSettings settings, McpTransport transport, AgentConfigResultMessage result)
        {
            if (!TryGetProjectKeyProvider(settings, transport, result, out var provider))
                return settings;
            string? key = null;
            try
            {
                key = provider.Store.Get(provider.Issuer, settings.ProjectPin)?.Key;
            }
            catch (Exception ex)
            {
                _logger?.LogDebug("Reading the project-key cache failed: {Message}", ex.Message);
            }
            return ApplyProjectKey(result, settings, key, "No project key yet — Configure creates one for this project.");
        }

        /// <summary>
        /// Get-or-mint (or, with <paramref name="regenerate"/>, force-mint) the project key and attach it. Never throws
        /// for a key failure: no login / mint refused (e.g. 404 while the feature is off) / unreachable ⇒ the settings
        /// come back without a key and the config stays URL-only.
        /// </summary>
        private async Task<AgentConfiguratorSettings> AttachProjectKeyAsync(
            AgentConfiguratorSettings settings, McpTransport transport, bool regenerate,
            AgentConfigResultMessage result, CancellationToken cancellationToken)
        {
            if (!TryGetProjectKeyProvider(settings, transport, result, out var provider))
                return settings;
            string? key = null;
            try
            {
                var pin = settings.ProjectPin;
                key = regenerate
                    ? await provider.RegenerateAsync(pin, ProjectKeyEngine, Environment.MachineName, settings.ProjectRootPath, cancellationToken).ConfigureAwait(false)
                    : await provider.GetOrMintAsync(pin, ProjectKeyEngine, Environment.MachineName, settings.ProjectRootPath, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception ex) when (!(ex is OperationCanceledException))
            {
                _logger?.LogWarning("Project key {Action} failed: {Message}", regenerate ? "regenerate" : "get-or-mint", ex.Message);
            }
            return ApplyProjectKey(result, settings, key,
                "No project key could be obtained — the config is URL-only and the agent signs in with its own OAuth.");
        }

        /// <summary>The shared prologue: project keys apply (Cloud HTTP) and this machine is signed in. Otherwise
        /// records NotApplicable / SignedOut and returns false.</summary>
        private bool TryGetProjectKeyProvider(
            AgentConfiguratorSettings settings, McpTransport transport, AgentConfigResultMessage result,
            [NotNullWhen(true)] out ProjectKeyProvider? provider)
        {
            provider = null;
            if (!UsesProjectKey(settings, transport))
            {
                SetKeyStatus(result, settings, ProjectKeyStatus.NotApplicable, null);
                return false;
            }
            provider = _projectKeys?.Invoke();
            if (provider != null)
                return true;
            SetKeyStatus(result, settings, ProjectKeyStatus.SignedOut,
                "Not signed in — Cloud agent configs are URL-only and each agent signs in with its own OAuth. Sign in to use a project key.");
            return false;
        }

        /// <summary>The shared epilogue: attach <paramref name="key"/> (InUse) or record None with <paramref name="noneHint"/>.</summary>
        private static AgentConfiguratorSettings ApplyProjectKey(
            AgentConfigResultMessage result, AgentConfiguratorSettings settings, string? key, string noneHint) =>
            string.IsNullOrEmpty(key)
                ? SetKeyStatus(result, settings, ProjectKeyStatus.None, noneHint)
                : SetKeyStatus(result, settings.WithProjectKey(key), ProjectKeyStatus.InUse,
                    "Project key in use — Cloud agent configs carry a key bound to this project.");

        private static AgentConfiguratorSettings SetKeyStatus(
            AgentConfigResultMessage result, AgentConfiguratorSettings settings, string status, string? hint)
        {
            result.ProjectKeyStatus = status;
            result.ProjectKeyHint = hint;
            return settings;
        }

        /// <summary>
        /// True when the agent's config holds an MCP entry AND the file carries this project's pinned HTTP route
        /// (<c>/p/&lt;pin&gt;</c>) — i.e. an HTTP config for THIS project whatever key it carries, so Regenerate rewrites
        /// it even when its key is one the cache no longer knows (e.g. regenerated and revoked by another tool).
        /// </summary>
        private bool HasPinnedHttpEntry(AiAgentConfigurator configurator, AgentConfiguratorSettings settings)
        {
            try
            {
                var http = configurator.GetHttpConfig(settings, _logger);
                return http.IsDetected()
                    && File.ReadAllText(http.ConfigPath).Contains("/p/" + settings.ProjectPin, StringComparison.OrdinalIgnoreCase);
            }
            catch (Exception ex) when (ex is NotImplementedException || ex is IOException || ex is ArgumentException || ex is UnauthorizedAccessException)
            {
                return false;
            }
        }

        private static McpTransport ParseTransport(string? transport) =>
            string.Equals(transport, "stdio", StringComparison.OrdinalIgnoreCase)
                ? McpTransport.stdio
                : McpTransport.streamableHttp;

        /// <summary>Push the Custom agent's edited path into its configurator (no-op for built-in agents).</summary>
        private static void ApplyEditableValue(AiAgentConfigurator configurator, string? editableValue)
        {
            if (!string.IsNullOrEmpty(editableValue) && configurator is CustomConfigurator custom)
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
