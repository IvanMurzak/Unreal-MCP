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
using System.Diagnostics;
using System.Text;
using System.Threading.Tasks;
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

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public void GuideContainsOnlyTheTargetShellAndExecutableExamples(bool windows)
        {
            var guide = BuiltinSkillGuides.BuildCliMarkdown(windows);
            Assert.Contains(windows ? "```powershell" : "```bash", guide);
            Assert.DoesNotContain(windows ? "```bash" : "```powershell", guide);
            Assert.Contains(windows ? "npm.cmd install -g unreal-mcp-cli" : "npm install -g unreal-mcp-cli", guide);
            Assert.Contains("run-tool rql-schema --input '{}'", guide);
            Assert.DoesNotContain("<tool-name>", guide);
            Assert.DoesNotContain("@SHELL@", guide);
            Assert.DoesNotContain("@unreal-mcp-cli@", guide);
            Assert.DoesNotContain("<<'EOF'", guide);
        }

        [Fact]
        public async Task GeneratedCommandRoundTripsQuotesUnicodeAndNewlinesThroughTheHostShell()
        {
            var windows = BuiltinSkillGuides.IsWindows;
            const string json = "{\"text\":\"Bob's hero \\\"quoted\\\" C:\\\\Games\\\\Hero $HOME `tick`\\n\\u041f\\u0440\\u0438\\u0432\\u0435\\u0442\"}";
            var block = BuiltinSkillGuides.ToolCommand("run-tool", "example-tool", json, windows);
            var command = block.Substring(block.IndexOf('\n') + 1);
            command = command.Substring(0, command.LastIndexOf("```", StringComparison.Ordinal));
            var script = windows
                ? "$env:PATH = (Get-Location).Path + ';' + $env:PATH\n" + command
                : BuiltinSkillGuides.JsonFile(json, false) + "\nunreal-mcp-cli() { printf '%s' \"$4\"; }\n" + command;
            var root = Path.Combine(Path.GetTempPath(), "Skill shell path " + Guid.NewGuid().ToString("N"));
            var dir = Path.Combine(root, "project with spaces");
            Directory.CreateDirectory(dir);
            if (windows) script = "Set-Location -LiteralPath '" + dir.Replace("'", "''") + "'\n" + script;
            try
            {
                if (windows)
                    File.WriteAllText(Path.Combine(dir, "unreal-mcp-cli.cmd"),
                        "@echo off\r\nif not \"%~1\"==\"run-tool\" exit /b 2\r\nif not \"%~3\"==\"--input-file\" exit /b 3\r\ntype \"%~4\"\r\n");
                var start = new ProcessStartInfo(windows ? "powershell.exe" : "/bin/bash")
                {
                    WorkingDirectory = windows ? root : dir, UseShellExecute = false,
                    RedirectStandardOutput = true, RedirectStandardError = true,
                };
                if (windows)
                {
                    start.ArgumentList.Add("-NoProfile");
                    start.ArgumentList.Add("-NonInteractive");
                    start.ArgumentList.Add("-EncodedCommand");
                    start.ArgumentList.Add(Convert.ToBase64String(Encoding.Unicode.GetBytes(script)));
                }
                else { start.ArgumentList.Add("-c"); start.ArgumentList.Add(script); }
                using var process = Process.Start(start)!;
                var output = process.StandardOutput.ReadToEndAsync();
                var error = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(20000)) { process.Kill(entireProcessTree: true); throw new TimeoutException("Skill shell command hung"); }
                Assert.Equal(0, process.ExitCode);
                Assert.Equal(json, (await output).Trim());
                var bytes = File.ReadAllBytes(Path.Combine(dir, "args.json"));
                Assert.False(bytes.Length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf);
                Assert.Equal(json, File.ReadAllText(Path.Combine(dir, "args.json")).TrimEnd('\n', '\r'));
                Assert.True(string.IsNullOrWhiteSpace(await error));
            }
            finally { Directory.Delete(root, recursive: true); }
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
            Assert.Contains(BuiltinSkillGuides.IsWindows ? "npm.cmd install -g unreal-mcp-cli" : "npm install -g unreal-mcp-cli", File.ReadAllText(Path.Combine(_root, "unreal-cli", "SKILL.md")));
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
