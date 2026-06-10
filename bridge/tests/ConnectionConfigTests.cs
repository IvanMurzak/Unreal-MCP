/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Text.Json.Nodes;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the sidecar's consumption of the §1.3 <c>config</c> / handshake-ack effective connection
    /// config (docs/ARCHITECTURE.md §8): mode-aware host routing (Cloud→cloudUrl, Custom→host), token
    /// application, keepConnected, the env-fallback dev override, and the auth-message stubs. All without
    /// a live socket — <see cref="SidecarHost.ApplyConnectionConfig"/> / <see cref="SidecarHost.HandleAuthMessage"/>
    /// are the testable seams the IPC events route through.
    /// </summary>
    public class ConnectionConfigTests
    {
        private static SidecarHost BuildHost(string? fallbackHost = null, string? fallbackToken = null)
        {
            var ipc = new IpcClient("127.0.0.1", 39998, token: "test-token", sidecarVersion: "0.1.0");
            var host = new SidecarHost(ipc, "0.1.0", fallbackHost: fallbackHost, fallbackToken: fallbackToken);
            host.Build();
            return host;
        }

        private static JsonObject Config(string mode, string? host, string? cloudUrl, string? token, bool? keepConnected = null)
        {
            var obj = new JsonObject { ["mode"] = mode };
            if (host != null) obj["host"] = host;
            if (cloudUrl != null) obj["cloudUrl"] = cloudUrl;
            if (token != null) obj["token"] = token;
            if (keepConnected.HasValue) obj["keepConnected"] = keepConnected.Value;
            return obj;
        }

        [Fact]
        public void CloudMode_RoutesToCloudUrlWithToken()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(Config("Cloud", host: "http://localhost:5200", cloudUrl: "https://ai-game.dev", token: "cloud-bearer"));

            Assert.Equal("https://ai-game.dev", host.Config.Host);
            Assert.Equal("cloud-bearer", host.Config.Token);
        }

        [Fact]
        public void CustomMode_RoutesToHostWithToken()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(Config("Custom", host: "http://localhost:5200", cloudUrl: "https://ai-game.dev", token: "custom-bearer"));

            Assert.Equal("http://localhost:5200", host.Config.Host);
            Assert.Equal("custom-bearer", host.Config.Token);
        }

        [Fact]
        public void CustomMode_EmptyToken_ClearsBearer()
        {
            using var host = BuildHost(fallbackToken: "stale");
            host.ApplyConnectionConfig(Config("Custom", host: "http://localhost:5200", cloudUrl: null, token: ""));

            Assert.Equal("http://localhost:5200", host.Config.Host);
            Assert.Null(host.Config.Token); // Custom+None sends no bearer (plugin resolved token to empty)
        }

        [Fact]
        public void AbsentFields_PreserveEnvFallback()
        {
            // §8: env-fallback stays as the dev override; a config message that omits a field never clobbers it.
            using var host = BuildHost(fallbackHost: "http://localhost:5170", fallbackToken: "env-token");
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom" }); // no host/cloudUrl/token

            Assert.Equal("http://localhost:5170", host.Config.Host);
            Assert.Equal("env-token", host.Config.Token);
        }

        [Fact]
        public void KeepConnected_IsApplied()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(Config("Custom", host: "http://localhost:5200", cloudUrl: null, token: null, keepConnected: false));

            Assert.False(host.Config.KeepConnected);
        }

        [Fact]
        public void NullConfig_IsNoOp()
        {
            using var host = BuildHost(fallbackHost: "http://localhost:5170");
            host.ApplyConnectionConfig(null);

            Assert.Equal("http://localhost:5170", host.Config.Host);
        }

        [Fact]
        public void CloudMode_MissingCloudUrl_FallsBackToHostField()
        {
            // A partial config (mode=Cloud but only `host` present) still resolves a host rather than blanking it.
            using var host = BuildHost();
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Cloud", ["host"] = "http://localhost:5200" });

            Assert.Equal("http://localhost:5200", host.Config.Host);
        }

        [Fact]
        public void AuthRevoke_ClearsStoredToken()
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(Config("Cloud", host: null, cloudUrl: "https://ai-game.dev", token: "cloud-bearer"));
            Assert.Equal("cloud-bearer", host.Config.Token);

            host.HandleAuthMessage(IpcProtocol.Type.AuthRevoke);
            Assert.Null(host.Config.Token);
        }

        [Theory]
        [InlineData(IpcProtocol.Type.AuthStart)]
        [InlineData(IpcProtocol.Type.AuthCancel)]
        public void AuthStartAndCancel_AreGracefulNoOps(string type)
        {
            using var host = BuildHost();
            host.ApplyConnectionConfig(Config("Custom", host: "http://localhost:5200", cloudUrl: null, token: "keep"));

            host.HandleAuthMessage(type); // must not throw and must not touch the bearer
            Assert.Equal("keep", host.Config.Token);
        }
    }
}
