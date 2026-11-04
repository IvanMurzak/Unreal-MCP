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
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.McpPlugin.AgentConfig.Impl;
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Microsoft.Extensions.Logging;
using Xunit;
using McpConsts = com.IvanMurzak.McpPlugin.Common.Consts;
using McpTransport = com.IvanMurzak.McpPlugin.Common.Consts.MCP.Server.TransportMethod;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the sidecar's §7 AI-agent configurator service (the replacement for the deleted C++ Agents/ logic)
    /// serves every IPC request against the shared <c>com.IvanMurzak.McpPlugin.AgentConfig</c> library: lists the
    /// agent set, describes one agent (the UI DTO with sections/items/kinds), writes + removes a real config file
    /// on disk (round-trip), resolves a skills folder, and degrades cleanly on bad input. No live socket — the
    /// service is pure, so it is driven directly with hand-built requests.
    /// </summary>
    public class AgentConfigServiceTests : IDisposable
    {
        private readonly string _projectRoot;
        private readonly AgentConfigService _service = new();

        public AgentConfigServiceTests()
        {
            _projectRoot = Path.Combine(Path.GetTempPath(), "unreal-mcp-agentcfg-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(_projectRoot);
        }

        public void Dispose()
        {
            try { if (Directory.Exists(_projectRoot)) Directory.Delete(_projectRoot, recursive: true); }
            catch { /* best-effort temp cleanup */ }
        }

        private AgentSettingsDto Settings(string mode = "Local", bool authRequired = false, string? token = null, string host = "http://localhost:12345/mcp", bool useAccessToken = false) => new()
        {
            ProjectRootPath = _projectRoot,
            ExecutableFullPath = Path.Combine(_projectRoot, "server.exe"),
            Port = 12345,
            TimeoutMs = 30000,
            Host = host,
            Token = token,
            ConnectionMode = mode,
            AuthRequired = authRequired,
            UseAccessToken = useAccessToken,
        };

        [Fact]
        public void List_ReturnsTheFullAgentSet_WithCustomLast()
        {
            var result = _service.HandleList(new AgentsListRequestMessage
            {
                RequestId = "r1",
                Transport = "streamableHttp",
                Settings = Settings(),
            });

            Assert.True(result.Ok);
            Assert.Equal(IpcProtocol.Type.AgentsList, result.Op);
            Assert.NotNull(result.Agents);
            // Behaviour parity with the old C++ set: 16 agents, the Custom agent pinned last.
            Assert.Equal(16, result.Agents!.Count);
            Assert.Equal("other-custom", result.Agents[^1].AgentId);
            // Every agent carries an identity; the icon is a NAME, never bytes.
            Assert.All(result.Agents, a => Assert.False(string.IsNullOrEmpty(a.AgentId)));
            Assert.Contains(result.Agents, a => a.AgentId == "claude-code" && a.IconName == "claude-64.png");
        }

        [Fact]
        public void Status_DescribesOneAgent_WithSectionsAndItemKinds()
        {
            var result = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "r2",
                AgentId = "claude-code",
                Transport = "streamableHttp",
                Settings = Settings(),
            });

            Assert.True(result.Ok);
            Assert.NotNull(result.Description);
            var d = result.Description!;
            Assert.Equal("claude-code", d.AgentId);
            Assert.Equal("Claude Code", d.AgentName);
            Assert.False(d.IsConfigured); // nothing written yet
            Assert.NotEmpty(d.Sections);
            // The DTO carries the engine-agnostic item kinds the thin Slate panel renders.
            var kinds = d.Sections.SelectMany(s => s.Items).Select(i => i.Kind).Distinct().ToList();
            Assert.Contains("Description", kinds);
            Assert.Contains("ReadOnlyField", kinds);
        }

        [Fact]
        public void Status_CarriesTheTroubleshootingSection_ThroughTheDto()
        {
            // 6.10.0 AgentConfig extension: most built-in agents gained a per-agent "Troubleshooting" section.
            // The sidecar's Describe() serializes description.Sections generically (no heading/kind filter), so the
            // new section must surface in the emitted DTO without any passthrough change. claude-code carries one.
            var result = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "r2t", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });

            Assert.True(result.Ok);
            var d = result.Description!;
            var troubleshooting = d.Sections.FirstOrDefault(s => s.Heading == "Troubleshooting");
            Assert.NotNull(troubleshooting);
            // The section's guidance lines come through as Description-kind items with non-empty text.
            Assert.NotEmpty(troubleshooting!.Items);
            Assert.Contains(troubleshooting.Items, i => i.Kind == "Description" && !string.IsNullOrEmpty(i.Text));
        }

        [Fact]
        public void Status_CustomAgent_CarriesTheDockerCommands_ThroughTheDto()
        {
            // 6.10.0 AgentConfig extension: the Custom (other-custom) agent's Configuration section now spells out
            // the Docker lifecycle (run / start / stop / rm) as ReadOnlyField command items. The generic Sections
            // passthrough must carry those command strings into the DTO verbatim so the Slate panel can show them.
            var result = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "r2d", AgentId = "other-custom", Transport = "streamableHttp", Settings = Settings(),
            });

            Assert.True(result.Ok);
            var d = result.Description!;
            var commandTexts = d.Sections
                .SelectMany(s => s.Items)
                .Where(i => i.Kind == "ReadOnlyField")
                .Select(i => i.Text ?? string.Empty)
                .ToList();
            // The Docker run command (with the resolved port) and the start command must both be present verbatim.
            Assert.Contains(commandTexts, t => t.StartsWith("docker run") && t.Contains("12345"));
            Assert.Contains(commandTexts, t => t.StartsWith("docker start"));
        }

        [Fact]
        public void Status_CarriesTheTriStateStatus_AndTopLevelLinks()
        {
            // 6.9.0 DTO extension: the description carries the ConfiguratorStatus tri-state and a top-level Links
            // collection of Link-kind items (with their Url). claude-code has nothing written, so it is NotConfigured,
            // and it advertises a Download + a YouTube Tutorial link.
            var result = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "r2b", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });

            Assert.True(result.Ok);
            var d = result.Description!;
            Assert.Equal("NotConfigured", d.Status);
            Assert.NotEmpty(d.Links);
            Assert.All(d.Links, l => Assert.Equal("Link", l.Kind));
            // Each link carries an open-URL target the plugin renders as a clickable hyperlink.
            Assert.All(d.Links, l => Assert.False(string.IsNullOrEmpty(l.Url)));
            Assert.Contains(d.Links, l => l.Url == "https://youtu.be/Sknh2p12W8c");
        }

        [Fact]
        public async Task Status_ConnectionSettingDrift_NeedsReconfigure_Under73xTypedPortUrl()
        {
            // McpPlugin 7.0 (mcp-authorize) made the written HTTP config URL project-DETERMINISTIC — a project pin
            // + a derived per-project port (SHA256(projectRoot) → 20000–29999) and NO embedded token — so the raw
            // host/port carried in the settings no longer drives the on-disk entry (the library writes e.g.
            // `http://localhost:<derived>/mcp/p/<pin>`). Consequence: a connection-setting change alone (here a
            // different Host, same project root) no longer makes the entry "stale" — the desired URL recomputes
            // identically from the same project, so the shared library reports Configured, NOT ReconfigureNeeded,
            // and emits no "Reconfiguration Required" alert. (Pre-7.0 this exact scenario returned ReconfigureNeeded;
            // this test was updated when the 7.0 dependency was adopted. `AgentConfigService.Describe` is unchanged
            // and still forwards whatever tri-state the library produces — it just no longer produces ReconfigureNeeded
            // for host/port drift. The engine-side reconciliation of the new config model — MapSettings token drop,
            // the 8080→derived-port migration — is a later mcp-authorize PR; this locks the adopted 7.0 behavior so
            // that later change is deliberate.)
            //
            // RESOLVED (McpPlugin 7.3.0, PRs #174/#176): `AgentConfiguratorSettings.PinnedPort` inserts "an explicit
            // port typed into the Host" BETWEEN the marker override and the derived port, so the written URL is no
            // longer host-independent. A host/port drift therefore makes the on-disk entry genuinely stale again and
            // the correct status is ReconfigureNeeded -- the pre-7.0 behaviour, restored deliberately by owner ruling
            // (the user's typed port must be honoured). This test now locks THAT contract.
            // Configure once so the entry lands on disk (project-local .mcp.json under the isolated temp _projectRoot)...
            var configure = await _service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "r2c-cfg", AgentId = "claude-code", Transport = "streamableHttp",
                Settings = Settings(host: "http://localhost:12345/mcp"),
            });
            Assert.True(configure.Ok);
            Assert.True(configure.Description!.IsConfigured);

            // ...then describe with a DIFFERENT Host. Under 7.3.0 the typed loopback port participates in the
            // written URL (PinnedPort precedence level 2), so :12345 -> :54321 genuinely changes the desired URL
            // and the on-disk entry IS now stale -> ReconfigureNeeded, exactly as this scenario behaved pre-7.0.
            var result = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "r2c", AgentId = "claude-code", Transport = "streamableHttp",
                Settings = Settings(host: "http://localhost:54321/mcp"),
            });

            Assert.True(result.Ok);
            var d = result.Description!;
            Assert.Equal("ReconfigureNeeded", d.Status);
            Assert.Contains(d.Sections, s => s.Heading == "Reconfiguration Required");
        }

        [Fact]
        public void Status_UnknownAgent_FailsCleanly()
        {
            var result = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "r3",
                AgentId = "does-not-exist",
                Transport = "streamableHttp",
                Settings = Settings(),
            });

            Assert.False(result.Ok);
            Assert.NotNull(result.Error);
            Assert.Null(result.Description);
        }

        [Fact]
        public async Task Configure_ThenRemove_RoundTripsAConfigFileOnDisk()
        {
            // Claude Code writes the project-local .mcp.json under the project root — a deterministic, writable path.
            var status0 = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "s0", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });
            Assert.False(status0.Description!.IsConfigured);

            var configure = await _service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "c1", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });
            Assert.True(configure.Ok);
            // The refreshed description is returned so the panel updates without a follow-up round-trip.
            Assert.NotNull(configure.Description);
            Assert.True(configure.Description!.IsConfigured);

            var remove = _service.HandleRemove(new AgentRemoveRequestMessage
            {
                RequestId = "rm1", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });
            Assert.True(remove.Ok);
            Assert.NotNull(remove.Description);
            Assert.False(remove.Description!.IsConfigured);
        }

        [Fact]
        public async Task Configure_DefaultPath_WritesUrlOnly_NoBearer_EvenWhenAuthRequired()
        {
            // mcp-authorize PR 5 (design 06, D11): the token is NO LONGER required input on the default path. Even with
            // authRequired set AND a token supplied, the default path (useAccessToken = false → native MCP OAuth) writes
            // a credential-free HTTP config — the client authorizes natively, so no bearer lands in .mcp.json.
            const string pat = "secret-pat-should-not-be-written";
            var configure = await _service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "cfg-default", AgentId = "claude-code", Transport = "streamableHttp",
                Settings = Settings(authRequired: true, token: pat, useAccessToken: false),
            });
            Assert.True(configure.Ok);
            Assert.True(configure.Description!.IsConfigured);

            // The written project-local .mcp.json carries only the URL — never the bearer/token on the default path.
            var mcpJson = Path.Combine(_projectRoot, ".mcp.json");
            Assert.True(File.Exists(mcpJson));
            var content = File.ReadAllText(mcpJson);
            Assert.DoesNotContain(pat, content);
            Assert.DoesNotContain("Authorization", content);
            Assert.DoesNotContain("Bearer", content);
        }

        [Fact]
        public async Task Configure_AdvancedAccessToken_WritesTheLegacyBearer()
        {
            // mcp-authorize PR 5 (design 06, Flow C): the "Advanced: use access token" escape hatch (useAccessToken =
            // true, a token supplied) writes the legacy Bearer shape for clients that cannot do MCP OAuth — the PAT
            // lands in the config the shared library's HttpCredentialMode.AccessToken path produces.
            const string pat = "advanced-pat-abc123";
            var configure = await _service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "cfg-advanced", AgentId = "claude-code", Transport = "streamableHttp",
                Settings = Settings(authRequired: true, token: pat, useAccessToken: true),
            });
            Assert.True(configure.Ok);

            // The advanced escape hatch DOES embed the PAT (the legacy Bearer shape `headers.Authorization: Bearer …`)
            // — the deliberate Flow C trade-off for clients that cannot do MCP OAuth. Assert on the written file, the
            // real deliverable; the refreshed Description's IsConfigured is Oauth-mode (the shared library's Describe
            // has no credentialMode), so it reports ReconfigureNeeded here — a known status-detection limitation, not a
            // write failure (see design_notes).
            var mcpJson = Path.Combine(_projectRoot, ".mcp.json");
            Assert.True(File.Exists(mcpJson));
            var content = File.ReadAllText(mcpJson);
            Assert.Contains(pat, content);
            Assert.Contains("Bearer", content);
        }

        [Fact]
        public void List_ForwardsSupportsOAuth_ForEveryAgent()
        {
            // mcp-authorize PR 5 (design 06): the description DTO forwards each configurator's SupportsOAuth so the
            // editor UI knows which agents need the "Advanced: use access token" escape hatch (SupportsOAuth == false).
            var result = _service.HandleList(new AgentsListRequestMessage
            {
                RequestId = "oauth", Transport = "streamableHttp", Settings = Settings(),
            });
            Assert.True(result.Ok);
            Assert.NotNull(result.Agents);
            // Every built-in agent in the current registry supports native MCP OAuth (all default true); the field is
            // wired through so a future SupportsOAuth == false agent surfaces the escape-hatch signal to the plugin.
            Assert.Contains(result.Agents!, a => a.AgentId == "claude-code" && a.SupportsOAuth);
            Assert.All(result.Agents!, a => Assert.True(a.SupportsOAuth));
        }

        [Fact]
        public async Task Configure_CustomAgent_HasNoWritableFile_AndReportsClearly()
        {
            var result = await _service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "c2", AgentId = "other-custom", Transport = "streamableHttp", Settings = Settings(),
            });
            // The Custom agent is snippet-only: configure is not a crash, it is a clear non-success reason.
            Assert.False(result.Ok);
            Assert.NotNull(result.Error);
        }

        /// <summary>
        /// A stdio-only agent with the Claude Desktop setup shape (stdio = config file, Cloud HTTP = manual steps, local
        /// HTTP = unsupported) whose config file lives in the test's temp project, so the Configure guard is exercised
        /// end to end without touching any real user-profile config.
        /// </summary>
        private sealed class StdioOnlyConfigurator : AiAgentConfigurator
        {
            private readonly string _configPath;
            public StdioOnlyConfigurator(string configPath) => _configPath = configPath;

            public override string AgentName => "Stdio Only";
            public override string AgentId => "stdio-only";
            public override string DownloadUrl => "https://example.com";
            public override string? IconName => null;
            public override bool SupportsTransport(McpTransport transport) => transport == McpTransport.stdio;
            public override AgentSetupKind GetSetupKind(AgentConfiguratorSettings settings, McpTransport transport)
                => transport == McpTransport.stdio ? AgentSetupKind.ConfigFile
                    : settings.ConnectionMode == ConnectionMode.Cloud ? AgentSetupKind.Manual
                    : AgentSetupKind.Unsupported;
            protected override IReadOnlyList<ConfigurationSection> BuildManualSetupSections(AgentConfiguratorSettings settings)
                => new[] { new ConfigurationSection("Manual steps", true, new[] { ConfigurationItem.ReadOnlyField(settings.PinnedHttpUrl) }) };

            // AgentConfigBuilders is internal to McpPlugin, so the entries are built with the public JsonAiAgentConfig.
            protected override AiAgentConfig CreateStdioConfig(AgentConfiguratorSettings settings, ILogger? logger)
                => new JsonAiAgentConfig(AgentName, _configPath, McpConsts.MCP.Server.DefaultBodyPath, logger)
                    .SetProperty("type", JsonValue.Create("stdio")!, requiredForConfiguration: true)
                    .SetProperty("command", JsonValue.Create(settings.ExecutableFullPath)!, requiredForConfiguration: true)
                    .SetPropertyToRemove("url");
            protected override AiAgentConfig CreateHttpConfig(AgentConfiguratorSettings settings, ILogger? logger)
                => new JsonAiAgentConfig(AgentName, _configPath, McpConsts.MCP.Server.DefaultBodyPath, logger)
                    .SetProperty("type", JsonValue.Create("http")!, requiredForConfiguration: true)
                    .SetProperty("url", JsonValue.Create(settings.PinnedHttpUrl)!, requiredForConfiguration: true)
                    .SetPropertyToRemove("command");
            protected override IReadOnlyList<ConfigurationSection> BuildSections(
                AgentConfiguratorSettings settings, McpTransport transport, ILogger? logger)
                => DefaultConfigurationSections(settings, transport, logger);
        }

        [Fact]
        public void ClaudeDesktop_IsStdioOnly_AndItsHttpDescription_IsTheAlertOnly()
        {
            // Pure: SupportsTransport is a constant, and Describe returns the alert before reading any config file.
            var claudeDesktop = new ClaudeDesktopConfigurator();
            Assert.True(claudeDesktop.SupportsTransport(McpTransport.stdio));
            Assert.False(claudeDesktop.SupportsTransport(McpTransport.streamableHttp));
            Assert.StartsWith("Fully quit Claude Desktop", claudeDesktop.GetRestartNotice(McpTransport.stdio));

            var result = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "cd1", AgentId = "claude-desktop", Transport = "streamableHttp", Settings = Settings(),
            });

            Assert.True(result.Ok);
            var description = result.Description!;
            Assert.False(description.TransportSupported);
            Assert.False(description.IsConfigured);
            Assert.Equal("NotConfigured", description.Status);
            Assert.Equal("Unsupported", description.SetupKind);
            Assert.Null(description.RestartNotice);
            var section = Assert.Single(description.Sections);
            Assert.True(section.ExpandedFirst);
            var alert = Assert.Single(section.Items);
            Assert.Equal("Alert", alert.Kind);
            Assert.Contains("stdio", alert.Text);
        }

        [Fact]
        public void ClaudeDesktop_InCloudOverHttp_IsTheClaudeConnectorSteps()
        {
            // Pure: a Manual description is built from the settings alone (Cloud, not signed in: no key, no network).
            var result = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "cd-cloud", AgentId = "claude-desktop", Transport = "streamableHttp",
                Settings = Settings(mode: "Cloud", host: "https://ai-game.dev/mcp"),
            });

            Assert.True(result.Ok);
            var description = result.Description!;
            Assert.Equal("Manual", description.SetupKind);
            Assert.False(description.TransportSupported);
            Assert.False(description.IsConfigured);
            Assert.Null(description.RestartNotice);
            var steps = description.Sections[0];
            Assert.Equal("Add as a Claude Connector", steps.Heading);
            Assert.True(steps.ExpandedFirst);
            Assert.Contains(steps.Items, i => i.Kind == "Link" && i.Url == ClaudeDesktopConfigurator.ConnectorsSettingsUrl);
            var url = Assert.Single(steps.Items, i => i.Kind == "ReadOnlyField").Text;
            Assert.StartsWith("https://ai-game.dev/mcp/p/", url);
            Assert.DoesNotContain(description.Sections.SelectMany(x => x.Items), i => i.Kind == "Alert");
        }

        [Fact]
        public async Task Configure_ManualSetup_WritesNothing_AndReturnsTheSteps()
        {
            var configPath = Path.Combine(_projectRoot, "stdio-only.json");
            var service = new AgentConfigService(new AiAgentConfigurator[] { new StdioOnlyConfigurator(configPath) });

            var result = await service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "so-cloud", AgentId = "stdio-only", Transport = "streamableHttp",
                Settings = Settings(mode: "Cloud", host: "https://ai-game.dev/mcp"),
            });

            Assert.False(result.Ok);
            Assert.Equal("Manual", result.Description!.SetupKind);
            Assert.Equal("Manual steps", Assert.Single(result.Description.Sections).Heading);
            Assert.False(File.Exists(configPath));
        }

        [Fact]
        public async Task Configure_UnsupportedTransport_WritesNothing_AndReturnsTheAlert()
        {
            var configPath = Path.Combine(_projectRoot, "stdio-only.json");
            var service = new AgentConfigService(new AiAgentConfigurator[] { new StdioOnlyConfigurator(configPath) });

            var http = await service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "so1", AgentId = "stdio-only", Transport = "streamableHttp", Settings = Settings(),
            });
            Assert.False(http.Ok);
            Assert.Contains("no config file", http.Error);
            Assert.Equal("Alert", Assert.Single(Assert.Single(http.Description!.Sections).Items).Kind);
            Assert.False(http.Description!.TransportSupported);
            Assert.Equal("Unsupported", http.Description.SetupKind);
            Assert.False(File.Exists(configPath)); // the guard refused before any write

            var stdio = await service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "so2", AgentId = "stdio-only", Transport = "stdio", Settings = Settings(),
            });
            Assert.True(stdio.Ok);
            Assert.True(stdio.Description!.TransportSupported);
            Assert.Equal("ConfigFile", stdio.Description.SetupKind);
            Assert.StartsWith("Restart Stdio Only", stdio.Description.RestartNotice);
            Assert.True(File.Exists(configPath));
        }

        [Fact]
        public void List_FlagsOnlyClaudeDesktop_AsUnsupportedOverHttp()
        {
            var http = _service.HandleList(new AgentsListRequestMessage { RequestId = "l-http", Transport = "streamableHttp", Settings = Settings() });

            Assert.Equal(new[] { "claude-desktop" }, http.Agents!.Where(a => !a.TransportSupported).Select(a => a.AgentId));
        }

        [Fact]
        public void Status_RestartNotice_OnlyForStdio_AndNeverForTheCustomAgent()
        {
            string? Notice(string agentId, string transport) => _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "rn-" + agentId + transport, AgentId = agentId, Transport = transport, Settings = Settings(),
            }).Description!.RestartNotice;

            // claude-code keeps its config in the temp project root (.mcp.json); Claude Desktop is covered above.
            Assert.StartsWith("Restart Claude Code", Notice("claude-code", "stdio"));
            Assert.Null(Notice("claude-code", "streamableHttp"));
            Assert.Null(Notice("other-custom", "stdio"));
        }

        [Fact]
        public async Task Configure_Stdio_ReturnsTheRestartNotice_WithTheRefreshedDescription()
        {
            // Claude Code's stdio entry goes to the project-local .mcp.json under the temp project root.
            var configure = await _service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "c-stdio", AgentId = "claude-code", Transport = "stdio", Settings = Settings(),
            });

            Assert.True(configure.Ok);
            Assert.True(configure.Description!.TransportSupported);
            Assert.False(string.IsNullOrEmpty(configure.Description.RestartNotice));
        }

        [Fact]
        public void Remove_WhenNothingConfigured_IsIdempotentSuccess()
        {
            var result = _service.HandleRemove(new AgentRemoveRequestMessage
            {
                RequestId = "rm2", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });
            Assert.True(result.Ok); // "nothing to remove" is still success
        }

        [Fact]
        public void SkillsPath_ResolvesUnderProjectRoot_ForAnAgentThatSupportsSkills()
        {
            var result = _service.HandleSkillsPath(new AgentSkillsPathRequestMessage
            {
                RequestId = "sp1", AgentId = "claude-code", Settings = Settings(),
            });
            Assert.True(result.Ok);
            Assert.NotNull(result.SkillsPath);
            // Absolute, forward-slashed, under the project root.
            Assert.StartsWith(_projectRoot.Replace('\\', '/'), result.SkillsPath!);
        }

        [Fact]
        public void SkillsPath_Custom_HonoursTheEditedValue()
        {
            var result = _service.HandleSkillsPath(new AgentSkillsPathRequestMessage
            {
                RequestId = "sp2", AgentId = "other-custom", CustomSkillsPath = "my/custom/skills", Settings = Settings(),
            });
            Assert.True(result.Ok);
            Assert.NotNull(result.SkillsPath);
            Assert.EndsWith("my/custom/skills", result.SkillsPath!);
        }

        [Fact]
        public void List_WithoutSettings_FailsCleanly()
        {
            var result = _service.HandleList(new AgentsListRequestMessage { RequestId = "r4", Settings = null });
            Assert.False(result.Ok);
            Assert.NotNull(result.Error);
            Assert.Null(result.Agents);
        }

        [Fact]
        public void GenerateSkills_WritesSkillFilesUnderTheResolvedFolder_FromTheToolCatalog()
        {
            // The catalog the plugin would have pushed over the manifest (carried in by the host).
            var tools = new List<global::com.IvanMurzak.Unreal.MCP.Bridge.Ipc.ToolDescriptor>
            {
                new() { Name = "actor-create", Title = "Actor Create", Description = "Spawns an actor." },
                new() { Name = "ping", Title = "Ping", Description = "Health probe." },
            };

            var result = _service.HandleGenerateSkills(new AgentGenerateSkillsRequestMessage
            {
                RequestId = "gs1", AgentId = "claude-code", Settings = Settings(),
            }, tools);

            Assert.True(result.Ok);
            Assert.Equal(IpcProtocol.Type.AgentGenerateSkills, result.Op);
            Assert.Equal(4, result.FilesWritten);
            Assert.NotNull(result.SkillsPath);
            Assert.True(File.Exists(Path.Combine(result.SkillsPath!, "actor-create", "SKILL.md")));
            Assert.True(File.Exists(Path.Combine(result.SkillsPath!, "ping", "SKILL.md")));
        }

        [Fact]
        public void GenerateSkills_NonSkillsAgent_FailsCleanly()
        {
            var result = _service.HandleGenerateSkills(new AgentGenerateSkillsRequestMessage
            {
                RequestId = "gs2", AgentId = "claude-desktop", Settings = Settings(),
            }, new List<global::com.IvanMurzak.Unreal.MCP.Bridge.Ipc.ToolDescriptor>());
            // claude-desktop is one of the agents Unity leaves without a skills path.
            Assert.False(result.Ok);
            Assert.NotNull(result.Error);
        }

        [Fact]
        public void ResolveAbsolute_NormalisesSeparatorsAndRoots()
        {
            // Use an OS-rooted project root + an OS-rooted "elsewhere" path so the assertions hold on
            // both Windows and Linux: "C:/root" is rooted on Windows but NOT on Linux (where Path.IsPathRooted
            // returns false for a drive-letter path), so a Windows-only expectation re-roots under the runner CWD
            // on Linux. ResolveAbsolute itself is correct on both platforms (Path.IsPathRooted / Path.GetFullPath
            // semantics); the inputs/expectations are what must be platform-aware.
            var isWindows = RuntimeInformation.IsOSPlatform(OSPlatform.Windows);
            var projectRoot = isWindows ? "C:/root" : "/root";
            var elsewhere = isWindows ? "D:/elsewhere/skills" : "/elsewhere/skills";

            // A project-relative folder is combined under the (rooted) project root and forward-slashed.
            var rel = AgentConfigService.ResolveAbsolute(projectRoot, ".claude/skills");
            Assert.Equal(projectRoot + "/.claude/skills", rel);
            // An already-rooted path is returned (forward-slashed), not re-rooted under the project.
            var abs = AgentConfigService.ResolveAbsolute(projectRoot, elsewhere);
            Assert.Equal(elsewhere, abs);
        }
    }
}
