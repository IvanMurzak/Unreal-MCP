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
using System.Text.Json;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// The `unreal-skill-generate` SYSTEM tool (§2.4) — the sidecar-native sibling of the C++
    /// `unreal-skill-create`. Covers the surface tag, path validation, the not-connected-yet guard, the
    /// system-tool exclusion from generated docs, and the structured result.
    /// </summary>
    public class SkillGenerateToolTests : IDisposable
    {
        private readonly string _projectRoot;

        public SkillGenerateToolTests()
        {
            _projectRoot = Path.Combine(Path.GetTempPath(), "unreal-mcp-skillgen-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(_projectRoot);
        }

        public void Dispose()
        {
            try { if (Directory.Exists(_projectRoot)) Directory.Delete(_projectRoot, recursive: true); }
            catch { /* best-effort temp cleanup */ }
        }

        private static ToolDescriptor Tool(string name, string? toolType = null) =>
            new() { Name = name, Title = name, Description = name + " does a thing.", ToolType = toolType, SchemaHash = "h" };

        private static IReadOnlyDictionary<string, JsonElement> Args(params (string Key, string Value)[] pairs)
        {
            var dict = new Dictionary<string, JsonElement>();
            foreach (var (key, value) in pairs)
                dict[key] = JsonDocument.Parse(JsonSerializer.Serialize(value)).RootElement.Clone();
            return dict;
        }

        private ResponseCallTool Run(IReadOnlyDictionary<string, JsonElement>? args, params ToolDescriptor[] tools) =>
            SkillGenerateTool.Run(() => tools, () => _projectRoot, new SkillFileGenerator(), args);

        [Fact]
        public void Create_DeclaresTheToolOnTheSystemSurface()
        {
            var tool = SkillGenerateTool.Create(() => Array.Empty<ToolDescriptor>(), () => _projectRoot, new SkillFileGenerator());
            Assert.Equal(SkillGenerateTool.ToolId, tool.Name);
            Assert.Equal("unreal-skill-generate", tool.Name);
            Assert.Equal(McpToolType.System, tool.ToolType);
        }

        [Fact]
        public void Run_DefaultsToTheClaudeSkillsFolder()
        {
            var response = Run(null, Tool("actor-create"));

            Assert.Equal(ResponseStatus.Success, response.Status);
            Assert.True(File.Exists(Path.Combine(_projectRoot, ".claude", "skills", "actor-create", "SKILL.md")));
        }

        [Fact]
        public void Run_HonoursAProjectRelativePath()
        {
            var response = Run(Args(("path", "docs/skills")), Tool("actor-create"));

            Assert.Equal(ResponseStatus.Success, response.Status);
            Assert.True(File.Exists(Path.Combine(_projectRoot, "docs", "skills", "actor-create", "SKILL.md")));
        }

        [Fact]
        public void Run_DoesNotDocumentSystemTools()
        {
            // A SKILL.md is agent-facing documentation; a system tool is one an agent must never call, so
            // documenting it would re-expose it through the skills channel.
            var response = Run(null, Tool("ping", "system"), Tool("unreal-skill-create", "system"), Tool("actor-create"));

            Assert.Equal(ResponseStatus.Success, response.Status);
            var skills = Path.Combine(_projectRoot, ".claude", "skills");
            Assert.True(Directory.Exists(Path.Combine(skills, "actor-create")));
            Assert.False(Directory.Exists(Path.Combine(skills, "ping")));
            Assert.False(Directory.Exists(Path.Combine(skills, "unreal-skill-create")));
        }

        [Fact]
        public void Run_PrunesAStaleSystemToolSkillFolder()
        {
            // A folder generated before §2.4 (when `ping` was standard) must disappear on the next run.
            var stale = Path.Combine(_projectRoot, ".claude", "skills", "ping");
            Directory.CreateDirectory(stale);
            File.WriteAllText(Path.Combine(stale, "SKILL.md"), "---\nname: ping\n---\n");

            var response = Run(null, Tool("ping", "system"), Tool("actor-create"));

            Assert.Equal(ResponseStatus.Success, response.Status);
            Assert.False(Directory.Exists(stale));
        }

        [Theory]
        [InlineData("../../etc")]
        [InlineData("skills/..")]
        public void Run_RejectsTraversal(string path)
        {
            var response = Run(Args(("path", path)), Tool("actor-create"));

            Assert.Equal(ResponseStatus.Error, response.Status);
            Assert.Equal(ResponseErrorKind.BadRequest, response.ErrorKind);
        }

        [Fact]
        public void Run_RejectsAnAbsolutePath()
        {
            var absolute = Path.Combine(Path.GetTempPath(), "elsewhere");
            var response = Run(Args(("path", absolute)), Tool("actor-create"));

            Assert.Equal(ResponseStatus.Error, response.Status);
            Assert.Equal(ResponseErrorKind.BadRequest, response.ErrorKind);
            Assert.False(Directory.Exists(absolute));
        }

        [Fact]
        public void Run_FailsClearlyBeforeTheHandshake()
        {
            // The project root only arrives with the handshake-ack; generating into "wherever the sidecar
            // happens to be running" would silently scatter files.
            var response = SkillGenerateTool.Run(
                () => new[] { Tool("actor-create") }, () => null, new SkillFileGenerator(), null);

            Assert.Equal(ResponseStatus.Error, response.Status);
            Assert.Equal(ResponseErrorKind.Conflict, response.ErrorKind);
        }

        [Fact]
        public void Run_ReportsCountsInStructuredContent()
        {
            var response = Run(null, Tool("actor-create"), Tool("ping", "system"));

            Assert.NotNull(response.StructuredContent);
            var structured = response.StructuredContent!;
            Assert.Equal(1, structured["filesWritten"]!.GetValue<int>());
            Assert.Equal(2, structured["toolsConsidered"]!.GetValue<int>());
            Assert.Contains("skills", structured["skillsPath"]!.GetValue<string>());
        }

        [Fact]
        public void Run_IsIdempotent()
        {
            var first = Run(null, Tool("actor-create"));
            var second = Run(null, Tool("actor-create"));

            Assert.Equal(ResponseStatus.Success, first.Status);
            Assert.Equal(ResponseStatus.Success, second.Status);
            Assert.Equal(0, second.StructuredContent!["filesPruned"]!.GetValue<int>());
        }
    }
}
