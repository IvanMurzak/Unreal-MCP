/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    public class ProxyToolFactoryTests
    {
        private static Dictionary<string, JsonElement> Args(string json)
        {
            using var doc = JsonDocument.Parse(json);
            var dict = new Dictionary<string, JsonElement>();
            foreach (var prop in doc.RootElement.EnumerateObject())
                dict[prop.Name] = prop.Value.Clone();
            return dict;
        }

        [Fact]
        public void ToArguments_NullBecomesEmptyObject()
        {
            var obj = ProxyToolFactory.ToArguments(null);
            Assert.NotNull(obj);
            Assert.Empty(obj);
        }

        [Fact]
        public void ToArguments_PreservesNamedParameters()
        {
            var obj = ProxyToolFactory.ToArguments(Args("{\"message\":\"hi\",\"count\":3}"));
            Assert.Equal("hi", obj["message"]!.GetValue<string>());
            Assert.Equal(3, obj["count"]!.GetValue<int>());
        }

        [Fact]
        public async Task Create_RoundTripsCallThroughChannel()
        {
            var channel = new FakeToolCallChannel((tool, args) => new ToolResponseMessage
            {
                RequestId = "x",
                Status = IpcProtocol.Status.Success,
                Structured = JsonNode.Parse($"{{\"echo\":\"{args!["message"]!.GetValue<string>()}\"}}"),
            });

            var proxy = ProxyToolFactory.Create(new ToolDescriptor { Name = "ping" }, channel);
            var result = await proxy.Run("req-1", Args("{\"message\":\"hello\"}"));

            Assert.Equal("ping", channel.LastTool);
            Assert.Equal("hello", channel.LastArguments!["message"]!.GetValue<string>());
            Assert.Equal(ResponseStatus.Success, result.Status);
            Assert.Equal("hello", result.StructuredContent!["echo"]!.GetValue<string>());
        }

        [Fact]
        public async Task Create_DisconnectedChannel_FailsFastWithStructuredError()
        {
            var channel = new FakeToolCallChannel { Connected = false };
            var proxy = ProxyToolFactory.Create(new ToolDescriptor { Name = "ping" }, channel);

            var result = await proxy.Run("req-1", null);

            Assert.Equal(ResponseStatus.Error, result.Status);
            Assert.Contains("disconnected", result.GetMessage());
        }

        [Fact]
        public void Create_PropagatesEnabledFlag()
        {
            var channel = new FakeToolCallChannel();
            var proxy = ProxyToolFactory.Create(new ToolDescriptor { Name = "ping", Enabled = false }, channel);
            Assert.False(proxy.Enabled);
        }
    }

    public class ProxyResponseMapperTests
    {
        [Fact]
        public void Map_SuccessWithTextContentAndStructured()
        {
            var msg = new ToolResponseMessage
            {
                Status = IpcProtocol.Status.Success,
                Content = JsonNode.Parse("[{\"type\":\"text\",\"text\":\"pong\",\"mimeType\":\"text/plain\"}]")!.AsArray(),
                Structured = JsonNode.Parse("{\"result\":\"pong\"}"),
            };

            var result = ProxyResponseMapper.Map(msg, "req-9");

            Assert.Equal("req-9", result.RequestID);
            Assert.Equal(ResponseStatus.Success, result.Status);
            Assert.Single(result.Content);
            Assert.Equal("pong", result.Content[0].Text);
            Assert.Equal("pong", result.StructuredContent!["result"]!.GetValue<string>());
        }

        // Characterizes the bridge half of the screenshot wire shape: the mapper passes the plugin's text +
        // image blocks through and adds no structured content of its own (McpPlugin.Server drops the content
        // array whenever StructuredContent is set). The plugin half — screenshots never SET structured
        // content — is pinned by UnrealMcpScreenshotToolsSpec, not here.
        [Fact]
        public void Map_ImageResultWithoutStructured_KeepsImageBlockAndLeavesStructuredNull()
        {
            var msg = new ToolResponseMessage
            {
                Status = IpcProtocol.Status.Success,
                Content = JsonNode.Parse(
                    "[{\"type\":\"text\",\"text\":\"Captured viewport (64x32, image/png, 10 bytes).\"}," +
                    "{\"type\":\"image\",\"data\":\"aGVsbG8=\",\"mimeType\":\"image/png\"}]")!.AsArray(),
                Structured = null,
            };

            var result = ProxyResponseMapper.Map(msg, "req-img");

            Assert.Null(result.StructuredContent);
            Assert.Equal(2, result.Content.Count);
            Assert.Equal("text", result.Content[0].Type);
            Assert.Contains("64x32", result.Content[0].Text);
            Assert.Equal("image", result.Content[1].Type);
            Assert.Equal("aGVsbG8=", result.Content[1].Data);
            Assert.Equal("image/png", result.Content[1].MimeType);
        }
        [Fact]
        public void Map_ErrorStatusMapsToError()
        {
            var msg = new ToolResponseMessage { Status = IpcProtocol.Status.Error };
            var result = ProxyResponseMapper.Map(msg, "r");
            Assert.Equal(ResponseStatus.Error, result.Status);
        }

        [Fact]
        public void Map_NullContent_YieldsEmptyContentList()
        {
            var msg = new ToolResponseMessage { Status = IpcProtocol.Status.Success, Content = null };
            var result = ProxyResponseMapper.Map(msg, "r");
            Assert.Empty(result.Content);
        }
    }
}
