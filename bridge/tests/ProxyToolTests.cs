/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>The "ProxyTool mapping" xUnit target (docs/ARCHITECTURE.md §9.3, §2.2-2.3).</summary>
    public class ProxyToolTests
    {
        private static ProxyTool Make(JsonNode? input = null, System.Func<string, System.Collections.Generic.IReadOnlyDictionary<string, JsonElement>?, System.Threading.CancellationToken, Task<ResponseCallTool>>? handler = null)
            => new ProxyTool("ping", "Ping", "Round-trip a ping.", null, null, input, null, true, false, true, false,
                handler ?? ((_, _, _) => Task.FromResult(ResponseCallTool.Success("pong"))));

        [Fact]
        public void TokenCount_MatchesCharsOver4Formula()
        {
            var input = JsonNode.Parse("{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}}}");
            var tool = Make(input);
            var expected = ProxyTool.CalculateTokenCount("ping", "Ping", "Round-trip a ping.", input, null);
            Assert.Equal(expected, tool.TokenCount);
            Assert.True(tool.TokenCount > 0);
        }

        [Fact]
        public void InputSchema_IsDetachedCopy()
        {
            var input = JsonNode.Parse("{\"type\":\"object\"}");
            var tool = Make(input);
            // Mutating the caller's live node must not change the proxy's copy.
            input!["type"] = "MUTATED";
            Assert.Equal("object", tool.InputSchema!["type"]!.GetValue<string>());
        }

        [Fact]
        public async Task Run_DelegatesToHandler()
        {
            var tool = Make(handler: (requestId, args, _) =>
            {
                Assert.Equal("req-1", requestId);
                return Task.FromResult(ResponseCallTool.Success("handled"));
            });
            var result = await tool.Run("req-1", null);
            Assert.Equal(ResponseStatus.Success, result.Status);
            Assert.Equal("handled", result.GetMessage());
        }

        [Fact]
        public async Task Run_PassesTheCallersToken_WhenItCanBeCancelled()
        {
            using var cts = new System.Threading.CancellationTokenSource();
            System.Threading.CancellationToken seen = default;
            var tool = Make(handler: (_, _, token) =>
            {
                seen = token;
                return Task.FromResult(ResponseCallTool.Success("handled"));
            });

            await tool.Run("req-1", null, cts.Token);
            Assert.Equal(cts.Token, seen);

            // Outside a McpPlugin call there is no call token: the handler gets one that never fires.
            await tool.Run("req-2", null);
            Assert.False(seen.CanBeCanceled);
        }

        [Fact]
        public void Hints_AreCarried()
        {
            var tool = Make();
            Assert.True(tool.ReadOnlyHint);
            Assert.False(tool.DestructiveHint);
            Assert.True(tool.IdempotentHint);
            Assert.False(tool.OpenWorldHint);
        }
    }
}
