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
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// The per-tool-call timeout (§4 / §8 <c>UNREAL_MCP_TOOL_TIMEOUT</c>): 300000 ms by default, set by the
    /// plugin's <c>config</c> push, read live by every registered proxy. Prompt/resource reads stay short.
    /// </summary>
    public class ToolTimeoutTests
    {
        private static SidecarHost BuildHost()
        {
            var ipc = new IpcClient("127.0.0.1", 39997, token: "test-token", sidecarVersion: "0.1.0");
            var host = new SidecarHost(ipc, "0.1.0");
            host.Build();
            return host;
        }

        [Fact]
        public void Defaults_ToolCallsAreLong_RequestsStayShort()
        {
            Assert.Equal(300000, IpcProtocol.DefaultToolTimeoutMs);
            Assert.Equal(300000, new ToolCallMessage().TimeoutMs);
            Assert.Equal(30000, new PromptGetMessage().TimeoutMs);
            Assert.Equal(30000, new ResourceReadMessage().TimeoutMs);
        }

        [Fact]
        public async Task Proxy_WithoutReader_SendsDefaultToolTimeout()
        {
            var channel = new FakeToolCallChannel();
            var proxy = ProxyToolFactory.Create(new ToolDescriptor { Name = "python-execute" }, channel);

            await proxy.Run("req-1", null);

            Assert.Equal(IpcProtocol.DefaultToolTimeoutMs, channel.LastTimeoutMs);
        }

        [Fact]
        public async Task Proxy_ReadsTimeoutAtCallTime()
        {
            var channel = new FakeToolCallChannel();
            var timeoutMs = 45000;
            var proxy = ProxyToolFactory.Create(new ToolDescriptor { Name = "python-execute" }, channel, () => timeoutMs);

            await proxy.Run("req-1", null);
            Assert.Equal(45000, channel.LastTimeoutMs);

            timeoutMs = 600000; // a later config push
            await proxy.Run("req-2", null);
            Assert.Equal(600000, channel.LastTimeoutMs);
        }

        [Fact]
        public async Task Registrar_PassesTimeoutReaderToProxies()
        {
            var sink = new FakeToolSink();
            var channel = new FakeToolCallChannel();
            var registrar = new ManifestRegistrar(sink, channel, toolTimeoutMs: () => 12345);
            registrar.Apply(new ToolManifestMessage
            {
                Revision = 1,
                Tools = new[] { new ToolDescriptor { Name = "python-execute", SchemaHash = "h" } }.ToList(),
            });

            await sink.Tools["python-execute"].Run("req-1", null);

            Assert.Equal(12345, channel.LastTimeoutMs);
        }

        [Fact]
        public void Host_DefaultsToToolTimeoutDefault()
        {
            using var host = BuildHost();
            Assert.Equal(IpcProtocol.DefaultToolTimeoutMs, host.ToolTimeoutMs);
        }

        [Fact]
        public void Host_AppliesToolTimeoutFromConfig()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["toolTimeoutMs"] = 90000 });
            Assert.Equal(90000, host.ToolTimeoutMs);

            // A partial push without the field keeps the value.
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom" });
            Assert.Equal(90000, host.ToolTimeoutMs);
        }

        [Theory]
        [InlineData(0)]
        [InlineData(-1)]
        public void Host_IgnoresNonPositiveToolTimeout(int value)
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["toolTimeoutMs"] = value });
            Assert.Equal(IpcProtocol.DefaultToolTimeoutMs, host.ToolTimeoutMs);
        }

        [Fact]
        public void Host_IgnoresNonNumericToolTimeout()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["toolTimeoutMs"] = "fast" });
            Assert.Equal(IpcProtocol.DefaultToolTimeoutMs, host.ToolTimeoutMs);
        }
    }
}
