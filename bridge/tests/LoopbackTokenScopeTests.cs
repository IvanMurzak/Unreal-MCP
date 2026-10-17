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
using System.Net.Http;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Custom + oauth: the ai-game.dev account token reaches a loopback host only; any other host gets
    /// <c>UNREAL_MCP_TOKEN</c> or the dial fails (<see cref="LoopbackTokenScope"/>).
    /// </summary>
    public class LoopbackTokenScopeTests : IDisposable
    {
        [Theory]
        [InlineData("http://localhost")]
        [InlineData("http://localhost:8080")]
        [InlineData("https://LOCALHOST:443/hub")]
        [InlineData("http://127.0.0.1:11111")]
        [InlineData("http://127.255.255.254")]
        [InlineData("http://[::1]")]
        [InlineData("http://[::1]:8080")]
        [InlineData("http://[::ffff:127.0.0.1]:8080")]
        public void IsLoopback_True(string url) => Assert.True(LoopbackTokenScope.IsLoopback(url));

        [Theory]
        [InlineData(null)]
        [InlineData("")]
        [InlineData("localhost:8080")]
        [InlineData("https://ai-game.dev")]
        [InlineData("http://evil.com")]
        [InlineData("http://localhost@evil.com")]
        [InlineData("http://user:pass@localhost:8080")]
        [InlineData("http://127.0.0.1.evil.com")]
        [InlineData("http://localhost.evil.com")]
        [InlineData("http://evil.localhost")]
        [InlineData("http://localhost.")]
        [InlineData("http://128.0.0.1")]
        [InlineData("http://0.0.0.0")]
        [InlineData("http://192.168.1.10:8080")]
        [InlineData("http://[::2]")]
        [InlineData("ftp://localhost")]
        public void IsLoopback_False(string? url) => Assert.False(LoopbackTokenScope.IsLoopback(url));

        private readonly string _storeDir = Path.Combine(Path.GetTempPath(), "umcp-scope-" + Guid.NewGuid().ToString("N")[..12]);

        public void Dispose()
        {
            try { Directory.Delete(_storeDir, recursive: true); } catch { /* best-effort cleanup */ }
        }

        /// <summary>A production-wired sidecar over a signed-in temp store; any auth HTTP call fails the test.</summary>
        private SidecarHost SignedInHost(string? envToken = null)
        {
            Directory.CreateDirectory(_storeDir);
            new MachineCredentialStore(_storeDir).Write(new MachineCredentials
            {
                AccessToken = "machine-jwt",
                ServerTarget = "https://ai-game.dev",
                ExpiresAt = DateTimeOffset.UtcNow.AddDays(1),
            });
            var host = SidecarHost.CreateForProduction(
                new IpcClient("127.0.0.1", 39998, token: "ipc-token", sidecarVersion: "0.1.0"), "0.1.0",
                fallbackToken: envToken,
                credentialStore: new MachineCredentialStore(_storeDir),
                authHttpClient: new HttpClient(new NoNetworkHandler()));
            host.Build();
            Assert.True(host.CredentialProvider!.IsSignedIn);
            return host;
        }

        private static JsonObject Custom(string host, string authOption, string token = "", string explicitToken = "") =>
            new()
            {
                ["mode"] = "Custom", ["host"] = host, ["cloudUrl"] = "https://ai-game.dev", ["token"] = token,
                ["authOption"] = authOption, ["explicitToken"] = explicitToken,
            };

        [Theory]
        [InlineData("http://localhost:5200")]
        [InlineData("http://127.0.0.1:5200")]
        [InlineData("http://[::1]:5200")]
        public void CustomOAuth_Loopback_PresentsMachineJwt(string url)
        {
            using var host = SignedInHost(envToken: "env-token");
            host.ApplyConnectionConfig(Custom(url, "oauth"));
            Assert.Equal("machine-jwt", host.CurrentBearer);
        }

        [Theory]
        [InlineData("https://my-server.example.com")]
        [InlineData("http://localhost@evil.com")]
        [InlineData("http://127.0.0.1.evil.com:9000")]
        public void CustomOAuth_Remote_UsesEnvToken_NeverMachineJwt(string url)
        {
            using var host = SignedInHost(envToken: "env-token");
            host.ApplyConnectionConfig(Custom(url, "oauth"));
            Assert.Equal("env-token", host.CurrentBearer);
        }

        [Fact]
        public void CustomOAuth_Remote_WithoutToken_Refuses()
        {
            using var host = SignedInHost();
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", "oauth"));
            var ex = Assert.Throws<CredentialScopeException>(() => host.CurrentBearer);
            Assert.Equal("Refusing to send your ai-game.dev account token to my-server.example.com; " +
                         "set UNREAL_MCP_TOKEN or use a localhost server.", ex.Message);
        }

        [Fact]
        public void CustomNoneOrToken_NeverPresentsMachineJwt()
        {
            using var host = SignedInHost();
            host.ApplyConnectionConfig(Custom("http://localhost:5200", "token", token: "local-secret"));
            Assert.Equal("local-secret", host.CurrentBearer);
            host.ApplyConnectionConfig(Custom("http://localhost:5200", "none"));
            Assert.Null(host.CurrentBearer);
        }

        [Fact]
        public void AuthOptionAbsent_KeepsPreviousValue()
        {
            using var host = SignedInHost();
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", "oauth"));
            host.ApplyConnectionConfig(new JsonObject { ["host"] = "https://other.example.com" }); // partial push
            Assert.Throws<CredentialScopeException>(() => host.CurrentBearer);
        }

        [Fact]
        public void CustomOAuth_Remote_NeverTreatsAStoredCloudBearerAsExplicit()
        {
            using var host = SignedInHost();
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Cloud", ["cloudUrl"] = "https://ai-game.dev", ["token"] = "cloud-bearer" });
            var custom = Custom("https://my-server.example.com", "oauth");
            custom.Remove("token"); // partial push: the stored cloud bearer stays in the sidecar's bearer slot
            host.ApplyConnectionConfig(custom);
            Assert.Throws<CredentialScopeException>(() => host.CurrentBearer);
        }

        [Fact]
        public void CustomOAuth_Remote_UsesPluginExplicitToken_OverEnv()
        {
            using var host = SignedInHost(envToken: "env-token");
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", "oauth", explicitToken: "configured-token"));
            Assert.Equal("configured-token", host.CurrentBearer);
        }

        [Fact]
        public void ProviderBoundToOldHost_NeverSendsMachineJwtThere_AfterHostTurnsLoopback()
        {
            using var host = SignedInHost();
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", "oauth", explicitToken: "configured-token"));
            var remoteLinkProvider = host.Config.CredentialProvider!; // what the live remote HubConnection holds
            host.ApplyConnectionConfig(Custom("http://localhost:5200", "oauth"));
            Assert.Equal("machine-jwt", host.CurrentBearer); // a new link to localhost
            // The old link still checks its own (remote) host: refused, never the machine JWT.
            Assert.Throws<CredentialScopeException>(() => remoteLinkProvider().GetAwaiter().GetResult());
        }

        private sealed class NoNetworkHandler : HttpMessageHandler
        {
            protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
                => throw new InvalidOperationException("unexpected auth HTTP call: " + request.RequestUri);
        }
    }
}
