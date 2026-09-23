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
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the §7 agent-config IPC dispatch in <see cref="SidecarHost"/>: a raw NDJSON request line (exactly the
    /// shape the C++ plugin frames) deserializes through <see cref="IpcProtocol.JsonOptions"/> into the right
    /// concrete request, routes to the matching service handler, and produces a correlated
    /// <c>agent-config-result</c>. This is the seam the reader-thread dispatch + IPC send wrap around — locking it
    /// here means the wire contract is exercised without standing up a live socket.
    /// </summary>
    public class AgentConfigDispatchTests
    {
        private static SidecarHost BuildHost()
        {
            var ipc = new IpcClient("127.0.0.1", 39997, token: "test-token", sidecarVersion: "0.1.0");
            var host = new SidecarHost(ipc, "0.1.0");
            host.Build();
            return host;
        }

        private static JsonObject Parse(object message)
        {
            // Round-trip through the SAME serializer options the wire uses, so the test exercises the real
            // camelCase property mapping the plugin's JSON relies on.
            var json = JsonSerializer.Serialize(message, IpcProtocol.JsonOptions);
            return (JsonObject)JsonNode.Parse(json)!;
        }

        [Fact]
        public async Task Dispatch_AgentsList_RoutesAndReturnsCorrelatedResult()
        {
            using var host = BuildHost();
            var request = new AgentsListRequestMessage
            {
                RequestId = "req-1",
                Transport = "streamableHttp",
                Settings = new AgentSettingsDto { ProjectRootPath = "C:/tmp/proj", Host = "http://localhost:1/mcp" },
            };

            var result = await host.ServeAgentConfigRequestAsync(IpcProtocol.Type.AgentsList, Parse(request));

            Assert.Equal("req-1", result.RequestId);
            Assert.Equal(IpcProtocol.Type.AgentsList, result.Op);
            Assert.True(result.Ok);
            Assert.NotNull(result.Agents);
            Assert.Equal(16, result.Agents!.Count);
        }

        [Fact]
        public async Task Dispatch_AgentStatus_RoutesToTheNamedAgent()
        {
            using var host = BuildHost();
            var request = new AgentStatusRequestMessage
            {
                RequestId = "req-2",
                AgentId = "cursor",
                Transport = "streamableHttp",
                Settings = new AgentSettingsDto { ProjectRootPath = "C:/tmp/proj", Host = "http://localhost:1/mcp" },
            };

            var result = await host.ServeAgentConfigRequestAsync(IpcProtocol.Type.AgentStatus, Parse(request));

            Assert.Equal("req-2", result.RequestId);
            Assert.True(result.Ok);
            Assert.Equal("cursor", result.Description!.AgentId);
        }

        [Fact]
        public async Task Dispatch_AgentRegenerateKey_Routes_AndFailsCleanlyWhenSignedOut()
        {
            // No machine credential store is wired here (the static-bearer test seam) — i.e. signed out — so the
            // regenerate is refused with a correlated, non-throwing result instead of minting anything.
            using var host = BuildHost();
            var request = new AgentRegenerateKeyRequestMessage
            {
                RequestId = "req-4",
                AgentId = "claude-code",
                Settings = new AgentSettingsDto { ProjectRootPath = "C:/tmp/proj", Host = "https://ai-game.dev/mcp", ConnectionMode = "Cloud" },
            };

            var result = await host.ServeAgentConfigRequestAsync(IpcProtocol.Type.AgentRegenerateKey, Parse(request));

            Assert.Equal("req-4", result.RequestId);
            Assert.Equal(IpcProtocol.Type.AgentRegenerateKey, result.Op);
            Assert.False(result.Ok);
            Assert.Equal(ProjectKeyStatus.SignedOut, result.ProjectKeyStatus);
        }

        [Theory]
        [InlineData(null, "https://ai-game.dev")]
        [InlineData("", "https://ai-game.dev")]
        [InlineData("not a url", "https://ai-game.dev")]
        [InlineData("https://AI-Game.dev:443/mcp/", "https://ai-game.dev")]
        [InlineData("http://agd.localhost/", "http://agd.localhost")]
        public void ProjectKeyIssuer_IsTheCredentialServerTargetOrigin(string? serverTarget, string expected)
        {
            Assert.Equal(expected, SidecarHost.ProjectKeyIssuer(serverTarget));
        }

        [Fact]
        public async Task Dispatch_UnknownType_ReturnsFailureResultNotThrow()
        {
            using var host = BuildHost();
            var node = new JsonObject { ["type"] = "agent-bogus", ["requestId"] = "req-3" };

            var result = await host.ServeAgentConfigRequestAsync("agent-bogus", node);

            Assert.Equal("req-3", result.RequestId);
            Assert.False(result.Ok);
            Assert.NotNull(result.Error);
        }
    }
}
