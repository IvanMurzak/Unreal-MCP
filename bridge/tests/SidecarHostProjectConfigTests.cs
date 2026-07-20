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
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the mcp-authorize PR 4 project-config IPC channel (design 04/06): the new message round-trips over the
    /// NDJSON wire, and the sidecar resolves a <c>project-config</c> request into the derived {pin, port, serverTarget}
    /// with byte-for-byte <see cref="ProjectIdentity"/> golden-vector parity and the project marker's
    /// <c>portOverride</c> precedence — all without a live socket (mirrors <c>SidecarHostInstanceMetadataTests</c>).
    /// </summary>
    public class SidecarHostProjectConfigTests
    {
        private static SidecarHost BuildHost()
        {
            var ipc = new IpcClient("127.0.0.1", 39996, token: "test-token", sidecarVersion: "0.1.0");
            var host = new SidecarHost(ipc, "0.1.0");
            host.Build();
            return host;
        }

        private static JsonObject Request(string requestId, string? projectPath)
        {
            var node = new JsonObject
            {
                ["type"] = IpcProtocol.Type.ProjectConfig,
                ["requestId"] = requestId,
            };
            if (projectPath != null)
                node["projectPath"] = projectPath;
            return node;
        }

        [Fact]
        public void ProjectConfigMessages_RoundTripOverTheWire()
        {
            var request = new ProjectConfigRequestMessage { RequestId = "req-1", ProjectPath = "/home/user/my-game" };
            var reqJson = JsonSerializer.Serialize(request, IpcProtocol.JsonOptions);
            var reqBack = JsonSerializer.Deserialize<ProjectConfigRequestMessage>(reqJson, IpcProtocol.JsonOptions)!;
            Assert.Equal(IpcProtocol.Type.ProjectConfig, reqBack.Type);
            Assert.Equal("req-1", reqBack.RequestId);
            Assert.Equal("/home/user/my-game", reqBack.ProjectPath);

            var result = new ProjectConfigResultMessage
            {
                RequestId = "req-1",
                Ok = true,
                Pin = "34ea75f2",
                Port = 23940,
                PortIsOverridden = false,
                ServerTarget = "https://ai-game.dev",
            };
            var resJson = JsonSerializer.Serialize(result, IpcProtocol.JsonOptions);
            var resBack = JsonSerializer.Deserialize<ProjectConfigResultMessage>(resJson, IpcProtocol.JsonOptions)!;
            Assert.Equal(IpcProtocol.Type.ProjectConfigResult, resBack.Type);
            Assert.Equal("req-1", resBack.RequestId);
            Assert.True(resBack.Ok);
            Assert.Equal("34ea75f2", resBack.Pin);
            Assert.Equal(23940, resBack.Port);
            Assert.False(resBack.PortIsOverridden);
            Assert.Equal("https://ai-game.dev", resBack.ServerTarget);
        }

        [Fact]
        public void BuildProjectConfigResult_FromRequestPath_ReturnsGoldenPinPort()
        {
            using var host = BuildHost();

            // Same canonical golden vector the resolver suite locks: /home/user/my-game → pin 34ea75f2, port 23940.
            var result = host.BuildProjectConfigResult(Request("r-1", "/home/user/my-game"));

            Assert.True(result.Ok);
            Assert.Equal("r-1", result.RequestId);
            Assert.Equal("34ea75f2", result.Pin);
            Assert.Equal(23940, result.Port);
            Assert.False(result.PortIsOverridden);
            Assert.Null(result.ServerTarget); // no marker present
        }

        [Fact]
        public void BuildProjectConfigResult_MarkerPortOverride_WinsAndSurfacesServerTarget()
        {
            RunInTempDir(dir =>
            {
                new ProjectMarker { ServerTarget = "http://localhost:5383", PortOverride = 25555 }.Write(dir);
                using var host = BuildHost();

                var result = host.BuildProjectConfigResult(Request("r-2", dir));

                Assert.True(result.Ok);
                Assert.Equal(25555, result.Port);              // user override wins over the SHA-derived port
                Assert.True(result.PortIsOverridden);
                Assert.Equal("http://localhost:5383", result.ServerTarget);
            });
        }

        /// <summary>
        /// Precedence level 2 end-to-end through the host (auth-fixes T1 / defect A): the §8 <c>config</c> message
        /// carries the plugin's effective Custom-mode host, and a port the user typed into it becomes the port the
        /// local server binds. Proves the wiring — <c>ApplyConnectionConfig</c> → <c>LocalBindHost</c> →
        /// <c>ProjectConnectionResolver.Resolve</c> — not just the resolver, which is covered per-level in
        /// <c>ProjectConnectionResolverTests</c>.
        /// </summary>
        [Fact]
        public void BuildProjectConfigResult_TypedCustomHostPort_WinsOverDerivedPort()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(new JsonObject
            {
                ["mode"] = "Custom",
                ["host"] = "http://localhost:27618",
                ["cloudUrl"] = "https://ai-game.dev",
            });

            var result = host.BuildProjectConfigResult(Request("r-5", "/home/user/my-game"));

            Assert.True(result.Ok);
            Assert.Equal("34ea75f2", result.Pin);   // the PIN is untouched by the port precedence
            Assert.Equal(27618, result.Port);
            Assert.True(result.PortIsOverridden);   // a user choice, not the derivation
            Assert.NotEqual(23940, result.Port);    // the golden derived port really was displaced
        }

        /// <summary>
        /// Cloud mode contributes no level 2, mirroring the writer's <c>ConnectionMode.Local</c> gate: the written
        /// URL keeps its authority verbatim and no local server runs, so the derived port stands even though the
        /// same message carried a Custom host with an explicit port.
        /// </summary>
        [Fact]
        public void BuildProjectConfigResult_CloudMode_IgnoresTypedCustomHostPort()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(new JsonObject
            {
                ["mode"] = "Cloud",
                ["host"] = "http://localhost:27618",
                ["cloudUrl"] = "https://ai-game.dev",
            });

            var result = host.BuildProjectConfigResult(Request("r-6", "/home/user/my-game"));

            Assert.True(result.Ok);
            Assert.Equal(23940, result.Port);
            Assert.False(result.PortIsOverridden);
        }

        /// <summary>
        /// A blank host clears level 2 back to the derivation — a user who empties the Host field gets the
        /// per-project derived port, not a stale typed one from an earlier push.
        /// </summary>
        [Fact]
        public void BuildProjectConfigResult_BlankCustomHost_FallsBackToDerivedPort()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "http://localhost:27618" });
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "" });

            var result = host.BuildProjectConfigResult(Request("r-7", "/home/user/my-game"));

            Assert.True(result.Ok);
            Assert.Equal(23940, result.Port);
            Assert.False(result.PortIsOverridden);
        }

        [Fact]
        public void BuildProjectConfigResult_FallsBackToHandshakeRoot_WhenRequestOmitsPath()
        {
            using var host = BuildHost();
            // The plugin's handshake-ack already reported the root; a request that omits projectPath resolves from it.
            host.ApplyProjectIdentity("/home/user/my-game");

            var result = host.BuildProjectConfigResult(Request("r-3", projectPath: null));

            Assert.True(result.Ok);
            Assert.Equal("34ea75f2", result.Pin);
            Assert.Equal(23940, result.Port);
        }

        [Fact]
        public void BuildProjectConfigResult_NoProjectPathAnywhere_ReturnsNotOk()
        {
            using var host = BuildHost(); // no ApplyProjectIdentity, request carries no path

            var result = host.BuildProjectConfigResult(Request("r-4", projectPath: null));

            Assert.False(result.Ok);
            Assert.Equal("r-4", result.RequestId);
            Assert.False(string.IsNullOrEmpty(result.Error));
            Assert.Equal(0, result.Port);
        }

        private static void RunInTempDir(Action<string> body)
        {
            var dir = Path.Combine(Path.GetTempPath(), "umcp-pcfg-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            try
            {
                body(dir);
            }
            finally
            {
                try { Directory.Delete(dir, recursive: true); } catch { /* best-effort cleanup */ }
            }
        }
    }
}
