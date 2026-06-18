/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak)              │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    public class ProxyPromptFactoryTests
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
        public async Task Create_RoundTripsGetThroughChannel_MapsMessagesRoleAndDescription()
        {
            var channel = new FakePromptCallChannel((prompt, args) => new PromptResponseMessage
            {
                RequestId = "x",
                Status = IpcProtocol.Status.Success,
                Description = "brief for " + args!["theme"]!.GetValue<string>(),
                Messages = new List<PromptResponseEntry>
                {
                    new() { Role = "user", Text = "Draft a brief for " + args!["theme"]!.GetValue<string>() },
                    new() { Role = "assistant", Text = "Sure." },
                },
            });

            var proxy = ProxyPromptFactory.Create(
                new PromptDescriptor { Name = "level-design-brief", Role = "user" }, channel);
            var result = await proxy.Run("req-1", Args("{\"theme\":\"haunted\"}"));

            Assert.Equal("level-design-brief", channel.LastPrompt);
            Assert.Equal("haunted", channel.LastArguments!["theme"]!.GetValue<string>());
            Assert.Equal(ResponseStatus.Success, result.Status);
            Assert.Equal("req-1", result.RequestID);
            Assert.Equal("brief for haunted", result.Description);
            Assert.Equal(2, result.Messages.Count);
            Assert.Equal(Role.User, result.Messages[0].Role);
            Assert.Equal("Draft a brief for haunted", result.Messages[0].Content?.Text);
            Assert.Equal(Role.Assistant, result.Messages[1].Role);
        }

        [Fact]
        public async Task Create_ErrorStatus_MapsToError()
        {
            var channel = new FakePromptCallChannel((prompt, args) => new PromptResponseMessage
            {
                RequestId = "x",
                Status = IpcProtocol.Status.Error,
                Error = "theme is required.",
            });

            var proxy = ProxyPromptFactory.Create(new PromptDescriptor { Name = "level-design-brief" }, channel);
            var result = await proxy.Run("req-2", null);

            Assert.Equal(ResponseStatus.Error, result.Status);
            Assert.Equal("req-2", result.RequestID);
            Assert.Contains("theme is required", result.GetMessage());
        }

        [Fact]
        public async Task Create_DisconnectedChannel_FailsFastWithStructuredError()
        {
            var channel = new FakePromptCallChannel { Connected = false };
            var proxy = ProxyPromptFactory.Create(new PromptDescriptor { Name = "level-design-brief" }, channel);

            var result = await proxy.Run("req-3", null);

            Assert.Equal(ResponseStatus.Error, result.Status);
            Assert.Equal("req-3", result.RequestID);
            Assert.Contains("disconnected", result.GetMessage());
        }

        [Theory]
        [InlineData("assistant", Role.Assistant)]
        [InlineData("USER", Role.User)]
        [InlineData(null, Role.Assistant)]   // missing → falls back to the descriptor's role
        public async Task Create_ParsesRole_FallingBackToDescriptorRole(string? entryRole, Role expected)
        {
            var channel = new FakePromptCallChannel((prompt, args) => new PromptResponseMessage
            {
                Status = IpcProtocol.Status.Success,
                Messages = new List<PromptResponseEntry> { new() { Role = entryRole, Text = "hi" } },
            });

            // Descriptor role = assistant so the null-entry case falls back to assistant.
            var proxy = ProxyPromptFactory.Create(new PromptDescriptor { Name = "p", Role = "assistant" }, channel);
            var result = await proxy.Run("r", null);
            Assert.Equal(expected, result.Messages.Single().Role);
        }

        [Fact]
        public void Create_MapsDescriptorRoleAndPropagatesEnabledFlag()
        {
            var channel = new FakePromptCallChannel();
            var proxy = ProxyPromptFactory.Create(
                new PromptDescriptor { Name = "p", Role = "assistant", Enabled = false }, channel);
            Assert.Equal(Role.Assistant, proxy.Role);
            Assert.False(proxy.Enabled);
        }

        [Fact]
        public void Create_DefaultsRoleToUser_WhenDescriptorRoleAbsent()
        {
            var channel = new FakePromptCallChannel();
            var proxy = ProxyPromptFactory.Create(new PromptDescriptor { Name = "p" }, channel);
            Assert.Equal(Role.User, proxy.Role);
        }

        [Fact]
        public void Create_DetachesInputSchema()
        {
            var schema = JsonNode.Parse("{\"type\":\"object\",\"properties\":{\"theme\":{\"type\":\"string\"}}}");
            var channel = new FakePromptCallChannel();
            var proxy = ProxyPromptFactory.Create(
                new PromptDescriptor { Name = "p", InputSchema = schema }, channel);
            // A detached clone: not the same reference the descriptor handed in.
            Assert.NotNull(proxy.InputSchema);
            Assert.NotSame(schema, proxy.InputSchema);
            Assert.Equal(schema!.ToJsonString(), proxy.InputSchema!.ToJsonString());
        }
    }
}
