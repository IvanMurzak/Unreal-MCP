/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak)               │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;
using ProxyToolFactory = com.IvanMurzak.Unreal.MCP.Bridge.Tools.ProxyToolFactory;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// MCP tool annotations (the Claude Connectors Directory rule): every tool in <c>tools/list</c> carries a non-empty
    /// <c>title</c> and explicit <c>readOnlyHint</c> / <c>destructiveHint</c> (plus <c>idempotentHint</c> /
    /// <c>openWorldHint</c>). The plugin declares them per tool (C++ guard: <c>UnrealMcp.Tools.Annotations</c>), the
    /// manifest ships them, <see cref="ProxyToolFactory"/> copies them onto the proxy and McpPlugin's tool listing
    /// forwards them to the server's <c>annotations</c>.
    ///
    /// <para>The fixture tests read <c>tests/chain-fixtures/&lt;ue&gt;/tools.jsonl</c>: the live <c>tools/list</c> of the
    /// editor that the CI <c>connection-smoke</c> leg diffs against, so a tool without a title or a hint fails here
    /// too, without an editor.</para>
    /// </summary>
    public class ToolAnnotationsTests
    {
        private static readonly string[] Hints = { "readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint" };

        private const string RqlDocsUrl = "https://ai-game.dev/docs/rql";

        [Fact]
        public void ProxyToolFactory_CarriesTitleAndEveryHintFromTheManifest()
        {
            // The descriptor exactly as the C++ ToDescriptorJson() writes it (all four hints always present).
            const string json = """
                {"name":"source-delete","title":"Delete Source File","description":"d","schemaHash":"h",
                 "readOnlyHint":false,"destructiveHint":true,"idempotentHint":true,"openWorldHint":false}
                """;
            var descriptor = JsonSerializer.Deserialize<ToolDescriptor>(json)!;

            var proxy = ProxyToolFactory.Create(descriptor, new FakeToolCallChannel());

            Assert.Equal("Delete Source File", proxy.Title);
            Assert.False(proxy.ReadOnlyHint);
            Assert.True(proxy.DestructiveHint);
            Assert.True(proxy.IdempotentHint);
            Assert.False(proxy.OpenWorldHint);
        }

        public static IEnumerable<object[]> FixtureTools() =>
            FixtureFiles().SelectMany(file => File.ReadAllLines(file)
                .Where(line => line.Length > 0)
                .Select(line => JsonNode.Parse(line)!.AsObject())
                .Where(entry => (string?)entry["kind"] == "tool")
                .Select(entry => new object[] { Path.GetFileName(Path.GetDirectoryName(file))!, entry["name"]!.GetValue<string>(), entry.ToJsonString() }));

        [Fact]
        public void Fixtures_ArePresentAndListTools()
        {
            Assert.NotEmpty(FixtureFiles());
            Assert.NotEmpty(FixtureTools());
        }

        [Theory]
        [MemberData(nameof(FixtureTools))]
        public void EveryListedTool_HasATitleAndExplicitHints(string engineVersion, string name, string toolJson)
        {
            var tool = JsonNode.Parse(toolJson)!.AsObject();
            var where = $"{name} (UE {engineVersion})";

            var title = tool["title"]?.GetValue<string>();
            Assert.False(string.IsNullOrWhiteSpace(title), $"{where} has no title");

            foreach (var hint in Hints)
            {
                Assert.True(tool[hint]?.GetValueKind() is JsonValueKind.True or JsonValueKind.False,
                    $"{where} does not declare {hint} as a boolean");
            }
            Assert.False(tool["readOnlyHint"]!.GetValue<bool>() && tool["destructiveHint"]!.GetValue<bool>(),
                $"{where} is both read-only and destructive");

            if (name.StartsWith("rql-"))
            {
                var description = tool["description"]?.GetValue<string>() ?? string.Empty;
                Assert.Contains("RQL", description);
                Assert.Contains(RqlDocsUrl, description);
            }
        }

        private static string[] FixtureFiles([CallerFilePath] string thisFile = "")
        {
            // bridge/tests/<this file> -> repo root -> tests/chain-fixtures/<ue>/tools.jsonl
            var root = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(thisFile)!, "..", "..", "tests", "chain-fixtures"));
            return Directory.Exists(root)
                ? Directory.GetFiles(root, "tools.jsonl", SearchOption.AllDirectories)
                : new string[0];
        }
    }
}
