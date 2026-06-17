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
using System.Text.Json.Nodes;
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the sidecar SKILL.md generator (the replacement for the removed C++ FUnrealMcpSkillFileGenerator,
    /// issue #101) reproduces the same SKILL.md shape from the tool manifest catalog the plugin pushes over IPC:
    /// YAML front-matter, title, description body, hints line, param table (derived from the input schema), JSON
    /// Schema fences, the token-free unreal-mcp-cli call form — plus the on-disk write + stale-folder pruning.
    /// The shape assertions mirror the old C++ UnrealMcpAgentSkillsSpec expectations 1:1.
    /// </summary>
    public class SkillFileGeneratorTests : IDisposable
    {
        private readonly string _root;

        public SkillFileGeneratorTests()
        {
            _root = Path.Combine(Path.GetTempPath(), "unreal-mcp-skills-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(_root);
        }

        public void Dispose()
        {
            try { if (Directory.Exists(_root)) Directory.Delete(_root, recursive: true); }
            catch { /* best-effort temp cleanup */ }
        }

        // A hand-built tool with one required + one optional param and a description carrying a literal pipe — the
        // same fixture shape the old C++ spec used to assert table escaping.
        private static ToolDescriptor DemoTool()
        {
            var schema = new JsonObject
            {
                ["type"] = "object",
                ["properties"] = new JsonObject
                {
                    ["name"] = new JsonObject { ["type"] = "string", ["description"] = "The thing's name." },
                    ["count"] = new JsonObject { ["type"] = "integer", ["description"] = "How many | with a pipe." },
                },
                ["required"] = new JsonArray { "name" },
            };
            return new ToolDescriptor
            {
                Name = "demo-tool",
                Title = "Demo Tool",
                Description = "Does a demo thing.",
                ReadOnlyHint = true,
                IdempotentHint = true,
                InputSchema = schema,
            };
        }

        [Fact]
        public void BuildSkillMarkdown_EmitsFrontMatterTitleTableSchemaAndCallForm()
        {
            var md = SkillFileGenerator.BuildSkillMarkdown(DemoTool());

            Assert.StartsWith("---", md);
            Assert.Contains("name: demo-tool", md);
            Assert.Contains("description: \"", md);
            Assert.Contains("# Demo Tool", md);
            Assert.Contains("## Input", md);
            Assert.Contains("| Name | Type | Required | Description |", md);
            Assert.Contains("| `name` | string | yes |", md);
            Assert.Contains("| `count` | integer | no |", md);
            // A stray pipe in a description is escaped so it does not break the markdown table.
            Assert.Contains("How many \\| with a pipe.", md);
            Assert.Contains("### Input JSON Schema", md);
            Assert.Contains("**Hints:** read-only, idempotent", md);
            Assert.Contains("unreal-mcp-cli run-tool demo-tool", md);
            // Token discipline (§8): a skill file is pure docs — no token/secret anywhere.
            Assert.DoesNotContain("token", md, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("Bearer", md);
        }

        [Fact]
        public void BuildSkillMarkdown_NoParams_SaysSo()
        {
            var tool = new ToolDescriptor { Name = "ping", Title = "Ping", Description = "Health probe." };
            var md = SkillFileGenerator.BuildSkillMarkdown(tool);
            Assert.Contains("This tool takes no parameters.", md);
            Assert.DoesNotContain("| Name | Type | Required | Description |", md);
        }

        [Fact]
        public void SanitizeSkillFolderName_KebabsAndCollapses()
        {
            Assert.Equal("demo-tool", SkillFileGenerator.SanitizeSkillFolderName("demo-tool"));
            Assert.Equal("a-b-c", SkillFileGenerator.SanitizeSkillFolderName("a__b  c"));
            Assert.Equal("tool", SkillFileGenerator.SanitizeSkillFolderName("***"));
            Assert.Equal("actorcreate", SkillFileGenerator.SanitizeSkillFolderName("ActorCreate"));
        }

        [Fact]
        public void Generate_WritesOneSkillPerToolUnderSanitizedFolders()
        {
            var gen = new SkillFileGenerator();
            var tools = new List<ToolDescriptor> { DemoTool(), new ToolDescriptor { Name = "ping", Title = "Ping", Description = "Probe." } };

            var result = gen.Generate(tools, _root);

            Assert.True(result.Success);
            Assert.Equal(2, result.FilesWritten);
            Assert.True(File.Exists(Path.Combine(_root, "demo-tool", "SKILL.md")));
            Assert.True(File.Exists(Path.Combine(_root, "ping", "SKILL.md")));
            // The written content is the same as the pure builder produced.
            Assert.Equal(SkillFileGenerator.BuildSkillMarkdown(DemoTool()),
                File.ReadAllText(Path.Combine(_root, "demo-tool", "SKILL.md")));
        }

        [Fact]
        public void Generate_PrunesStaleGeneratorOwnedFolders_ButKeepsUserContent()
        {
            var gen = new SkillFileGenerator();
            // First run writes demo-tool + ping.
            gen.Generate(new List<ToolDescriptor> { DemoTool(), new ToolDescriptor { Name = "ping", Description = "p" } }, _root);
            // An unrelated user folder WITHOUT a SKILL.md must never be pruned.
            var userDir = Path.Combine(_root, "my-notes");
            Directory.CreateDirectory(userDir);
            File.WriteAllText(Path.Combine(userDir, "notes.txt"), "keep me");

            // Second run drops 'ping' from the catalog → its generator-owned folder is pruned.
            var result = gen.Generate(new List<ToolDescriptor> { DemoTool() }, _root);

            Assert.True(result.Success);
            Assert.Equal(1, result.FilesPruned);
            Assert.True(Directory.Exists(Path.Combine(_root, "demo-tool")));
            Assert.False(Directory.Exists(Path.Combine(_root, "ping")));   // stale generator folder pruned
            Assert.True(File.Exists(Path.Combine(userDir, "notes.txt")));  // user content untouched
        }

        [Fact]
        public void Generate_EmptyRoot_Fails()
        {
            var gen = new SkillFileGenerator();
            var result = gen.Generate(new List<ToolDescriptor> { DemoTool() }, string.Empty);
            Assert.False(result.Success);
            Assert.NotNull(result.Error);
        }

        [Fact]
        public void BuildParamRows_DerivesTypeRequiredDescriptionFromSchema()
        {
            var rows = SkillFileGenerator.BuildParamRows(DemoTool().InputSchema);
            Assert.Equal(2, rows.Count);
            Assert.Contains(rows, r => r.StartsWith("| `name` | string | yes |"));
            Assert.Contains(rows, r => r.StartsWith("| `count` | integer | no |"));
        }
    }
}
