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
using SharedSkillFileGenerator = com.IvanMurzak.McpPlugin.Skills.SkillFileGenerator;

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
    /// Output shape (1:1 with the removed C++ generator, plus the provenance marker): YAML front-matter (name +
    /// capped single-line description + the <c>metadata: / generated-by:</c> marker that identifies the file as
    /// generated rather than hand-authored — see <see cref="ProvenanceKey"/>)
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

        /// <summary>
        /// YAML front-matter key holding the provenance block. In the Skills file format used by Codex (and
        /// Anthropic Agent Skills) <c>metadata</c> is a declared front-matter field carrying a free-form mapping
        /// of extra keys, which is why the marker nests under it rather than sitting as a bare top-level key a
        /// host would simply ignore. (That framing is the shared library's own, restated here rather than
        /// re-derived — nothing in this repository pins the host format.)
        /// <para>
        /// This and the two constants below are <c>public</c> to mirror the shared library, which publishes the
        /// same three — so a later unification has a name-for-name seam. They are not consumed outside this
        /// class today; the tests reach them through <c>InternalsVisibleTo</c> either way.
        /// </para>
        /// </summary>
        public const string ProvenanceBlockKey = "metadata";

        /// <summary>
        /// Key, nested under <see cref="ProvenanceBlockKey"/>, marking a SKILL.md as written by a generator
        /// rather than hand-authored by a user. It exists so a consumer that dedups generated skills against
        /// the live tool catalog can tell the two apart and leave user-authored files alone; such a consumer
        /// is expected to be fail-open (an unmarked file is kept). No consumer exists in this repository yet,
        /// so the marker is currently inert here — and note that <see cref="PruneStaleSkillFolders"/> is NOT
        /// one: it deletes on the presence of a SKILL.md, not on this marker.
        /// <para>
        /// Matching a marked file back to a catalog entry relies on the front matter's <c>name:</c> — see
        /// <see cref="SanitizeSkillFolderName"/>, which is the identity on the kebab-case ids the registry
        /// validates and lossy on anything else.
        /// </para>
        /// </summary>
        public const string ProvenanceKey = "generated-by";

        /// <summary>
        /// Value written for <see cref="ProvenanceKey"/>. Deliberately carries NO version and NO timestamp, so
        /// regenerating an unchanged tool rewrites a byte-identical file.
        /// <para>
        /// The same two lines that <c>com.IvanMurzak.McpPlugin.Skills.SkillFileGenerator</c> emits — verified
        /// against the MCP-Plugin-dotnet source at the time of writing, and NOT present in the released 8.3.0
        /// this bridge pins, so nothing here fails if the shared value drifts. Re-check at the next McpPlugin
        /// bump, and prefer emitting from the shared constants once a release carries them.
        /// </para>
        /// <para>
        /// Same line CONTENT, not the same bytes: the shared generator uses <c>AppendLine</c>, so its
        /// terminator is <c>Environment.NewLine</c>, while this one joins with <c>"\n"</c>. A consumer must
        /// therefore match the marker per line, never as one raw byte run.
        /// </para>
        /// </summary>
        public const string ProvenanceValue = "unreal-mcp";

        private readonly ILogger? _logger;
        private readonly IReadOnlyDictionary<string, string>? _metadata;

        public SkillFileGenerator(ILogger? logger = null, IReadOnlyDictionary<string, string>? metadata = null)
        {
            _logger = logger;
            _metadata = metadata;
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
        ///
        /// <para>
        /// SYSTEM tools are the ONE exclusion (docs/ARCHITECTURE.md §2.4): a skill file is agent-facing
        /// documentation, and a system tool is by definition one an AI agent must never call (`ping`,
        /// `unreal-skill-create`). Emitting a SKILL.md for one would re-expose it through the skills channel,
        /// defeating the surface split. They are excluded HERE, in the single place both callers (the editor
        /// panel's generate button and the `unreal-skill-generate` system tool) funnel through, so the rule
        /// cannot drift between them — and an existing system-tool skill folder from before §2.4 is pruned as
        /// stale on the next run.
        /// </para>
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

            // Stable order (the C++ generator sorted by name) so a regenerate is deterministic. System tools are
            // filtered out first — see the §2.4 note on this method.
            var ordered = tools.Where(t => !t.IsSystem).OrderBy(t => t.Name, StringComparer.Ordinal).ToList();
            var currentFolders = new HashSet<string>(StringComparer.Ordinal);
            var written = 0;

            foreach (var tool in ordered)
            {
                var folderName = SanitizeSkillFolderName(tool.Name);
                currentFolders.Add(folderName);
                if (WriteSkill(skillsRootAbsolute, folderName, BuildSkillMarkdown(tool, _metadata)))
                    written++;
                // Skills the tool ships beside its page (rql-query: unreal-rql, docs/rql.md), named and written by the plugin.
                foreach (var skill in tool.Skills ?? new List<ToolSkillDescriptor>())
                {
                    if (string.IsNullOrWhiteSpace(skill.Name) || string.IsNullOrWhiteSpace(skill.Markdown))
                        continue;
                    var skillFolder = SanitizeSkillFolderName(skill.Name);
                    currentFolders.Add(skillFolder);
                    if (WriteSkill(skillsRootAbsolute, skillFolder, BuildShippedSkillMarkdown(skill, _metadata)))
                        written++;
                }
            }

            // NEVER prune on an empty document set. `currentFolders` is empty here, so the prune below would
            // delete EVERY skill folder under the root. Two ways to reach it, and §2.4 made the second one
            // routine: (a) the plugin has not pushed a manifest yet (the catalog is genuinely empty), and
            // (b) every tool in the manifest is a SYSTEM tool — which is exactly the runtime (in-game) case,
            // where the built-in surface is `ping` ALONE and `ping` is now a system tool. Before §2.4 `ordered`
            // was `[ping]` there and the wipe was unreachable. Bailing out keeps Success=false, so the caller
            // is told nothing happened instead of being handed "generated 0 (pruned N)" as a success.
            if (ordered.Count == 0)
            {
                result.Error = tools.Count == 0
                    ? "No tools in the catalog yet (the plugin has not pushed a tool manifest) — nothing generated, nothing pruned."
                    : $"All {tools.Count} tool(s) in the catalog are SYSTEM tools (§2.4), which are never documented — nothing generated, nothing pruned.";
                return result;
            }

            foreach (var skill in BuiltinSkillGuides.All())
            {
                currentFolders.Add(skill.Name);
                if (WriteSkill(skillsRootAbsolute, skill.Name, BuildShippedSkillMarkdown(skill, _metadata)))
                    written++;
                else
                {
                    result.Error = $"Failed to write the standalone '{skill.Name}' skill; references would be incomplete.";
                    return result;
                }
            }

            result.FilesPruned = PruneStaleSkillFolders(skillsRootAbsolute, currentFolders);

            if (written == 0)
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

        /// <summary>Write <c>&lt;root&gt;/&lt;folder&gt;/SKILL.md</c> (UTF-8 without BOM, as the C++ generator did). Never throws.</summary>
        private bool WriteSkill(string skillsRootAbsolute, string folderName, string markdown)
        {
            var dir = Path.Combine(skillsRootAbsolute, folderName);
            var file = Path.Combine(dir, "SKILL.md");
            try
            {
                Directory.CreateDirectory(dir);
                File.WriteAllText(file, markdown, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
                return true;
            }
            catch (Exception ex)
            {
                _logger?.LogWarning("Failed to write skill file '{File}': {Message}", file, ex.Message);
                return false;
            }
        }

        /// <summary>
        /// The YAML front matter of a generated SKILL.md: <c>name</c>, the capped single-line <c>description</c>, then the
        /// provenance marker (see <see cref="ProvenanceKey"/>), followed by the blank line before the body. The marker is
        /// the LAST block INSIDE the front matter (a consumer parses the front matter only), indented two spaces (a nested
        /// mapping) and written RAW from the constants, never through <see cref="EscapeYamlQuoted"/> (which would collapse
        /// the mapping into a quoted string). The description cannot break out of its scalar to forge it — see
        /// <see cref="SingleLineCapped"/> for the mechanism and its limits.
        /// </summary>
        private static List<string> FrontMatter(string name, string description, IReadOnlyDictionary<string, string>? metadata = null)
        {
            var entries = new Dictionary<string, string> { [ProvenanceKey] = ProvenanceValue };
            if (metadata != null)
                foreach (var entry in metadata) entries[entry.Key] = entry.Value;
            var yaml = new StringBuilder();
            SharedSkillFileGenerator.AppendMetadata(yaml, entries);
            var lines = new List<string>
            {
                "---", $"name: {name}",
                $"description: \"{EscapeYamlQuoted(SingleLineCapped(description, MaxSkillDescriptionLength))}\"",
            };
            lines.AddRange(yaml.ToString().Replace("\r\n", "\n").TrimEnd('\n').Split('\n'));
            lines.Add("---");
            lines.Add(string.Empty);
            return lines;
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
        public static string BuildSkillMarkdown(ToolDescriptor tool, IReadOnlyDictionary<string, string>? metadata = null)
        {
            var folderName = SanitizeSkillFolderName(tool.Name);
            // YAML `description:` ← the dedicated short skill description ([AiSkillDescription] analog the manifest
            // carries as `skillDescription`) when present, else FALL BACK to the full Description (single-lined +
            // capped). The body below always uses the full Description. This matches the operator's intent and is
            // forward-compatible: the moment the C++ manifest starts emitting skillDescription, the front-matter
            // uses it without any further change here.
            var shortDesc = !string.IsNullOrWhiteSpace(tool.SkillDescription) ? tool.SkillDescription! : (tool.Description ?? string.Empty);
            var title = string.IsNullOrEmpty(tool.Title) ? tool.Name : tool.Title!;

            var lines = FrontMatter(folderName, shortDesc, metadata);

            // Title + the body: the dedicated skill body ([AiSkillBody] analog the manifest carries as `skillBody`)
            // when present, else the full Description. The shared lib's [AiSkillBody]/[AiSkillDescription] split is
            // mirrored here so a tool that authors a richer skill body uses it, with a clean fallback otherwise.
            lines.Add($"# {title}");
            lines.Add(string.Empty);
            var body = !string.IsNullOrWhiteSpace(tool.SkillBody) ? tool.SkillBody! : (tool.Description ?? string.Empty);
            if (!string.IsNullOrEmpty(body))
            {
                lines.Add(body.Trim());
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
            lines.Add(BuiltinSkillGuides.References);

            return string.Join("\n", lines);
        }

        /// <summary>
        /// A skill a tool ships (<see cref="ToolDescriptor.Skills"/>, e.g. rql-query's <c>unreal-rql</c>): the generated front
        /// matter from its name and description, then its markdown as the plugin composed it. Pure (no disk).
        /// </summary>
        public static string BuildShippedSkillMarkdown(ToolSkillDescriptor skill, IReadOnlyDictionary<string, string>? metadata = null)
        {
            var lines = FrontMatter(SanitizeSkillFolderName(skill.Name), skill.Description ?? string.Empty, metadata);
            lines.Add((skill.Markdown ?? string.Empty).Trim());
            lines.Add(string.Empty);
            if (skill.Name != BuiltinSkillGuides.CliSkillName && skill.Name != BuiltinSkillGuides.AppSkillName)
                lines.Add(BuiltinSkillGuides.References);
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

        /// <summary>
        /// Flatten a description to a single capped line for the YAML <c>description:</c> scalar.
        /// <para>
        /// Load-bearing beyond formatting: flattening CR/LF/tab is what stops a tool description from spanning
        /// several front-matter lines and forging a second top-level <c>metadata:</c> block (see
        /// <see cref="ProvenanceKey"/>). That guarantee is scoped to a real YAML parser — this does NOT
        /// neutralise U+0085 / U+2028 / U+2029 or other C0 controls, which a purely line-oriented reader (a
        /// regex under the <c>m</c> flag) would treat as line breaks. Long-standing behaviour; if you tighten
        /// it, the marker's forgery guard is what you are strengthening.
        /// </para>
        /// </summary>
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
