/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Linq;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.ReflectorNet;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;
using McpVersion = com.IvanMurzak.McpPlugin.Common.Version;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the dynamic-registration foundation (docs/ARCHITECTURE.md §2.1) against the REAL pinned
    /// <c>com.IvanMurzak.McpPlugin</c> 6.7.0: a <see cref="ProxyTool"/> registered into the live
    /// <c>ToolManager</c> via <see cref="ManifestRegistrar"/> is listed and is callable end-to-end
    /// (the call routes back through the IPC channel). This is the unit-level analog of the live
    /// <c>ping</c> e2e — no editor required.
    /// </summary>
    public class SidecarHostIntegrationTests
    {
        private static IMcpPlugin BuildPlugin()
        {
            var version = new McpVersion { Api = Consts.ApiVersion, Plugin = "0.1.0", Environment = "test" };
            return new McpPluginBuilder(version)
                .SetConfig(new ConnectionConfig { GenerateSkillFiles = false, KeepConnected = false })
                .Build(new Reflector());
        }

        [Fact]
        public void Manifest_RegistersProxyIntoRealToolManager()
        {
            var plugin = BuildPlugin();
            var tm = plugin.McpManager.ToolManager;
            Assert.NotNull(tm);

            var channel = new FakeToolCallChannel();
            var registrar = new ManifestRegistrar(new ToolManagerSink(tm!), channel);

            registrar.Apply(new ToolManifestMessage
            {
                Revision = 1,
                Tools = { new ToolDescriptor { Name = "ping", Title = "Ping", Description = "Round-trip a ping." } },
            });

            Assert.True(tm!.HasTool("ping"));
            Assert.Contains(tm.GetAllTools(), t => t.Name == "ping");
            Assert.True(tm.EnabledToolsCount >= 1);
        }

        [Fact]
        public async Task RegisteredProxy_IsCallableThroughToolManager()
        {
            var plugin = BuildPlugin();
            var tm = plugin.McpManager.ToolManager!;

            var channel = new FakeToolCallChannel((tool, args) => new ToolResponseMessage
            {
                Status = IpcProtocol.Status.Success,
                Content = JsonNode.Parse("[{\"type\":\"text\",\"text\":\"pong\"}]")!.AsArray(),
                Structured = JsonNode.Parse("{\"result\":\"pong\"}"),
            });
            var registrar = new ManifestRegistrar(new ToolManagerSink(tm), channel);
            registrar.Apply(new ToolManifestMessage { Revision = 1, Tools = { new ToolDescriptor { Name = "ping" } } });

            var ping = tm.GetAllTools().First(t => t.Name == "ping");
            var result = await ping.Run("req-1", null);

            Assert.Equal(ResponseStatus.Success, result.Status);
            Assert.Equal("pong", result.StructuredContent!["result"]!.GetValue<string>());
            Assert.Equal("ping", channel.LastTool);
        }

        [Fact]
        public void RemovedTool_DisappearsFromToolManager()
        {
            var plugin = BuildPlugin();
            var tm = plugin.McpManager.ToolManager!;
            var registrar = new ManifestRegistrar(new ToolManagerSink(tm), new FakeToolCallChannel());

            registrar.Apply(new ToolManifestMessage { Revision = 1, Tools = { new ToolDescriptor { Name = "ping", SchemaHash = "h1" } } });
            Assert.True(tm.HasTool("ping"));

            registrar.Apply(new ToolManifestMessage { Revision = 2, Tools = { } });
            Assert.False(tm.HasTool("ping"));
        }

        [Fact]
        public void SidecarHost_Build_WiresRegistrarAndDefaultsConfig()
        {
            var ipc = new IpcClient("127.0.0.1", 39999, token: "test-token", sidecarVersion: "0.1.0");
            using var host = new SidecarHost(ipc, "0.1.0", fallbackHost: "http://localhost:5170");
            host.Build();

            Assert.NotNull(host.Plugin);
            Assert.NotNull(host.Plugin!.McpManager.ToolManager);
            Assert.NotNull(ipc.Registrar);              // wired before the run loop can deliver a manifest
            Assert.Equal("http://localhost:5170", host.Config.Host);
            Assert.True(host.Config.KeepConnected);
            Assert.False(host.Config.GenerateSkillFiles);
        }
    }
}
