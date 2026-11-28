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
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
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
            Assert.Contains("setup-skills claude-code", guide);
            Assert.DoesNotContain("rql-schema", guide);
            Assert.DoesNotContain("<tool-name>", guide);
            Assert.DoesNotMatch("@[A-Z_]+@", guide);
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
            if (windows)
                script = "$ErrorActionPreference='Stop'\n$ProgressPreference='SilentlyContinue'\nSet-Location -LiteralPath '" +
                    dir.Replace("'", "''") + "'\n" + script + "\nif ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n";
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
                var diagnostics = await error;
                Assert.True(process.ExitCode == 0, $"Shell exited {process.ExitCode}: {diagnostics}");
                Assert.Equal(json, (await output).Trim());
                var bytes = File.ReadAllBytes(Path.Combine(dir, "args.json"));
                Assert.False(bytes.Length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf);
                Assert.Equal(json, File.ReadAllText(Path.Combine(dir, "args.json")).TrimEnd('\n', '\r'));

            }
            finally { Directory.Delete(root, recursive: true); }
        }

        [Fact]
        public void GeneratesStandaloneGuidesWithoutRql_AndMarksEachKindForTheApp()
        {
            // The sidecar's own metadata (DEV-313): the App hides a tool page and shows a standalone skill.
            using var host = new SidecarHost(new IpcClient("127.0.0.1", 39997, token: "test-token", sidecarVersion: "0.1.0"), "0.1.0");
            var generator = new SkillFileGenerator(toolMetadata: host.ToolSkillMetadata, standaloneMetadata: host.StandaloneSkillMetadata);
            var tools = new List<ToolDescriptor>(_tools)
            {
                new() { Name = "rql-query", Description = "Run a query.",
                    Skills = new List<ToolSkillDescriptor> { new() { Name = "unreal-rql", Description = "RQL.", Markdown = "# RQL\n" } } },
                new() { Name = "python-exec", Description = "Run Python." },
                // The renamed tool's hidden old id (DEV-314) still gets no page.
                new() { Name = "python-execute", Description = "Run Python.", AliasOf = "python-exec" },
            };
            var result = generator.Generate(tools, _root);
            Assert.True(result.Success, result.Error);
            Assert.Equal(6, result.FilesWritten);
            Assert.False(Directory.Exists(Path.Combine(_root, "python-execute")));
            Assert.Equal(6, Directory.GetFiles(_root, "SKILL.md", SearchOption.AllDirectories).Length);

            const string toolMarker = "metadata:\n  generated-by: unreal-mcp\n  ai-game-dev: exclude\n---\n";
            const string standaloneMarker = "metadata:\n  generated-by: unreal-mcp\n  ai-game-dev: include\n---\n";
            foreach (var name in new[] { "actor-create", "rql-query", "python-exec" })
                Assert.Contains(toolMarker, File.ReadAllText(Path.Combine(_root, name, "SKILL.md")));
            foreach (var name in new[] { "unreal-rql", BuiltinSkillGuides.CliSkillName, BuiltinSkillGuides.AppSkillName })
                Assert.Contains(standaloneMarker, File.ReadAllText(Path.Combine(_root, name, "SKILL.md")));

            var tool = File.ReadAllText(Path.Combine(_root, "actor-create", "SKILL.md"));
            Assert.Contains("the `unreal-mcp-cli` skill", tool);
            Assert.Contains("installation: `unreal-cli`", tool);
            Assert.Contains("ai-game-dev-app", tool);
            Assert.DoesNotContain("npm install", tool);
            Assert.Contains(BuiltinSkillGuides.IsWindows ? "npm.cmd install -g unreal-mcp-cli" : "npm install -g unreal-mcp-cli", File.ReadAllText(Path.Combine(_root, "unreal-cli", "SKILL.md")));
            Assert.Contains("Blender", File.ReadAllText(Path.Combine(_root, "ai-game-dev-app", "SKILL.md")));

            var snapshot = Directory.GetFiles(_root, "SKILL.md", SearchOption.AllDirectories).ToDictionary(p => p, File.ReadAllText);
            Assert.True(generator.Generate(tools, _root).Success);
            foreach (var entry in snapshot) Assert.Equal(entry.Value, File.ReadAllText(entry.Key));
        }

        [Fact]
        public void PrunesOnlyItsOwnStaleFolders_NeverTheCliSkillOrAUsersSkill()
        {
            Directory.CreateDirectory(_root);
            void Skill(string name, string body)
            {
                Directory.CreateDirectory(Path.Combine(_root, name));
                File.WriteAllText(Path.Combine(_root, name, "SKILL.md"), body);
            }
            const string ours = "---\nname: x\nmetadata:\n  generated-by: unreal-mcp\n---\nold\n";
            Skill("removed-tool", ours);                                   // a tool the catalog no longer has
            Skill(BuiltinSkillGuides.CliUsageSkillName, ours);             // written by unreal-mcp-cli setup-skills
            Skill("my-notes", "---\nname: my-notes\n---\nmine\n");       // a user's own skill
            var generator = new SkillFileGenerator(toolMetadata: new Dictionary<string, string> { ["generated-by"] = "unreal-mcp" });

            var result = generator.Generate(_tools, _root);

            Assert.True(result.Success, result.Error);
            Assert.Equal(1, result.FilesPruned);
            Assert.False(Directory.Exists(Path.Combine(_root, "removed-tool")));
            Assert.True(File.Exists(Path.Combine(_root, BuiltinSkillGuides.CliUsageSkillName, "SKILL.md")));
            Assert.True(File.Exists(Path.Combine(_root, "my-notes", "SKILL.md")));
            Assert.Equal(0, SkillFileGenerator.PruneStaleSkillFolders(_root, new HashSet<string>(), provenance: null));
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
