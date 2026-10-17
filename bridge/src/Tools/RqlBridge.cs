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
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.McpPlugin.Common.Model;
using Rql;
using Rql.Execution;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The sidecar half of the RQL data layer (docs/rql.md). The plugin's C++ RQL runtime executes QueryPlans and has no
    /// text parser by design (RQL ADR-0003), so for <c>rql-query</c> / <c>rql-mutate</c> this class parses the agent's
    /// <c>query</c> text with <see cref="RqlParser"/> and adds the canonical plan as the <c>plan</c> argument before the
    /// call goes over IPC. The plugin answers with the QueryResult JSON (rql-schema: schema JSON) as its text content,
    /// which is rendered here into RQL's compact agent format (<see cref="AgentFormat"/>) — the same output Unity-MCP's
    /// rql tools produce.
    /// </summary>
    public static class RqlBridge
    {
        public const string QueryTool = "rql-query";
        public const string MutateTool = "rql-mutate";
        public const string SchemaTool = "rql-schema";
        public const string QueryArgument = "query";
        public const string PlanArgument = "plan";

        /// <summary>The skill the rql-query tool's <c>skillBody</c> carries (<see cref="AgentConfig.SkillFileGenerator"/>).</summary>
        public const string SkillName = "unreal-rql";

        /// <summary>The unreal-rql skill's front-matter description (Unity-MCP's unity-rql skill, for Unreal).</summary>
        public const string SkillDescription =
            "How to read and change anything in the Unreal Editor with RQL (rql-query / rql-mutate / rql-schema): the query " +
            "language, operators, sources (actors, components, assets, levels, console logs, editor state), editor operations " +
            "(spawn actors, add components, levels, assets, selection, Play In Editor) and what installed extensions add.";

        public static bool IsQueryTool(string toolName) => toolName == QueryTool || toolName == MutateTool;

        public static bool IsRqlTool(string toolName) => IsQueryTool(toolName) || toolName == SchemaTool;

        /// <summary>
        /// Parse <c>arguments.query</c> and store its plan as <c>arguments.plan</c>. Returns null on success, or the error
        /// response for the agent (rendered like any failing query) when the text does not parse.
        /// </summary>
        public static ResponseCallTool? AddPlan(JsonObject arguments)
        {
            var text = arguments[QueryArgument] is JsonValue value && value.TryGetValue<string>(out var s) ? s : null;
            if (string.IsNullOrWhiteSpace(text))
                return Failure(Diagnostic.Error("invalid-plan", "'query' is required: an RQL query such as actors | first 5"));
            if (!RqlParser.TryParse(text, out var plan, out var error))
                return Failure(error ?? Diagnostic.Error("syntax-error", "the query does not parse"));
            arguments[PlanArgument] = PlanCodec.Serialize(plan!);
            return null;
        }

        /// <summary>
        /// The plugin's answer for an RQL tool as agent text, keeping its status. An answer that is not RQL JSON (an IPC
        /// error, a disconnected bridge) is returned unchanged.
        /// </summary>
        public static ResponseCallTool Render(string toolName, ResponseCallTool response)
        {
            var text = response.Content?.FirstOrDefault(c => c.Type == Consts.ContentType.Text)?.Text;
            if (string.IsNullOrEmpty(text))
                return response;
            JsonObject? json;
            try { json = JsonNode.Parse(text!) as JsonObject; }
            catch (JsonException) { return response; }
            if (json == null)
                return response;

            string rendered;
            if (toolName == SchemaTool)
                rendered = RenderSchema(json);
            else if (json.ContainsKey("ok"))
                rendered = AgentFormat.Render(ParseResult(json), OutputFormat.Table);
            else
                return response;

            response.Content = new List<ContentBlock> { new ContentBlock { Type = Consts.ContentType.Text, Text = rendered } };
            response.StructuredContent = null;
            return response;
        }

        private static ResponseCallTool Failure(Diagnostic diagnostic) =>
            ResponseCallTool.Error(AgentFormat.Render(QueryResult.Fail(new[] { diagnostic }), OutputFormat.Table));

        // ------------------------------------------------------------ QueryResult JSON (spec form) -> QueryResult

        /// <summary>The C++ runtime's QueryResult JSON (spec/query-result.schema.json) as RQL's .NET result.</summary>
        public static QueryResult ParseResult(JsonObject json)
        {
            var result = new QueryResult { Ok = Bool(json, "ok") };
            if (json["kind"] is JsonValue kind && kind.TryGetValue<string>(out var k))
            {
                result.Kind = k switch
                {
                    "rows" => ResultKind.Rows,
                    "items" => ResultKind.Items,
                    "scalar" => ResultKind.Scalar,
                    "schema" => ResultKind.Schema,
                    _ => (ResultKind?)null,
                };
            }
            if (json["columns"] is JsonArray columns)
                result.Columns = columns.Select(c => c?.GetValue<string>() ?? "").ToList();
            if (json["rows"] is JsonArray rows)
                result.Rows = rows.Select(r => r is JsonArray cells ? cells.Select(c => c?.DeepClone()).ToArray() : Array.Empty<JsonNode?>()).ToList();
            if (json["items"] is JsonArray items)
                result.Items = items.Select(i => i?.DeepClone()).ToList();
            if (json.TryGetPropertyValue("value", out var value))
            {
                result.HasValue = true;
                result.Value = value?.DeepClone();
            }
            if (json["types"] is JsonArray types)
                result.Types = (JsonArray)types.DeepClone();
            if (json["total"] is JsonValue total && total.TryGetValue<long>(out var t))
                result.Total = t;
            result.Truncated = Bool(json, "truncated");
            result.DryRun = Bool(json, "dryRun");
            if (json["diagnostics"] is JsonArray diagnostics)
            {
                foreach (var node in diagnostics.OfType<JsonObject>())
                {
                    TextSpan? at = null;
                    if (node["at"] is JsonObject span && span["start"] is JsonValue start && span["end"] is JsonValue end)
                        at = new TextSpan(start.GetValue<int>(), end.GetValue<int>());
                    result.Diagnostics.Add(new Diagnostic(
                        Str(node, "severity") == "warning" ? DiagnosticSeverity.Warning : DiagnosticSeverity.Error,
                        Str(node, "code") ?? "internal-error", Str(node, "message") ?? "", at, Str(node, "hint")));
                }
            }
            return result;
        }

        // ------------------------------------------------------------ schema JSON -> compact text

        /// <summary>
        /// rql-schema's text, in the compact lines of RQL's own schema (<c>Name : Base {member type ro, …} methods: …</c>) as
        /// Unity-MCP prints it: without a type, the sources, functions, the classes present in the editor world and the
        /// Actor / SceneComponent members; with one, that type (members shared with Actor / ActorComponent summarised).
        /// </summary>
        public static string RenderSchema(JsonObject json)
        {
            if (Str(json, "error") == "unknown-type")
            {
                var candidates = (json["candidates"] as JsonArray)?.Select(c => c?.GetValue<string>() ?? "").Where(c => c.Length > 0).ToList()
                    ?? new List<string>();
                return AgentFormat.UnknownTypeError(Str(json, "name") ?? "", candidates);
            }

            var sb = new StringBuilder();
            if (json["sources"] is JsonArray sources)
            {
                sb.Append("sources:\n");
                foreach (var s in sources.OfType<JsonObject>())
                {
                    sb.Append("  ").Append(Str(s, "name"));
                    var type = Str(s, "type");
                    if (!string.IsNullOrEmpty(type)) sb.Append(": ").Append(type);
                    var description = Str(s, "description");
                    if (!string.IsNullOrEmpty(description)) sb.Append(" — ").Append(description);
                    sb.Append('\n');
                }
            }
            if (json["functions"] is JsonArray functions && functions.Count > 0)
            {
                sb.Append("functions:\n");
                foreach (var f in functions.OfType<JsonObject>().OrderBy(f => Str(f, "name"), StringComparer.Ordinal))
                    sb.Append("  ").Append(Str(f, "name")).Append("(…) — ").Append(Str(f, "description")).Append('\n');
            }
            AppendPresent(sb, json["actorClasses"] as JsonArray, "actor classes in the editor world");
            AppendPresent(sb, json["componentClasses"] as JsonArray, "component classes in the editor world");
            if (json["sources"] is JsonArray)
                sb.Append("types (members: name type; ro = read-only; rql-schema \"<type>\" for one type):\n");

            if (json["types"] is JsonArray types)
            {
                foreach (var entry in types.OfType<JsonObject>())
                {
                    if (!(entry["type"] is JsonObject type)) continue;
                    var shared = entry["shared"] as JsonObject;
                    AppendType(sb, type, shared);
                    if (shared != null)
                    {
                        var own = new HashSet<string>(MemberNames(type), StringComparer.Ordinal);
                        var inherited = MemberNames(shared).Where(own.Contains).ToList();
                        if (inherited.Count > 0)
                            sb.Append("  (+ inherited from ").Append(Str(shared, "name")).Append(": ").Append(string.Join(", ", inherited)).Append(")\n");
                    }
                }
            }
            return sb.ToString().TrimEnd();
        }

        private static void AppendPresent(StringBuilder sb, JsonArray? present, string title)
        {
            if (present == null) return;
            sb.Append(title).Append(": ");
            sb.Append(present.Count == 0
                ? "none"
                : string.Join(", ", present.OfType<JsonObject>().Select(p => Str(p, "type") + " ×" + (p["count"]?.ToJsonString() ?? "?"))));
            sb.Append('\n');
        }

        /// <summary>One type: <c>[struct ]Name : Base {member type [ro] [explicit-only] (description), …} methods: … — description</c>.</summary>
        private static void AppendType(StringBuilder sb, JsonObject type, JsonObject? shared)
        {
            var name = Str(type, "name") ?? "?";
            var kind = Str(type, "kind");
            if (kind == "enum")
            {
                var values = (type["values"] as JsonArray)?.Select(v => v?.GetValue<string>() ?? "") ?? Enumerable.Empty<string>();
                sb.Append("enum ").Append(name).Append(": ").Append(string.Join(" ", values)).Append('\n');
                return;
            }
            sb.Append(kind == "struct" ? "struct " : "").Append(name);
            var baseName = Str(type, "base");
            if (!string.IsNullOrEmpty(baseName)) sb.Append(" : ").Append(baseName);
            sb.Append(" {");
            var skip = new HashSet<string>(shared == null ? Enumerable.Empty<string>() : MemberNames(shared), StringComparer.Ordinal);
            var parts = new List<string>();
            foreach (var member in (type["members"] as JsonArray)?.OfType<JsonObject>() ?? Enumerable.Empty<JsonObject>())
            {
                var memberName = Str(member, "name") ?? "?";
                if (skip.Contains(memberName)) continue;
                var part = memberName + " " + Str(member, "type");
                if (!Bool(member, "writable")) part += " ro";
                if (Bool(member, "explicitOnly")) part += " explicit-only";
                var description = Str(member, "description");
                if (!string.IsNullOrEmpty(description)) part += " (" + description + ")";
                parts.Add(part);
            }
            sb.Append(string.Join(", ", parts)).Append('}');
            if (type["methods"] is JsonArray methods && methods.Count > 0)
            {
                var inheritedMethods = new HashSet<string>(shared?["methods"] is JsonArray sm ? sm.Select(m => m?.GetValue<string>() ?? "") : Enumerable.Empty<string>());
                var own = methods.Select(m => m?.GetValue<string>() ?? "").Where(m => !inheritedMethods.Contains(m)).ToList();
                if (own.Count > 0) sb.Append(shared != null ? " +methods: " : " methods: ").Append(string.Join(" | ", own));
            }
            var typeDescription = Str(type, "description");
            if (!string.IsNullOrEmpty(typeDescription)) sb.Append(" — ").Append(typeDescription);
            sb.Append('\n');
        }

        private static IEnumerable<string> MemberNames(JsonObject type) =>
            (type["members"] as JsonArray)?.OfType<JsonObject>().Select(m => Str(m, "name") ?? "") ?? Enumerable.Empty<string>();

        private static string? Str(JsonObject o, string key) => o[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;

        private static bool Bool(JsonObject o, string key) => o[key] is JsonValue v && v.TryGetValue<bool>(out var b) && b;
    }
}
