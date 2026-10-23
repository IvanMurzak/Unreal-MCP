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
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    public class BuiltinSkillGuideTests : IDisposable
    {
        private readonly string _root = Path.Combine(Path.GetTempPath(), "UnrealSkillGuides_" + Guid.NewGuid().ToString("N"));
        private readonly ToolDescriptor[] _tools = { new() { Name = "actor-create", Description = "Create an actor." } };

        public void Dispose()
        {
            if (Directory.Exists(_root)) Directory.Delete(_root, true);
        }

        [Fact]
        public void GeneratesStandaloneGuidesWithoutRql_AndAppliesTheSameGlobalMetadata()
        {
            var generator = new SkillFileGenerator(metadata: new Dictionary<string, string>
            {
                ["generated-by"] = "unreal-mcp", ["ai-game-dev"] = "exclude",
            });
            var result = generator.Generate(_tools, _root);
            Assert.True(result.Success, result.Error);
            Assert.Equal(3, result.FilesWritten);
            foreach (var file in Directory.GetFiles(_root, "SKILL.md", SearchOption.AllDirectories))
                Assert.Contains("metadata:\n  generated-by: unreal-mcp\n  ai-game-dev: exclude\n", File.ReadAllText(file));

            var tool = File.ReadAllText(Path.Combine(_root, "actor-create", "SKILL.md"));
            Assert.Contains("unreal-cli", tool);
            Assert.Contains("ai-game-dev-app", tool);
            Assert.DoesNotContain("npm install", tool);
            Assert.Contains("npm install -g unreal-mcp-cli", File.ReadAllText(Path.Combine(_root, "unreal-cli", "SKILL.md")));
            Assert.Contains("Blender", File.ReadAllText(Path.Combine(_root, "ai-game-dev-app", "SKILL.md")));

            var snapshot = Directory.GetFiles(_root, "SKILL.md", SearchOption.AllDirectories).ToDictionary(p => p, File.ReadAllText);
            Assert.True(generator.Generate(_tools, _root).Success);
            foreach (var entry in snapshot) Assert.Equal(entry.Value, File.ReadAllText(entry.Key));
        }

        [Fact]
        public void MissingStandaloneGuideMakesGenerationFail()
        {
            Directory.CreateDirectory(_root);
            File.WriteAllText(Path.Combine(_root, "unreal-cli"), "blocks the guide directory");
            var result = new SkillFileGenerator().Generate(_tools, _root);
            Assert.False(result.Success);
            Assert.Contains("unreal-cli", result.Error);
        }
    }
}
