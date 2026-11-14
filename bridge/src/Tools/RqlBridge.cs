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
using System.Threading.Tasks;
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
        /// <summary>The <see cref="ToolDescriptor.Rql"/> roles the plugin declares on its rql-* tools.</summary>
        public const string QueryRole = "query";
        public const string MutateRole = "mutate";
        public const string SchemaRole = "schema";
        public const string QueryArgument = "query";
        public const string PlanArgument = "plan";

        /// <summary>Whether a tool of this RQL role takes RQL text that the sidecar parses into a plan.</summary>
        public static bool ParsesQuery(string? role) => role == QueryRole || role == MutateRole;

        /// <summary>
        /// Parse <c>arguments.query</c> and store its plan as <c>arguments.plan</c>. Returns null on success, or the error
        /// response for the agent (rendered like any failing query) when the text does not parse. Text of several statements
        /// separated by <c>;</c> (RQL spec §14) gets no plan here: <paramref name="statements"/> holds them, and
        /// <see cref="RunStatementsAsync"/> parses and sends each one as a call of its own. One statement (the usual case) is
        /// handled exactly as before and leaves <paramref name="statements"/> null.
        /// </summary>
        public static ResponseCallTool? AddPlan(JsonObject arguments, out IReadOnlyList<RqlStatement>? statements)
        {
            statements = null;
            var node = arguments[QueryArgument];
            var text = node is JsonValue value && value.TryGetValue<string>(out var s) ? s : null;
            if (node != null && text == null)
                return Failure(Diagnostic.Error("invalid-plan", $"'query' must be a string (got {JsonKindName(node)}): an RQL query such as actors | first 5"));
            if (string.IsNullOrWhiteSpace(text))
                return Failure(Diagnostic.Error("invalid-plan", "'query' is required: an RQL query such as actors | first 5"));
            var split = RqlParser.SplitStatements(text);
            if (split.Count > 1)
            {
                statements = split;
                return null;
            }
            // One statement between blank ones (";query;"), as RqlEngine.Execute does it: drop what follows the statement and
            // blank the separators before it, so diagnostic positions stay the request's. Text without a ';' is unchanged.
            var span = split[0].Span;
            if (span.Start > 0 || span.End < text.Length)
            {
                text = text.Substring(0, span.Start).Replace(';', ' ') + text.Substring(span.Start, span.End - span.Start);
                arguments[QueryArgument] = split[0].Text; // the plugin's undo step is named after the query
            }
            if (!RqlParser.TryParse(text, out var plan, out var error))
                return Failure(error ?? Diagnostic.Error("syntax-error", "the query does not parse"));
            arguments[PlanArgument] = PlanCodec.Serialize(plan!);
            return null;
        }

        /// <summary>
        /// Runs a request of several statements (RQL spec §14) with <see cref="RqlScript.Run"/>: each statement that runs is one
        /// call to the plugin through <paramref name="call"/> (the request's arguments with that statement's <c>query</c> and
        /// <c>plan</c>; <c>dry_run</c> and <c>max_targets</c> apply to each), so it gets its own session, budgets, target limit
        /// and, for rql-mutate, its own all-or-nothing change and undo step. RqlScript decides what runs: a read runs every
        /// statement that parses; a mutation runs nothing unless every statement parses and ends with set / call, and stops at
        /// the first statement that fails. The answer is one <c>[i/n]</c> block per statement, followed by the text blocks the
        /// plugin added after each answer (its "Editor diagnostics" notes), in order.
        /// An exception of the first call ends the request (the caller answers it as for a single statement: nothing else
        /// ran). One of a later call (a link drop, an IPC timeout) fails only its own statement, so the blocks still show what
        /// the earlier statements did; cancellation always ends the request.
        /// </summary>
        public static async Task<ResponseCallTool> RunStatementsAsync(string role, JsonObject arguments, IReadOnlyList<RqlStatement> statements,
            Func<JsonObject, Task<ResponseCallTool>> call)
        {
            var notes = new List<ContentBlock>();
            // Each statement's plan, to find the statement of a plan RqlScript runs: its text is the call's query (the undo step's title).
            var plans = statements.Select(st => RqlParser.TryParse(st.Text, out var p, out _) ? PlanCodec.Serialize(p!) : null).ToList();
            int next = 0, calls = 0;

            QueryResult Execute(QueryPlan plan)
            {
                var serialized = PlanCodec.Serialize(plan);
                var index = plans.FindIndex(next, p => p == serialized);
                if (index >= 0) next = index + 1;
                var statementArguments = (JsonObject)arguments.DeepClone();
                if (index >= 0) statementArguments[QueryArgument] = statements[index].Text;
                statementArguments[PlanArgument] = serialized;

                ResponseCallTool response;
                try { response = call(statementArguments).GetAwaiter().GetResult(); }
                catch (IpcDisconnectedException ex) when (calls > 0 && ex.NotSent)
                {
                    return QueryResult.Fail(new[] { Diagnostic.Error("internal-error", ex.Message) });
                }
                catch (Exception ex) when (calls > 0 && !(ex is OperationCanceledException))
                {
                    return QueryResult.Fail(new[] { Diagnostic.Error("internal-error", ex.Message,
                        "the editor may have run this statement: check it before running it again") });
                }
                finally { calls++; }

                var first = response.Content?.FirstOrDefault(c => c.Type == Consts.ContentType.Text);
                if (response.Content != null)
                    notes.AddRange(response.Content.Where(c => !ReferenceEquals(c, first)));
                var json = ParseObject(first?.Text);
                if (json == null || !json.ContainsKey("ok"))
                    return QueryResult.Fail(new[] { Diagnostic.Error("internal-error", string.IsNullOrEmpty(first?.Text) ? "the editor sent no RQL result" : first!.Text!) });
                return ToResult(role, json);
            }

            // RqlScript runs the statements synchronously, one after another: off the caller's thread, each IPC call awaited in turn.
            var result = await Task.Run(() => RqlScript.Run(statements, new QueryOptions { AllowMutation = role == MutateRole }, Execute)).ConfigureAwait(false);
            var content = new List<ContentBlock> { new ContentBlock { Type = Consts.ContentType.Text, Text = RenderResult(result) } };
            content.AddRange(notes);
            return new ResponseCallTool { Status = result.Ok ? ResponseStatus.Success : ResponseStatus.Error, Content = content };
        }

        /// <summary>
        /// The plugin's answer for an RQL tool as agent text, keeping its status. An answer that is not RQL JSON (an IPC
        /// error, a disconnected bridge) is returned unchanged.
        /// </summary>
        public static ResponseCallTool Render(string role, ResponseCallTool response)
        {
            var first = response.Content?.FirstOrDefault(c => c.Type == Consts.ContentType.Text);
            var json = ParseObject(first?.Text);
            if (json == null)
                return response;

            string rendered;
            if (role == SchemaRole)
                rendered = RenderSchema(json);
            else if (json.ContainsKey("ok"))
                rendered = RenderResult(ToResult(role, json));
            else
                return response;

            // Only the first text block is the RQL answer; later blocks (the plugin's "Editor diagnostics" note) follow it unchanged.
            var content = new List<ContentBlock> { new ContentBlock { Type = Consts.ContentType.Text, Text = rendered } };
            content.AddRange(response.Content!.Where(c => !ReferenceEquals(c, first)));
            response.Content = content;
            response.StructuredContent = null;
            return response;
        }

        private static JsonObject? ParseObject(string? text)
        {
            if (string.IsNullOrEmpty(text))
                return null;
            try { return JsonNode.Parse(text) as JsonObject; }
            catch (JsonException) { return null; }
        }

        /// <summary>
        /// A query / mutate answer as RQL's result. rql-mutate answers with a set / call receipt: the table format compacts it
        /// (one method: line, no $ref column for created objects, the first 50 rows and a count). The flag is not part of the JSON.
        /// </summary>
        private static QueryResult ToResult(string role, JsonObject json)
        {
            var result = ParseResult(json);
            result.Receipt = role == MutateRole && result.Ok;
            return result;
        }

        // The rql tools have no json format, so a receipt's count carries no "(the json format lists every row)" pointer.
        private static string RenderResult(QueryResult result) =>
            AgentFormat.Render(result, OutputFormat.Table, AgentFormat.DefaultReceiptRows, moreRowsNote: "");

        private static string JsonKindName(JsonNode node) =>
            node.GetValueKind() is JsonValueKind.True or JsonValueKind.False ? "boolean" : node.GetValueKind().ToString().ToLowerInvariant();

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
            // The parsed answer is ours: its nodes are detached and moved into the result (no deep copies).
            if (json["columns"] is JsonArray columns)
                result.Columns = columns.Select(c => c?.GetValue<string>() ?? "").ToList();
            if (json["rows"] is JsonArray rows)
                result.Rows = Detach(rows).Select(r => r is JsonArray cells ? Detach(cells).ToArray() : Array.Empty<JsonNode?>()).ToList();
            if (json["items"] is JsonArray items)
                result.Items = Detach(items).ToList();
            if (json.TryGetPropertyValue("value", out var value))
            {
                result.HasValue = true;
                json.Remove("value");
                result.Value = value;
            }
            if (json["types"] is JsonArray types)
            {
                json.Remove("types");
                result.Types = types;
            }
            if (json["total"] is JsonValue total && total.TryGetValue<long>(out var t))
                result.Total = t;
            // A set receipt leaves out assignments that kept the old value and counts them here (present only when > 0).
            if (json["unchanged"] is JsonValue unchanged && unchanged.TryGetValue<long>(out var u) && u > 0)
                result.Unchanged = u;
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

        /// <summary>The elements of <paramref name="array"/>, removed from it so they can join another tree.</summary>
        private static List<JsonNode?> Detach(JsonArray array)
        {
            var nodes = array.ToList();
            array.Clear();
            return nodes;
        }

        // ------------------------------------------------------------ schema JSON -> compact text

        /// <summary>
        /// rql-schema's text. Without a type: the sources, functions, missing extensions, the classes present in the editor
        /// world and the Actor / SceneComponent member names (<c>Name : Base {a, b, …} methods: …</c>). With one: that type
        /// in the compact lines of RQL's own schema (<c>Name : Base {member type ro, …} methods: …</c>, as Unity-MCP prints
        /// it; members shared with Actor / ActorComponent summarised).
        /// </summary>
        public static string RenderSchema(JsonObject json)
        {
            if (Str(json, "error") == "unknown-type")
            {
                var candidates = (json["candidates"] as JsonArray)?.Select(c => c?.GetValue<string>() ?? "").Where(c => c.Length > 0).ToList()
                    ?? new List<string>();
                var error = AgentFormat.UnknownTypeError(Str(json, "name") ?? "", candidates);
                // The type belongs to an extension this editor does not have: the hint names it and how to get it.
                var hint = Str(json, "hint");
                return string.IsNullOrEmpty(hint) ? error : error.TrimEnd() + "\nhint: " + hint;
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
            if (json["extensions"] is JsonArray extensions && extensions.Count > 0)
            {
                // The Unreal-MCP extensions this editor does not have: the note says how to install one, a line each.
                sb.Append(Str(json, "extensionsNote") ?? "extensions:").Append('\n');
                foreach (var e in extensions.OfType<JsonObject>())
                    sb.Append("  ").Append(ShortExtensionLine(Str(e, "line") ?? Str(e, "id") ?? "")).Append('\n');
            }
            AppendPresent(sb, json["actorClasses"] as JsonArray, "actor classes in the editor world");
            AppendPresent(sb, json["componentClasses"] as JsonArray, "component classes in the editor world");
            var overview = json["sources"] is JsonArray;
            if (overview)
                sb.Append("types (rql-schema \"<type>\" lists a type's members with their types and descriptions):\n");

            if (json["types"] is JsonArray types)
            {
                foreach (var entry in types.OfType<JsonObject>())
                {
                    if (!(entry["type"] is JsonObject type)) continue;
                    if (overview)
                    {
                        AppendTypeSummary(sb, type, entry["shared"] as JsonObject);
                        continue;
                    }
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

        /// <summary>
        /// "id (Title): what it adds - not installed" -> "id (Title)": the title names the feature, and the rest costs a few
        /// hundred characters per extension on every later turn. Any other state keeps its tail ("id (Title) - plugin
        /// disabled: enable the plugin ..."), since installing is then the wrong advice.
        /// </summary>
        public static string ShortExtensionLine(string line)
        {
            const string NotInstalled = " - not installed";
            var titleEnd = line.IndexOf("): ", StringComparison.Ordinal);
            return titleEnd > 0 && line.EndsWith(NotInstalled, StringComparison.Ordinal) ? line.Substring(0, titleEnd + 1) : line;
        }

        /// <summary><c>Name : Base</c>.</summary>
        private static void AppendTypeHeader(StringBuilder sb, JsonObject type)
        {
            sb.Append(Str(type, "name") ?? "?");
            var baseName = Str(type, "base");
            if (!string.IsNullOrEmpty(baseName)) sb.Append(" : ").Append(baseName);
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
            if (kind == "struct") sb.Append("struct ");
            AppendTypeHeader(sb, type);
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

        /// <summary>
        /// The overview's short form of a type (its member types and descriptions cost kilobytes the agent re-reads every
        /// turn): its own member names, the base whose members it shares, and its method signatures.
        /// </summary>
        private static void AppendTypeSummary(StringBuilder sb, JsonObject type, JsonObject? shared)
        {
            AppendTypeHeader(sb, type);
            var inherited = new HashSet<string>(shared == null ? Enumerable.Empty<string>() : MemberNames(shared), StringComparer.Ordinal);
            sb.Append(" {").Append(string.Join(", ", MemberNames(type).Where(n => !inherited.Contains(n)))).Append('}');
            if (inherited.Count > 0) sb.Append(" (+ the ").Append(Str(shared!, "name")).Append(" members)");
            if (type["methods"] is JsonArray methods && methods.Count > 0)
            {
                // "Destroy() — remove the actor" -> "Destroy()"
                var signatures = methods.Select(m => (m?.GetValue<string>() ?? "").Split(" — ")[0]);
                sb.Append(" methods: ").Append(string.Join(" | ", signatures));
            }
            sb.Append('\n');
        }

        private static IEnumerable<string> MemberNames(JsonObject type) =>
            (type["members"] as JsonArray)?.OfType<JsonObject>().Select(m => Str(m, "name") ?? "") ?? Enumerable.Empty<string>();

        private static string? Str(JsonObject o, string key) => o[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;

        private static bool Bool(JsonObject o, string key) => o[key] is JsonValue v && v.TryGetValue<bool>(out var b) && b;
    }
}
