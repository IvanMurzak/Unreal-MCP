/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The `unreal-skill-generate` SYSTEM tool (docs/ARCHITECTURE.md §2.4) — Unreal's
    /// `unity-skill-generate`: regenerate every `SKILL.md` from the currently-registered tool set into the
    /// project's skills folder.
    ///
    /// <para>
    /// <b>Why this one lives in the SIDECAR while its sibling `unreal-skill-create` lives in C++.</b> The
    /// SKILL.md generator was deliberately moved out of C++ and into the sidecar (issue #101,
    /// <see cref="SkillFileGenerator"/>): the sidecar already holds the full descriptor catalog the plugin
    /// pushes over the manifest, so it needs no extra C++→sidecar payload. A C++-homed `generate` tool would
    /// have to ask the sidecar to do the work and — because a tool handler runs on the game thread and MUST NOT
    /// synchronously wait on bridge state (§4, §11 risk 2) — could only fire the request and return before it
    /// finished, i.e. report a completion it had not observed. Hosting the tool where the work happens keeps
    /// the result honest. `unreal-skill-create` is the opposite case: it writes plugin C++ and needs the
    /// editor's own view of the install, so it stays in C++.
    /// </para>
    /// <para>
    /// Registered at BUILD time via <c>McpPluginBuilder.AddTool</c>: the builder partitions declared runners by
    /// <see cref="IRunTool.ToolType"/>, so a <see cref="McpToolType.System"/> proxy lands in the
    /// <c>SystemToolRunnerCollection</c> with no post-build mutation at all. The catalog + project root are
    /// supplied as late-bound callbacks because both are only known after the IPC handshake.
    /// </para>
    /// </summary>
    public static class SkillGenerateTool
    {
        /// <summary>The tool id, mirroring Unity's `unity-skill-generate` with the engine prefix swapped.</summary>
        public const string ToolId = "unreal-skill-generate";

        /// <summary>
        /// Where skills go when the caller names no <c>path</c>. Matches the agent-config library's default for
        /// Claude Code, which is the overwhelmingly common target; any other location is one `path` away.
        /// </summary>
        public const string DefaultSkillsFolder = ".claude/skills";

        /// <summary>
        /// Build the system-tool proxy. <paramref name="toolCatalog"/> returns the live manifest descriptors
        /// (null/empty before the first manifest arrives) and <paramref name="projectRoot"/> the handshake's
        /// project path; both are callbacks because neither exists when the plugin is built.
        /// </summary>
        public static ProxyTool Create(
            Func<IReadOnlyList<ToolDescriptor>?> toolCatalog,
            Func<string?> projectRoot,
            SkillFileGenerator generator)
        {
            if (toolCatalog == null) throw new ArgumentNullException(nameof(toolCatalog));
            if (projectRoot == null) throw new ArgumentNullException(nameof(projectRoot));
            if (generator == null) throw new ArgumentNullException(nameof(generator));

            return new ProxyTool(
                name: ToolId,
                title: "Skill (Tool) / Generate All",
                description:
                    "Regenerate every SKILL.md from the Unreal project's currently-registered MCP tools into the " +
                    "skills folder. 'path' is a PROJECT-RELATIVE folder (e.g. '.claude/skills', the default); " +
                    "absolute paths and '..' traversal are rejected. Existing files are overwritten and stale " +
                    "generator-owned folders are pruned, so the run is idempotent. System tools (ping, " +
                    "unreal-skill-create, unreal-skill-generate) are deliberately NOT documented — a skill file is " +
                    "agent-facing and those are host plumbing.",
                skillDescription:
                    "Regenerate every SKILL.md from the Unreal project's registered MCP tools into a " +
                    "project-relative skills folder (default '.claude/skills'). Idempotent: overwrites existing " +
                    "files and prunes stale ones.",
                skillBody: null,
                inputSchema: BuildInputSchema(),
                outputSchema: null,
                readOnlyHint: false,
                // Destructive per the MCP definition ("may perform destructive updates ... deleting data,
                // overwriting files"): every run overwrites each SKILL.md and recursively deletes stale
                // generator-owned folders. Idempotent still holds — the same catalog yields the same tree.
                destructiveHint: true,
                idempotentHint: true,
                openWorldHint: false,
                handler: (requestId, namedParameters, cancellationToken) =>
                    Task.FromResult(Run(toolCatalog, projectRoot, generator, namedParameters).SetRequestID(requestId)),
                toolType: McpToolType.System);
        }

        /// <summary>The `{ path?: string }` input schema. Separate + pure so the tests assert its exact shape.</summary>
        public static JsonNode BuildInputSchema() => new JsonObject
        {
            ["type"] = "object",
            ["properties"] = new JsonObject
            {
                ["path"] = new JsonObject
                {
                    ["type"] = "string",
                    ["description"] =
                        $"Project-relative skills folder. Defaults to '{DefaultSkillsFolder}'. " +
                        "Absolute paths and '..' traversal segments are rejected.",
                },
            },
        };

        /// <summary>
        /// The tool body, factored out so the xUnit suite drives it directly with a temp folder and a hand-built
        /// catalog. Never throws — every failure becomes a structured error response.
        /// </summary>
        public static ResponseCallTool Run(
            Func<IReadOnlyList<ToolDescriptor>?> toolCatalog,
            Func<string?> projectRoot,
            SkillFileGenerator generator,
            IReadOnlyDictionary<string, JsonElement>? namedParameters)
        {
            var path = ReadStringArgument(namedParameters, "path");
            if (string.IsNullOrWhiteSpace(path))
                path = DefaultSkillsFolder;

            if (Path.IsPathRooted(path))
            {
                return ResponseCallTool.Error(
                    "'path' must be a project-relative folder (e.g. '.claude/skills'). Absolute paths are not allowed.",
                    ResponseErrorKind.BadRequest);
            }

            var normalized = path!.Replace('\\', '/');
            if (normalized.Contains("../") || normalized.EndsWith("..", StringComparison.Ordinal))
            {
                return ResponseCallTool.Error(
                    "'path' must not contain '..' traversal segments. Use a path relative to the project root.",
                    ResponseErrorKind.BadRequest);
            }

            var root = projectRoot();
            if (string.IsNullOrWhiteSpace(root))
            {
                return ResponseCallTool.Error(
                    "The project root is not known yet — the editor has not completed the IPC handshake. " +
                    "Connect the Unreal editor and retry.",
                    ResponseErrorKind.Conflict);
            }

            var skillsRoot = AgentConfigService.ResolveAbsolute(root!, normalized);
            var tools = toolCatalog() ?? Array.Empty<ToolDescriptor>();

            var generated = generator.Generate(tools, skillsRoot);
            if (!generated.Success)
            {
                return ResponseCallTool.Error(
                    generated.Error ?? $"Skill generation failed under '{skillsRoot}'.",
                    ResponseErrorKind.Internal);
            }

            var response = ResponseCallTool.Success(
                $"Generated {generated.FilesWritten} skill file(s) (pruned {generated.FilesPruned} stale) under '{skillsRoot}'.");
            response.StructuredContent = new JsonObject
            {
                ["skillsPath"] = generated.SkillsRootAbsolute,
                ["filesWritten"] = generated.FilesWritten,
                ["filesPruned"] = generated.FilesPruned,
                ["toolsConsidered"] = tools.Count,
            };
            return response;
        }

        private static string? ReadStringArgument(IReadOnlyDictionary<string, JsonElement>? namedParameters, string key)
        {
            if (namedParameters == null || !namedParameters.TryGetValue(key, out var element))
                return null;
            return element.ValueKind == JsonValueKind.String ? element.GetString() : null;
        }
    }
}
