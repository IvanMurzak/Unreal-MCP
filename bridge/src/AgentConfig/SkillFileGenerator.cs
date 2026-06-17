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
using System.Text.Json;
using System.Text.Json.Nodes;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig
{
    /// <summary>
    /// Per-agent SKILL.md generator (docs/ARCHITECTURE.md §7) — the SIDECAR-side replacement for the removed C++
    /// FUnrealMcpSkillFileGenerator (issue #101). It writes one <c>&lt;skillsRoot&gt;/&lt;tool-name&gt;/SKILL.md</c>
    /// per tool, sourced ENTIRELY from the tool manifest the C++ plugin already pushes to the sidecar over IPC
    /// (the <see cref="ToolDescriptor"/> catalog the <c>ManifestRegistrar</c> holds: Name / Title / Description /
    /// MCP hints / input+output JSON schema). That is exactly the metadata the old C++ generator read from
    /// <c>FUnrealMcpToolRegistry</c>, so no new C++→sidecar payload is needed — the sidecar already has it.
    ///
    /// Deliberately lives in <c>unreal-mcp-bridge</c>, NOT the shared <c>com.IvanMurzak.McpPlugin.AgentConfig</c>
    /// library: putting it upstream would force another NuGet release, and the SKILL.md shape (the param table
    /// derived from the input schema, the <c>unreal-mcp-cli run-tool</c> call form) is Unreal-specific.
    ///
    /// Output shape (1:1 with the removed C++ generator): YAML front-matter (name + capped single-line description)
    /// → <c># Title</c> → full description body → optional <c>**Hints:**</c> line → <c>## Input</c> (param table +
    /// the input JSON Schema fence) → optional <c>## Output</c> (output schema fence) → <c>## How to Call</c> (the
    /// plain token-free <c>unreal-mcp-cli run-tool</c> form). Idempotent: every SKILL.md is overwritten and stale
    /// generator-owned folders (a SKILL.md whose tool is no longer in the catalog) are pruned. Token discipline
    /// (§8): a skill file is pure tool docs — no token/secret/host/connection value is ever read or written.
    ///
    /// <see cref="BuildSkillMarkdown"/> / <see cref="SanitizeSkillFolderName"/> are pure (no disk) so the xUnit
    /// suite asserts the exact shape for a hand-built descriptor catalog.
    /// </summary>
    public sealed class SkillFileGenerator
    {
        /// <summary>A defensive single-line cap for the YAML <c>description:</c> scalar (mirrors the C++ generator).</summary>
        public const int MaxSkillDescriptionLength = 1024;

        private readonly ILogger? _logger;

        public SkillFileGenerator(ILogger? logger = null)
        {
            _logger = logger;
        }

        /// <summary>Outcome of a generation run (for the IPC result + the panel's status line + the tests).</summary>
        public sealed class Result
        {
            public bool Success { get; set; }
            public int FilesWritten { get; set; }
            public int FilesPruned { get; set; }
            public string SkillsRootAbsolute { get; set; } = string.Empty;
            public string? Error { get; set; }
        }

        /// <summary>
        /// Generate one SKILL.md per tool in <paramref name="tools"/> under <paramref name="skillsRootAbsolute"/>.
        /// Creates the folder tree, overwrites existing files (idempotent), prunes stale generator-owned folders.
        /// Never throws — a write failure is reported in the result. Every tool is documented regardless of its
        /// enabled flag (the docs stay useful even for a tool hidden at runtime).
        /// </summary>
        public Result Generate(IReadOnlyList<ToolDescriptor> tools, string skillsRootAbsolute)
        {
            var result = new Result { SkillsRootAbsolute = skillsRootAbsolute };

            if (string.IsNullOrEmpty(skillsRootAbsolute))
            {
                result.Error = "Skills root path is empty.";
                return result;
            }

            try
            {
                Directory.CreateDirectory(skillsRootAbsolute);
            }
            catch (Exception ex)
            {
                result.Error = $"Could not create skills directory '{skillsRootAbsolute}': {ex.Message}";
                return result;
            }

            // Stable order (the C++ generator sorted by name) so a regenerate is deterministic.
            var ordered = tools.OrderBy(t => t.Name, StringComparer.Ordinal).ToList();
            var currentFolders = new HashSet<string>(StringComparer.Ordinal);
            var written = 0;

            foreach (var tool in ordered)
            {
                var folderName = SanitizeSkillFolderName(tool.Name);
                currentFolders.Add(folderName);
                var dir = Path.Combine(skillsRootAbsolute, folderName);
                var file = Path.Combine(dir, "SKILL.md");
                try
                {
                    Directory.CreateDirectory(dir);
                    // UTF-8 without BOM (portable docs), matching the C++ generator's ForceUTF8WithoutBOM.
                    File.WriteAllText(file, BuildSkillMarkdown(tool), new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
                    written++;
                }
                catch (Exception ex)
                {
                    _logger?.LogWarning("Failed to write skill file '{File}': {Message}", file, ex.Message);
                }
            }

            result.FilesPruned = PruneStaleSkillFolders(skillsRootAbsolute, currentFolders);

            if (written == 0 && ordered.Count > 0)
            {
                result.Error = $"Failed to write any of {ordered.Count} skill file(s) under {skillsRootAbsolute}.";
                return result;
            }

            result.Success = true;
            result.FilesWritten = written;
            _logger?.LogInformation("Generated {Written} skill file(s) (pruned {Pruned} stale) under {Root}.",
                written, result.FilesPruned, skillsRootAbsolute);
            return result;
        }

        /// <summary>
        /// Remove stale generator-owned skill folders under <paramref name="skillsRootAbsolute"/> — immediate child
        /// dirs that own a SKILL.md but whose name is NOT in <paramref name="currentFolderNames"/>. Two guards (must
        /// contain a SKILL.md AND be absent from the current set) keep it from touching unrelated user content.
        /// Returns the count pruned. Never throws.
        /// </summary>
        public static int PruneStaleSkillFolders(string skillsRootAbsolute, ISet<string> currentFolderNames)
        {
            var pruned = 0;
            IEnumerable<string> childDirs;
            try
            {
                childDirs = Directory.EnumerateDirectories(skillsRootAbsolute);
            }
            catch
            {
                return 0;
            }

            foreach (var dir in childDirs)
            {
                var name = Path.GetFileName(dir);
                if (!File.Exists(Path.Combine(dir, "SKILL.md")))
                    continue; // Guard 1: only ever a folder we would have produced ourselves.
                if (currentFolderNames.Contains(name))
                    continue; // Guard 2: still a current tool.
                try
                {
                    Directory.Delete(dir, recursive: true);
                    pruned++;
                }
                catch { /* best-effort; a locked dir is left in place */ }
            }
            return pruned;
        }

        /// <summary>
        /// Sanitize a tool name into a safe folder name: lowercase, alphanumeric runs joined by single hyphens, no
        /// leading/trailing/consecutive hyphens. Empty → "tool". Mirrors the C++ generator 1:1 (tool ids are
        /// already kebab-case, so this is a defensive normalization for odd extension ids).
        /// </summary>
        public static string SanitizeSkillFolderName(string toolName)
        {
            var sb = new StringBuilder(toolName?.Length ?? 0);
            var pendingHyphen = false;
            foreach (var ch in toolName ?? string.Empty)
            {
                var isAlnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
                if (isAlnum)
                {
                    if (pendingHyphen && sb.Length > 0)
                        sb.Append('-');
                    pendingHyphen = false;
                    sb.Append(char.ToLowerInvariant(ch));
                }
                else
                {
                    pendingHyphen = true;
                }
            }
            return sb.Length == 0 ? "tool" : sb.ToString();
        }

        /// <summary>
        /// Build the SKILL.md markdown for ONE tool. Pure (no disk). Reproduces the removed C++
        /// FUnrealMcpSkillFileGenerator::BuildSkillMarkdown shape; the param table + JSON Schema fence are derived
        /// from <see cref="ToolDescriptor.InputSchema"/> (the C++ generator read the same data off its registry).
        /// </summary>
        public static string BuildSkillMarkdown(ToolDescriptor tool)
        {
            var folderName = SanitizeSkillFolderName(tool.Name);
            var yamlDesc = EscapeYamlQuoted(SingleLineCapped(tool.Description ?? string.Empty, MaxSkillDescriptionLength));
            var title = string.IsNullOrEmpty(tool.Title) ? tool.Name : tool.Title!;

            var lines = new List<string>();

            // YAML front-matter (name + description).
            lines.Add("---");
            lines.Add($"name: {folderName}");
            lines.Add($"description: \"{yamlDesc}\"");
            lines.Add("---");
            lines.Add(string.Empty);

            // Title + full description body.
            lines.Add($"# {title}");
            lines.Add(string.Empty);
            if (!string.IsNullOrEmpty(tool.Description))
            {
                lines.Add(tool.Description!.Trim());
                lines.Add(string.Empty);
            }

            // Behavioural hints (MCP read-only / destructive / idempotent / open-world flags).
            var hints = new List<string>();
            if (tool.ReadOnlyHint == true) hints.Add("read-only");
            if (tool.DestructiveHint == true) hints.Add("destructive");
            if (tool.IdempotentHint == true) hints.Add("idempotent");
            if (tool.OpenWorldHint == true) hints.Add("open-world");
            if (hints.Count > 0)
            {
                lines.Add($"**Hints:** {string.Join(", ", hints)}");
                lines.Add(string.Empty);
            }

            // ## Input — a param table + the JSON Schema block, both derived from the input schema.
            lines.Add("## Input");
            lines.Add(string.Empty);
            var paramRows = BuildParamRows(tool.InputSchema);
            if (paramRows.Count == 0)
            {
                lines.Add("This tool takes no parameters.");
                lines.Add(string.Empty);
            }
            else
            {
                lines.Add("| Name | Type | Required | Description |");
                lines.Add("| --- | --- | --- | --- |");
                lines.AddRange(paramRows);
                lines.Add(string.Empty);
                lines.Add("### Input JSON Schema");
                lines.Add(string.Empty);
                lines.Add("```json");
                lines.Add(PrettyJson(tool.InputSchema));
                lines.Add("```");
                lines.Add(string.Empty);
            }

            // ## Output — the structured output schema when the tool declares one.
            if (tool.OutputSchema != null)
            {
                lines.Add("## Output");
                lines.Add(string.Empty);
                lines.Add("### Output JSON Schema");
                lines.Add(string.Empty);
                lines.Add("```json");
                lines.Add(PrettyJson(tool.OutputSchema));
                lines.Add("```");
                lines.Add(string.Empty);
            }

            // ## How to Call — the plain unreal-mcp-cli form. Token discipline (§8): NO token at all.
            lines.Add("## How to Call");
            lines.Add(string.Empty);
            lines.Add("```bash");
            lines.Add($"unreal-mcp-cli run-tool {tool.Name} --input '{{ ... }}'");
            lines.Add("```");
            lines.Add(string.Empty);

            return string.Join("\n", lines);
        }

        // --- Input-schema → param table -------------------------------------------------------------

        /// <summary>
        /// Derive the param table rows (Name / Type / Required / Description) from a JSON-Schema object's
        /// <c>properties</c> + <c>required</c>. The C++ generator read these off its declared param specs; the
        /// sidecar reads the equivalent from the schema the plugin already serialized into the manifest. Returns an
        /// empty list when the schema has no properties (→ "This tool takes no parameters.").
        /// </summary>
        internal static List<string> BuildParamRows(JsonNode? inputSchema)
        {
            var rows = new List<string>();
            if (inputSchema is not JsonObject schema)
                return rows;
            if (schema["properties"] is not JsonObject props || props.Count == 0)
                return rows;

            var required = new HashSet<string>(StringComparer.Ordinal);
            if (schema["required"] is JsonArray reqArr)
                foreach (var r in reqArr)
                    if (r is JsonValue rv && rv.TryGetValue<string>(out var rs))
                        required.Add(rs);

            foreach (var (name, node) in props)
            {
                var type = "object";
                var desc = string.Empty;
                if (node is JsonObject prop)
                {
                    if (prop["type"] is JsonValue tv && tv.TryGetValue<string>(out var ts))
                        type = ts;
                    else if (prop["type"] is JsonArray) // union type (e.g. ["string","null"])
                        type = string.Join("|", ((JsonArray)prop["type"]!).Select(n => n?.GetValue<string>() ?? "?"));
                    if (prop["description"] is JsonValue dv && dv.TryGetValue<string>(out var ds))
                        desc = ds;
                }
                // Keep a stray pipe / newline in a description from breaking the markdown table.
                desc = desc.Replace("|", "\\|").Replace("\r", " ").Replace("\n", " ");
                var req = required.Contains(name) ? "yes" : "no";
                rows.Add($"| `{name}` | {type} | {req} | {desc} |");
            }
            return rows;
        }

        // --- Formatting helpers (mirror the C++ generator) ------------------------------------------

        internal static string SingleLineCapped(string input, int maxLen)
        {
            var flat = (input ?? string.Empty)
                .Replace("\r\n", " ").Replace("\n", " ").Replace("\r", " ").Replace("\t", " ");
            while (flat.Contains("  "))
                flat = flat.Replace("  ", " ");
            flat = flat.Trim();

            if (flat.Length <= maxLen)
                return flat;

            var capped = flat.Substring(0, maxLen);
            var lastSpace = capped.LastIndexOf(' ');
            if (lastSpace > maxLen / 2)
                capped = capped.Substring(0, lastSpace);
            return capped.TrimEnd() + "...";
        }

        private static string EscapeYamlQuoted(string input) =>
            (input ?? string.Empty).Replace("\\", "\\\\").Replace("\"", "\\\"");

        private static string PrettyJson(JsonNode? node)
        {
            if (node == null)
                return "{}";
            // Match the C++ generator's pretty (indented) JSON block; deep-clone first so serialization never
            // mutates / re-parents the node the registrar still holds.
            return node.DeepClone().ToJsonString(new JsonSerializerOptions { WriteIndented = true });
        }
    }
}
