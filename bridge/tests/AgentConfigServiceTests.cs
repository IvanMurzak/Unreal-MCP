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
using System.IO;
using System.Linq;
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

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

        private AgentSettingsDto Settings(string mode = "Local", bool authRequired = false, string? token = null) => new()
        {
            ProjectRootPath = _projectRoot,
            ExecutableFullPath = Path.Combine(_projectRoot, "server.exe"),
            Port = 12345,
            TimeoutMs = 30000,
            Host = "http://localhost:12345/mcp",
            Token = token,
            ConnectionMode = mode,
            AuthRequired = authRequired,
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
        public void Configure_ThenRemove_RoundTripsAConfigFileOnDisk()
        {
            // Claude Code writes the project-local .mcp.json under the project root — a deterministic, writable path.
            var status0 = _service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "s0", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });
            Assert.False(status0.Description!.IsConfigured);

            var configure = _service.HandleConfigure(new AgentConfigureRequestMessage
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
        public void Configure_CustomAgent_HasNoWritableFile_AndReportsClearly()
        {
            var result = _service.HandleConfigure(new AgentConfigureRequestMessage
            {
                RequestId = "c2", AgentId = "other-custom", Transport = "streamableHttp", Settings = Settings(),
            });
            // The Custom agent is snippet-only: configure is not a crash, it is a clear non-success reason.
            Assert.False(result.Ok);
            Assert.NotNull(result.Error);
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
        public void ResolveAbsolute_NormalisesSeparatorsAndRoots()
        {
            var rel = AgentConfigService.ResolveAbsolute("C:/root", ".claude/skills");
            Assert.Equal("C:/root/.claude/skills", rel);
            // An already-rooted path is returned (forward-slashed), not re-rooted under the project.
            var abs = AgentConfigService.ResolveAbsolute("C:/root", "D:/elsewhere/skills");
            Assert.Equal("D:/elsewhere/skills", abs);
        }
    }
}
