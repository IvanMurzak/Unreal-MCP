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
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// The sidecar half of the RQL data layer (docs/rql.md): query text → canonical plan before the IPC call, the C++
    /// QueryResult / schema JSON → RQL's compact agent text after it, and the unreal-rql skill file.
    /// </summary>
    public class RqlBridgeTests : IDisposable
    {
        private readonly string _root = Path.Combine(Path.GetTempPath(), "unreal-mcp-rql-" + Guid.NewGuid().ToString("N"));

        public void Dispose()
        {
            try { if (Directory.Exists(_root)) Directory.Delete(_root, recursive: true); }
            catch { /* best-effort temp cleanup */ }
        }

        private static ResponseCallTool Text(string text, ResponseStatus status = ResponseStatus.Success) => new ResponseCallTool
        {
            Status = status,
            Content = new List<ContentBlock> { new ContentBlock { Type = Consts.ContentType.Text, Text = text } },
        };

        private static string Rendered(ResponseCallTool response) => response.Content!.Single().Text!;

        private static Dictionary<string, JsonElement> Args(string json)
        {
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone());
        }

        // ------------------------------------------------------------ plans

        [Fact]
        public void AddPlan_StoresTheCanonicalPlanOfTheQuery()
        {
            var args = new JsonObject { ["query"] = "actors | where Label == \"Lamp\" | first 2" };
            Assert.Null(RqlBridge.AddPlan(args));
            var plan = JsonNode.Parse(args["plan"]!.GetValue<string>())!.AsObject();
            Assert.Equal("0.1", plan["rql"]!.GetValue<string>());
            Assert.Equal("actors", plan["source"]!["path"]![0]!.GetValue<string>());
            Assert.Equal("first", plan["stages"]![1]!["op"]!.GetValue<string>());
            Assert.Equal("actors | where Label == \"Lamp\" | first 2", args["query"]!.GetValue<string>());
        }

        [Fact]
        public void AddPlan_ParsesTheConditionalAndKeepsTheOtherArguments()
        {
            // RQL 7dd3b29: cond ? a : b is the core function if (the plugin's C++ runtime runs it); rql-mutate's max_targets
            // and dry_run travel to the plugin untouched.
            var args = new JsonObject
            {
                ["query"] = "actors | select kind = ($ is PointLight ? \"point\" : \"other\")",
                ["max_targets"] = 300,
                ["dry_run"] = true,
            };
            Assert.Null(RqlBridge.AddPlan(args));
            var plan = JsonNode.Parse(args["plan"]!.GetValue<string>())!.AsObject();
            var expr = plan["stages"]![0]!["fields"]![0]!["expr"]!;
            Assert.Equal("if", expr["call"]!.GetValue<string>());
            Assert.Equal(3, expr["args"]!.AsArray().Count);
            Assert.Equal(300, args["max_targets"]!.GetValue<int>());
            Assert.True(args["dry_run"]!.GetValue<bool>());

            var incomplete = RqlBridge.AddPlan(new JsonObject { ["query"] = "actors | select k = ($ is PointLight ? \"point\")" });
            Assert.NotNull(incomplete);
            Assert.Contains("syntax-error", incomplete!.Content!.Single().Text);
        }

        [Fact]
        public void AddPlan_AnswersSyntaxErrorsLikeAFailingQuery()
        {
            var error = RqlBridge.AddPlan(new JsonObject { ["query"] = "actors | where (" });
            Assert.NotNull(error);
            Assert.Equal(ResponseStatus.Error, error!.Status);
            Assert.StartsWith("error ", error.Content!.Single().Text);
        }

        [Fact]
        public void AddPlan_RequiresAQuery()
        {
            var error = RqlBridge.AddPlan(new JsonObject());
            Assert.NotNull(error);
            Assert.Contains("'query' is required", error!.Content!.Single().Text);
        }

        // ------------------------------------------------------------ results

        [Fact]
        public void Render_RowsAsCompactTable()
        {
            var json = "{\"rql\":\"0.1\",\"ok\":true,\"kind\":\"rows\",\"columns\":[\"$ref\",\"Label\",\"Location\"]," +
                       "\"rows\":[[{\"$ref\":\"PointLight#5\",\"name\":\"Lamp\"},\"Lamp\",\"(1, 2, 3)\"]],\"total\":1}";
            var text = Rendered(RqlBridge.Render(RqlBridge.QueryRole, Text(json)));
            Assert.Equal("1 result\n$ref|Label|Location\nPointLight#5(Lamp)|Lamp|(1, 2, 3)", text);
        }

        [Fact]
        public void Render_DiagnosticsWithHintsAndKeepsTheErrorStatus()
        {
            var json = "{\"rql\":\"0.1\",\"ok\":false,\"diagnostics\":[{\"severity\":\"error\",\"code\":\"readonly-member\"," +
                       "\"message\":\"Name is read-only\",\"hint\":\"set Label instead\"}]}";
            var response = RqlBridge.Render(RqlBridge.MutateRole, Text(json, ResponseStatus.Error));
            Assert.Equal(ResponseStatus.Error, response.Status);
            Assert.Equal("error readonly-member: Name is read-only — hint: set Label instead", Rendered(response));
        }

        [Fact]
        public void Render_DryRunReceiptsAndTruncation()
        {
            var dry = "{\"rql\":\"0.1\",\"ok\":true,\"kind\":\"rows\",\"columns\":[\"$ref\",\"member\",\"old\",\"new\"]," +
                      "\"rows\":[[\"PointLight#5\",\"Intensity\",5000,10000]],\"dryRun\":true}";
            Assert.StartsWith("DRY RUN — nothing changed; 1 result\n$ref|member|old|new\nPointLight#5|Intensity|5000|10000",
                Rendered(RqlBridge.Render(RqlBridge.MutateRole, Text(dry))));

            var cut = "{\"rql\":\"0.1\",\"ok\":true,\"kind\":\"rows\",\"columns\":[\"Label\"],\"rows\":[[\"a\"],[\"b\"]],\"total\":70,\"truncated\":true}";
            Assert.StartsWith("2 of 70 (truncated;", Rendered(RqlBridge.Render(RqlBridge.QueryRole, Text(cut))));
        }

        [Fact]
        public void Render_Scalars()
        {
            var json = "{\"rql\":\"0.1\",\"ok\":true,\"kind\":\"scalar\",\"value\":3}";
            Assert.Equal("3", Rendered(RqlBridge.Render(RqlBridge.QueryRole, Text(json))));
        }

        [Fact]
        public void Render_LeavesNonRqlAnswersAlone()
        {
            var response = Text("bridge disconnected", ResponseStatus.Error);
            Assert.Same(response, RqlBridge.Render(RqlBridge.QueryRole, response));
            Assert.Equal("bridge disconnected", Rendered(response));
        }

        // ------------------------------------------------------------ schema

        [Fact]
        public void RenderSchema_OverviewListsSourcesFunctionsClassesAndTypes()
        {
            var json = JsonNode.Parse(@"{
                ""sources"": [{""name"": ""actors"", ""type"": ""list of Actor"", ""description"": ""every actor""}],
                ""functions"": [{""name"": ""unreal.find"", ""description"": ""an actor by label""}],
                ""actorClasses"": [{""type"": ""StaticMeshActor"", ""count"": 12}],
                ""componentClasses"": [],
                ""types"": [{""type"": {""name"": ""Actor"", ""base"": ""Object"", ""kind"": ""object"",
                    ""members"": [{""name"": ""Id"", ""type"": ""int""}, {""name"": ""Label"", ""type"": ""string"", ""writable"": true}],
                    ""methods"": [""Destroy() — remove the actor""]}}]
            }")!.AsObject();
            var text = RqlBridge.RenderSchema(json);
            Assert.Contains("sources:\n  actors: list of Actor — every actor\n", text);
            Assert.Contains("functions:\n  unreal.find(…) — an actor by label\n", text);
            Assert.Contains("actor classes in the editor world: StaticMeshActor ×12\n", text);
            Assert.Contains("component classes in the editor world: none\n", text);
            Assert.Contains("Actor : Object {Id int ro, Label string} methods: Destroy() — remove the actor", text);
        }

        [Fact]
        public void RenderSchema_OneTypeSummarisesTheSharedBase()
        {
            var json = JsonNode.Parse(@"{""types"": [{
                ""type"": {""name"": ""PointLight"", ""base"": ""Light"", ""kind"": ""object"",
                    ""members"": [{""name"": ""Label"", ""type"": ""string"", ""writable"": true}, {""name"": ""PointLightComponent"", ""type"": ""PointLightComponent""}],
                    ""methods"": [""Destroy()""]},
                ""shared"": {""name"": ""Actor"", ""kind"": ""object"", ""members"": [{""name"": ""Label"", ""type"": ""string""}], ""methods"": [""Destroy()""]}
            }]}")!.AsObject();
            var text = RqlBridge.RenderSchema(json);
            Assert.Equal("PointLight : Light {PointLightComponent PointLightComponent ro}\n  (+ inherited from Actor: Label)", text);
        }

        [Fact]
        public void RenderSchema_UnknownTypeSuggestsCandidates()
        {
            var json = JsonNode.Parse(@"{""error"": ""unknown-type"", ""name"": ""PointLigth"", ""candidates"": [""PointLight"", ""StaticMeshActor""]}")!.AsObject();
            var text = RqlBridge.RenderSchema(json);
            Assert.StartsWith("error unknown-type: no type 'PointLigth'", text);
            Assert.Contains("PointLight", text);
        }

        // ------------------------------------------------------------ proxy wiring

        [Fact]
        public async Task Proxy_SendsThePlanAndRendersTheAnswer()
        {
            var channel = new FakeToolCallChannel((tool, args) => new ToolResponseMessage
            {
                RequestId = "r",
                Status = IpcProtocol.Status.Success,
                Content = new JsonArray(new JsonObject
                {
                    ["type"] = "text",
                    ["text"] = "{\"rql\":\"0.1\",\"ok\":true,\"kind\":\"scalar\",\"value\":2}",
                }),
            });
            var skill = new ToolSkillDescriptor { Name = "unreal-rql", Description = "d", Markdown = "# RQL" };
            var proxy = ProxyToolFactory.Create(new ToolDescriptor { Name = "rql-query", Rql = RqlBridge.QueryRole, Skills = new List<ToolSkillDescriptor> { skill } }, channel);
            Assert.Null(proxy.SkillBody); // the unreal-rql skill stays out of tools/list

            var response = await proxy.Run("req", Args("{\"query\":\"actors | count\"}"));
            Assert.Equal(1, channel.Calls);
            Assert.Contains("\"op\":\"count\"", channel.LastArguments!["plan"]!.GetValue<string>());
            Assert.Equal("2", response.Content!.Single().Text);
            Assert.Equal("req", response.RequestID);
        }

        [Fact]
        public async Task Proxy_ParseErrorsNeverReachThePlugin()
        {
            var channel = new FakeToolCallChannel();
            var proxy = ProxyToolFactory.Create(new ToolDescriptor { Name = "rql-mutate", Rql = RqlBridge.MutateRole }, channel);
            var response = await proxy.Run("req", Args("{\"query\":\"actors | set\"}"));
            Assert.Equal(0, channel.Calls);
            Assert.Equal(ResponseStatus.Error, response.Status);
        }

        [Fact]
        public async Task Proxy_OtherToolsAreUntouched()
        {
            var channel = new FakeToolCallChannel((tool, args) => new ToolResponseMessage
            {
                RequestId = "r",
                Status = IpcProtocol.Status.Success,
                Content = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = "{\"ok\":true}" }),
            });
            var proxy = ProxyToolFactory.Create(new ToolDescriptor { Name = "source-list", SkillBody = "body" }, channel);
            Assert.Equal("body", proxy.SkillBody);
            var response = await proxy.Run("req", Args("{\"query\":\"x\"}"));
            Assert.False(channel.LastArguments!.ContainsKey("plan"));
            Assert.Equal("{\"ok\":true}", response.Content!.Single().Text);
        }

        // ------------------------------------------------------------ unreal-rql skill

        [Fact]
        public void Descriptor_CarriesTheRqlRoleAndTheShippedSkills()
        {
            var descriptor = JsonSerializer.Deserialize<ToolDescriptor>(
                @"{""name"": ""rql-query"", ""rql"": ""query"", ""skills"": [{""name"": ""unreal-rql"", ""description"": ""d"", ""markdown"": ""# RQL""}]}")!;
            Assert.Equal(RqlBridge.QueryRole, descriptor.Rql);
            Assert.Equal("unreal-rql", Assert.Single(descriptor.Skills!).Name);
            Assert.True(RqlBridge.ParsesQuery(descriptor.Rql));
            Assert.False(RqlBridge.ParsesQuery(RqlBridge.SchemaRole));

            // A new skill text is a changed entry (the registrar re-registers it; the generator writes the new skill).
            var changed = JsonSerializer.Deserialize<ToolDescriptor>(
                @"{""name"": ""rql-query"", ""rql"": ""query"", ""skills"": [{""name"": ""unreal-rql"", ""description"": ""d"", ""markdown"": ""# RQL 2""}]}")!;
            Assert.NotEqual(descriptor.StructuralSignature, changed.StructuralSignature);
        }

        [Fact]
        public void SkillGenerator_WritesTheSkillsAToolShips()
        {
            var skill = new ToolSkillDescriptor
            {
                Name = "unreal-rql",
                Description = "How to read and change anything in the Unreal Editor with RQL.",
                Markdown = "# RQL - query and change the Unreal Editor\n\nbody",
            };
            var tools = new List<ToolDescriptor>
            {
                new ToolDescriptor { Name = "rql-query", Title = "RQL / Query", Description = "Run a read-only RQL query.", Rql = RqlBridge.QueryRole, Skills = new List<ToolSkillDescriptor> { skill } },
                new ToolDescriptor { Name = "rql-schema", Title = "RQL / Schema", Description = "List types.", Rql = RqlBridge.SchemaRole },
            };
            var result = new SkillFileGenerator().Generate(tools, _root);
            Assert.True(result.Success, result.Error);
            Assert.Equal(5, result.FilesWritten);

            var written = File.ReadAllText(Path.Combine(_root, "unreal-rql", "SKILL.md"));
            Assert.Equal("---\nname: unreal-rql\ndescription: \"How to read and change anything in the Unreal Editor with RQL.\"\n"
                + "metadata:\n  generated-by: unreal-mcp\n---\n\n# RQL - query and change the Unreal Editor\n\nbody\n\n> CLI installation and usage: see the `unreal-cli` skill.\n> If this task needs unavailable asset-generation or Blender tools, consult `ai-game-dev-app` for an optional integrated workflow.\n", written);

            // The tool's own page documents the tool (its description), not the whole guide.
            var page = File.ReadAllText(Path.Combine(_root, "rql-query", "SKILL.md"));
            Assert.Contains("Run a read-only RQL query.", page);
            Assert.DoesNotContain("# RQL - query and change", page);
        }
    }
}
