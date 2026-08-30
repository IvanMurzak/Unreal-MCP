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
using System.Text;
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

        /// <summary>Where <see cref="DemoTool"/>'s SKILL.md lands under the per-test temp root.</summary>
        private string DemoSkillPath => Path.Combine(_root, "demo-tool", "SKILL.md");

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
        public void BuildSkillMarkdown_PrefersSkillDescriptionAndSkillBody_WithFallbackToDescription()
        {
            // When the manifest carries the dedicated [AiSkillDescription]/[AiSkillBody] analog fields, the YAML
            // front-matter uses the short description and the body uses the skill body.
            var rich = new ToolDescriptor
            {
                Name = "rich-tool",
                Title = "Rich Tool",
                Description = "Full prose description used as the fallback.",
                SkillDescription = "Short one-liner for YAML.",
                SkillBody = "The richer agent-facing skill body.",
            };
            var md = SkillFileGenerator.BuildSkillMarkdown(rich);
            Assert.Contains("description: \"Short one-liner for YAML.\"", md);
            Assert.Contains("The richer agent-facing skill body.", md);

            // With neither present, BOTH fall back to the full Description (the Unreal manifest's current shape).
            var plain = new ToolDescriptor { Name = "plain-tool", Title = "Plain", Description = "Only a description." };
            var pmd = SkillFileGenerator.BuildSkillMarkdown(plain);
            Assert.Contains("description: \"Only a description.\"", pmd);
            Assert.Contains("Only a description.", pmd);
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
            Assert.True(File.Exists(DemoSkillPath));
            Assert.True(File.Exists(Path.Combine(_root, "ping", "SKILL.md")));
            // The written content is the same as the pure builder produced.
            Assert.Equal(SkillFileGenerator.BuildSkillMarkdown(DemoTool()),
                File.ReadAllText(DemoSkillPath));
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
        public void Generate_SkipsSystemTools()
        {
            var gen = new SkillFileGenerator();
            var tools = new List<ToolDescriptor>
            {
                DemoTool(),
                new ToolDescriptor { Name = "ping", Description = "p", ToolType = "system" },
            };

            var result = gen.Generate(tools, _root);

            Assert.True(result.Success);
            Assert.Equal(1, result.FilesWritten);
            Assert.True(Directory.Exists(Path.Combine(_root, "demo-tool")));
            // A system tool is never documented — writing it would re-expose it through the skills channel.
            Assert.False(Directory.Exists(Path.Combine(_root, "ping")));
        }

        [Fact]
        public void Generate_AllSystemCatalog_PrunesNothingAndReportsFailure()
        {
            // REGRESSION (§2.4): when EVERY tool is a system tool the documented set is empty, so the stale-folder
            // prune would run with an empty "current" set and delete every skill folder under the root — then
            // report Success. That case is routine now: the runtime (in-game) built-in surface is `ping` ALONE and
            // `ping` became a system tool, so a bare packaged game hits it on every `unreal-skill-generate`.
            var gen = new SkillFileGenerator();
            gen.Generate(new List<ToolDescriptor> { DemoTool() }, _root);
            Assert.True(Directory.Exists(Path.Combine(_root, "demo-tool")));

            var result = gen.Generate(
                new List<ToolDescriptor> { new ToolDescriptor { Name = "ping", Description = "p", ToolType = "system" } },
                _root);

            Assert.False(result.Success);
            Assert.NotNull(result.Error);
            Assert.Equal(0, result.FilesPruned);
            Assert.True(Directory.Exists(Path.Combine(_root, "demo-tool")));   // NOT wiped
        }

        [Fact]
        public void Generate_EmptyCatalog_PrunesNothingAndReportsFailure()
        {
            // The sibling case: no manifest pushed yet. Same hazard, same refusal.
            var gen = new SkillFileGenerator();
            gen.Generate(new List<ToolDescriptor> { DemoTool() }, _root);

            var result = gen.Generate(new List<ToolDescriptor>(), _root);

            Assert.False(result.Success);
            Assert.Equal(0, result.FilesPruned);
            Assert.True(Directory.Exists(Path.Combine(_root, "demo-tool")));   // NOT wiped
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

        // --- Provenance marker -------------------------------------------------------------------------
        //
        // A generated SKILL.md carries a front-matter marker identifying it as generated rather than
        // hand-authored, so a consumer can dedup its own generated skills against the live tool catalog while
        // leaving user-authored files alone. The marker is two literal lines, the LAST block inside the front
        // matter:
        //
        //     metadata:
        //       generated-by: mcp-plugin-dotnet
        //
        // The two-space indent is load-bearing — it is what makes this a nested YAML mapping rather than a
        // sibling scalar — so these assertions compare whole lines POSITIONALLY and UN-TRIMMED.

        private const string MarkerBlockLine = "metadata:";
        private const string MarkerEntryLine = "  generated-by: mcp-plugin-dotnet";

        /// <summary>
        /// Assert a marker line occurs exactly once in <paramref name="region"/>. `Assert.Equal(1, n)` and
        /// `Assert.Single` have no message overload, so an `Assert.True` carries <paramref name="half"/> —
        /// otherwise every one of these prints an indistinguishable "Expected: 1 / Actual: 2" and a red names
        /// the test without naming which half broke.
        /// </summary>
        private static void AssertExactlyOne(string half, IEnumerable<string> region, string marker)
        {
            var found = region.Count(l => l == marker);
            Assert.True(found == 1, $"{half}: expected exactly ONE '{marker.Trim()}' — found {found}");
        }

        /// <summary>
        /// The front-matter lines: everything strictly between the opening `---` and the next `---`. Splitting
        /// on '\n' only (never trimming) keeps a stray '\r' visible to the caller instead of silently absorbing
        /// it — a consumer parsing the front matter has to see the same bytes we do.
        /// </summary>
        private static List<string> FrontMatterLines(string markdown)
        {
            var lines = new List<string>(markdown.Split('\n'));
            Assert.Equal("---", lines[0]);                       // the document opens the front matter
            var close = lines.IndexOf("---", 1);
            Assert.True(close > 0, "front matter is never closed");
            return lines.GetRange(1, close - 1);
        }

        [Fact]
        public void Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed()
        {
            var gen = new SkillFileGenerator();
            Assert.True(gen.Generate(new List<ToolDescriptor> { DemoTool() }, _root).Success);

            // Assert against the file on DISK — that is the artifact a dedup consumer parses.
            var lines = File.ReadAllText(DemoSkillPath).Split('\n');

            Assert.Equal("---", lines[0]);
            Assert.Equal("name: demo-tool", lines[1]);                     // `name:` unchanged and still FIRST
            Assert.Equal("description: \"Does a demo thing.\"", lines[2]);
            // PRESENCE and POSITION break for different reasons — a marker deleted outright vs one emitted
            // below the closing `---`. The index equality alone discriminates them (-1 vs 6), but a bare -1
            // reads as a puzzle, so the absent case gets its own named message first.
            var markerIndex = Array.IndexOf(lines, MarkerBlockLine);
            Assert.True(markerIndex >= 0, $"'{MarkerBlockLine}' is ABSENT from the generated document entirely");
            Assert.Equal(3, markerIndex);                                  // ...and it sits INSIDE the front matter
            Assert.Equal(MarkerEntryLine, lines[4]);                       // exactly two leading spaces
            Assert.Equal("---", lines[5]);                                 // ...and it is the LAST block inside
        }

        [Fact]
        public void BuildSkillMarkdown_ProvenanceMarkerAppearsExactlyOnce_InTheFrontMatterAndNowhereElse()
        {
            var md = SkillFileGenerator.BuildSkillMarkdown(DemoTool());
            var allLines = md.Split('\n');
            var frontMatter = FrontMatterLines(md);

            // (1) Exactly one marker in the front matter. For this fixture the positional test above entails
            //     this much — but only via Generate_WritesOneSkillPerToolUnderSanitizedFolders, whose
            //     whole-document equality is what ties that test's on-disk file to this builder's output.
            //     Kept as the direct claim on the builder, so weakening that bridge does not silently leave
            //     the builder path unguarded.
            AssertExactlyOne("in-front-matter half", frontMatter, MarkerBlockLine);
            AssertExactlyOne("in-front-matter half", frontMatter, MarkerEntryLine);
            // (2) ...and NOWHERE else in the document. This fixture's own text carries no marker, so a second
            //     occurrence anywhere below the front matter could only be one the generator emitted itself.
            AssertExactlyOne("nowhere-else half", allLines, MarkerBlockLine);
            AssertExactlyOne("nowhere-else half", allLines, MarkerEntryLine);
            // (3) The published constants compose the two literals the assertions above pin. This catches a
            //     constant renamed or revalued out from under those literals; it does NOT prove the generator
            //     READS the constants (a hardcoded emission with the constants intact satisfies everything
            //     here), which C# affords no cheap assertion for.
            Assert.Equal(MarkerBlockLine, SkillFileGenerator.ProvenanceBlockKey + ":");
            Assert.Equal(MarkerEntryLine, "  " + SkillFileGenerator.ProvenanceKey + ": " + SkillFileGenerator.ProvenanceValue);
        }

        [Fact]
        public void BuildSkillMarkdown_DescriptionCannotForgeASecondTopLevelMarker()
        {
            // A tool whose own description carries the marker text at line start. The front-matter description
            // is single-lined and double-quoted, so this text can never escape its scalar — the front matter
            // still holds exactly ONE top-level marker. (The verbatim multi-line text DOES reappear in the body
            // below the closing `---`; the front matter is the only region a marker means anything in, which is
            // why these counts are scoped to it.)
            var forger = new ToolDescriptor
            {
                Name = "forge-tool",
                Title = "Forge Tool",
                Description = "Prose first.\n" + MarkerBlockLine + "\n" + MarkerEntryLine + "\nAnd more prose.",
            };

            var md = SkillFileGenerator.BuildSkillMarkdown(forger);
            var frontMatter = FrontMatterLines(md);

            Assert.Equal(1, frontMatter.Count(l => l == MarkerBlockLine));
            Assert.Equal(1, frontMatter.Count(l => l == MarkerEntryLine));
            // The forged text was flattened into the quoted description scalar rather than dropped.
            Assert.Contains(frontMatter, l => l.StartsWith("description: \"Prose first. " + MarkerBlockLine));
            // Positive control: the marker really is where we expect, so the counts above are not "1 by luck".
            Assert.Equal(MarkerBlockLine, frontMatter[frontMatter.Count - 2]);
            Assert.Equal(MarkerEntryLine, frontMatter[frontMatter.Count - 1]);
        }

        [Fact]
        public void Generate_IsByteStable_AcrossAFreshGeneratorAndAFreshEqualValuedInput()
        {
            // Regeneration must rewrite a byte-identical file: the marker value carries no version and no
            // timestamp. Fresh generator instance AND a freshly constructed (equal-valued) descriptor on each
            // pass, so nothing is shared between them but the values.
            //
            // Generate_WritesOneSkillPerToolUnderSanitizedFolders already compares one written file against a
            // rebuild, so it catches per-emission nondeterminism too. What is new here is the rest: a second
            // generator INSTANCE (no cross-pass state), an overwrite of an existing file rather than a first
            // write, and a BYTE comparison that would also catch an encoding or BOM change.
            var first = new SkillFileGenerator();
            Assert.True(first.Generate(new List<ToolDescriptor> { DemoTool() }, _root).Success);
            var pass1 = File.ReadAllBytes(DemoSkillPath);

            var second = new SkillFileGenerator();
            Assert.True(second.Generate(new List<ToolDescriptor> { DemoTool() }, _root).Success);
            var pass2 = File.ReadAllBytes(DemoSkillPath);

            Assert.Equal(pass1, pass2);
            // Positive control — without it an equality over two MARKER-LESS files would score green. ONE
            // reading is the whole control: past the equality above the two decode to the same string, so a
            // second Contains over pass2 could not fail whatever the generator did.
            Assert.Contains(MarkerEntryLine, Encoding.UTF8.GetString(pass1));
        }

        [Fact]
        public void Generate_WritesAnLfOnlyFrontMatter_SoTheMarkerNeedsNoCrlfNormalisation()
        {
            // The document skeleton is joined with "\n" (not AppendLine / Environment.NewLine), so the FRONT
            // MATTER — the only region a marker means anything in — is LF-terminated on every platform, Windows
            // included: a parser never sees a '\r' glued to the marker value.
            //
            // Scoped to the front matter deliberately. Further down the document the embedded JSON Schema fence
            // is serialized by System.Text.Json with WriteIndented, whose JsonWriterOptions.NewLine defaults to
            // Environment.NewLine — so a generated SKILL.md is MIXED-ending on Windows (LF skeleton, CRLF inside
            // the fence). That is pre-existing and out of scope here; a whole-document "no \r" assertion would
            // pass on Linux and fail on Windows, which is why this asserts the region the claim is about.
            var gen = new SkillFileGenerator();
            Assert.True(gen.Generate(new List<ToolDescriptor> { DemoTool() }, _root).Success);

            var content = File.ReadAllText(DemoSkillPath);
            var closeIndex = content.IndexOf("\n---\n", StringComparison.Ordinal);
            Assert.True(closeIndex > 0, "front matter is never closed with an LF-delimited ---");
            var frontMatterRegion = content.Substring(0, closeIndex + "\n---\n".Length);

            Assert.DoesNotContain("\r", frontMatterRegion);
            Assert.Contains("\n" + MarkerEntryLine + "\n", frontMatterRegion);
        }
    }
}
