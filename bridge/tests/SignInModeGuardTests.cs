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
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Auth;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// A sign-in result reaches the connection token slot only while the connection is in Cloud mode at completion
    /// time. A sign-in that completes after a switch to Custom (none / token / oauth) stays in the machine
    /// credential store only, and its commit targets the authorization server the flow started against.
    /// </summary>
    public class SignInModeGuardTests : IDisposable
    {
        private readonly string _storeDir = Path.Combine(Path.GetTempPath(), "umcp-signin-" + Guid.NewGuid().ToString("N")[..12]);

        public void Dispose()
        {
            try { Directory.Delete(_storeDir, recursive: true); } catch { /* best-effort cleanup */ }
        }

        private static IpcClient NewIpc() => new("127.0.0.1", 39998, token: "ipc-token", sidecarVersion: "0.1.0");

        private const string AuthServer = "https://ai-game.dev";

        private static JsonObject Cloud(string token = "") =>
            new() { ["mode"] = "Cloud", ["cloudUrl"] = AuthServer, ["token"] = token };

        private static JsonObject Custom(string host, string authOption, string token = "", string explicitToken = "") =>
            new()
            {
                ["mode"] = "Custom", ["host"] = host, ["cloudUrl"] = AuthServer, ["token"] = token,
                ["authOption"] = authOption, ["explicitToken"] = explicitToken,
            };

        private static SidecarHost NewHost(MachineCredentialStore? store = null, ITokenExchangeClient? exchange = null,
            DeviceCodeAuthenticator? authenticator = null)
        {
            var host = new SidecarHost(NewIpc(), "0.1.0", authenticator: authenticator, credentialStore: store, exchangeClient: exchange);
            host.SetStatusEmitterForTest(_ => Task.CompletedTask);
            host.SetDeviceAuthEmitterForTest(_ => Task.CompletedTask);
            host.LoginDerivationRetryDelayMs = 0;
            return host;
        }

        [Theory]
        [InlineData("none", "", null)]
        [InlineData("token", "local-secret", "local-secret")]
        public async Task SignInCompletingInCustomMode_NeverEntersTheConnectionSlot(string authOption, string token, string? expected)
        {
            using var host = NewHost();
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", authOption, token));

            await host.CommitAuthorizedSessionAsync("account-jwt", CancellationToken.None);

            Assert.Equal(expected, host.CurrentBearer);
        }

        [Fact]
        public async Task SignInCompletingInCustomOAuth_RemoteHost_KeepsTheExplicitToken()
        {
            using var host = NewHost();
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", "oauth", explicitToken: "remote-secret"));

            await host.CommitAuthorizedSessionAsync("account-jwt", CancellationToken.None);

            Assert.Equal("remote-secret", host.CurrentBearer);
        }

        [Fact]
        public async Task SignInCompletingInCloudMode_StillFillsTheSlot()
        {
            using var host = NewHost();
            host.ApplyConnectionConfig(Cloud());

            await host.CommitAuthorizedSessionAsync("account-jwt", CancellationToken.None);

            Assert.Equal("account-jwt", host.CurrentBearer);
        }

        [Fact]
        public void LeavingCloudWithoutAToken_DropsTheCloudBearer()
        {
            using var host = NewHost();
            host.ApplyConnectionConfig(Cloud("account-jwt"));
            Assert.Equal("account-jwt", host.CurrentBearer);

            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "https://my-server.example.com" });

            Assert.Null(host.CurrentBearer);
        }

        [Fact]
        public void AuthRevokeInCustomMode_KeepsTheCustomToken()
        {
            using var host = NewHost();
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", "token", "local-secret"));

            host.HandleAuthMessage(IpcProtocol.Type.AuthRevoke);

            Assert.Equal("local-secret", host.CurrentBearer);
        }

        [Fact]
        public async Task AuthRevokeInCustomMode_RevokesAtTheAccountServer_AndKeepsTheCustomToken()
        {
            // A legacy store without a server target: revocation falls back to the account server, never the Custom host.
            new MachineCredentialStore(_storeDir).Write(new MachineCredentials
            {
                Families = new MachineCredentialFamilies
                {
                    Plugin = new MachineCredentialFamily
                    {
                        AccessToken = "plugin-jwt", RefreshToken = "plugin-refresh",
                        ClientId = "unreal-mcp-plugin", Scope = "mcp:plugin",
                    },
                },
            });
            var revokeHosts = new RecordingHandler();
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(_storeDir),
                authHttpClient: new HttpClient(revokeHosts));
            host.SetStatusEmitterForTest(_ => Task.CompletedTask);
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", "token", "local-secret"));

            host.HandleAuthMessage(IpcProtocol.Type.AuthRevoke);
            await host.PendingSignOut!.WaitAsync(TimeSpan.FromSeconds(10));

            Assert.Equal(new[] { "ai-game.dev/oauth/revoke" }, revokeHosts.Requests);
            Assert.False(new MachineCredentialStore(_storeDir).Exists);
            Assert.False(host.CredentialProvider!.IsSignedIn);
            Assert.Equal("local-secret", host.CurrentBearer);
        }

        [Theory]
        [InlineData("oauth", "http://localhost:8080", true)]
        [InlineData("token", "http://localhost:8080", false)]
        [InlineData("oauth", "https://my-server.example.com", false)]
        public async Task AuthRevoke_RedialsOnlyALinkThatPresentedTheAccountToken(string authOption, string customHost, bool expectRedial)
        {
            new MachineCredentialStore(_storeDir).Write(new MachineCredentials
            {
                ServerTarget = AuthServer,
                Families = new MachineCredentialFamilies
                {
                    Plugin = new MachineCredentialFamily
                    {
                        AccessToken = "plugin-jwt", RefreshToken = "plugin-refresh",
                        ClientId = "unreal-mcp-plugin", Scope = "mcp:plugin",
                    },
                },
            });
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(_storeDir),
                revocationClient: new MachineCredentialTests.FakeRevocationClient());
            var statuses = 0;
            host.SetStatusEmitterForTest(_ => { Interlocked.Increment(ref statuses); return Task.CompletedTask; });
            var config = Custom(customHost, authOption, authOption == "token" ? "local-secret" : "", explicitToken: "remote-secret");
            config["keepConnected"] = false; // the re-dial then only emits its status, no network
            host.ApplyConnectionConfig(config);
            Assert.True(host.CredentialProvider!.IsSignedIn);

            host.HandleAuthMessage(IpcProtocol.Type.AuthRevoke);
            await host.PendingSignOut!.WaitAsync(TimeSpan.FromSeconds(10));

            Assert.False(host.CredentialProvider!.IsSignedIn);
            Assert.Equal(expectRedial ? 1 : 0, statuses);
        }

        private sealed class RecordingHandler : HttpMessageHandler
        {
            public System.Collections.Generic.List<string> Requests { get; } = new();

            protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                lock (Requests)
                    Requests.Add(request.RequestUri!.Host + request.RequestUri.AbsolutePath);
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK));
            }
        }

        [Fact]
        public async Task ModeSwitchDuringTheStoreCommit_KeepsTheAccountTokenInTheMachineStoreOnly()
        {
            var exchange = new MachineCredentialTests.FakeExchangeClient(MachineCredentialTests.PluginExchangeSuccess());
            using var host = NewHost(new MachineCredentialStore(_storeDir), exchange);
            exchange.OnExchange = () => host.ApplyConnectionConfig(Custom("https://my-server.example.com", "token", "local-secret"));
            host.ApplyConnectionConfig(Cloud());

            await host.CommitAuthorizedSessionAsync(
                DeviceAuthResult.Authorized("agent-jwt", "agent-refresh", DateTimeOffset.UtcNow.AddHours(1)),
                AuthServer, CancellationToken.None);

            Assert.Equal("local-secret", host.CurrentBearer);
            var creds = new MachineCredentialStore(_storeDir).Read();
            Assert.Equal("agent-jwt", creds?.Families?.Agent?.AccessToken);
            Assert.Equal(AuthServer, creds!.ServerTarget);
        }

        [Fact]
        public async Task ModeSwitchWhileSignInIsInFlight_CommitsAgainstTheOriginalServer_AndLeavesTheCustomBearer()
        {
            var polling = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var authenticator = new DeviceCodeAuthenticator(
                new HttpClient(new DeviceFlowHandler()),
                delay: async (_, _) =>
                {
                    polling.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                });
            var exchange = new MachineCredentialTests.FakeExchangeClient(MachineCredentialTests.PluginExchangeSuccess());
            using var host = NewHost(new MachineCredentialStore(_storeDir), exchange, authenticator);
            host.ApplyConnectionConfig(Cloud());

            host.HandleAuthMessage(IpcProtocol.Type.AuthStart);
            await polling.Task.WaitAsync(TimeSpan.FromSeconds(10));
            host.ApplyConnectionConfig(Custom("https://my-server.example.com", "token", "local-secret"));
            release.SetResult();
            await host.PendingDeviceAuth!.WaitAsync(TimeSpan.FromSeconds(10));

            Assert.Equal(AuthServer, exchange.LastServerTarget);
            Assert.Equal(AuthServer, new MachineCredentialStore(_storeDir).Read()!.ServerTarget);
            Assert.Equal("local-secret", host.CurrentBearer);
        }

        // The ai-game.dev device flow: authorization succeeds, the first token poll issues the credential. A request
        // to any other host fails the test, so a call sent to the Custom host is caught.
        private sealed class DeviceFlowHandler : HttpMessageHandler
        {
            protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                var url = request.RequestUri!;
                Assert.Equal("ai-game.dev", url.Host);
                var body = url.AbsolutePath.EndsWith("/oauth/device_authorization", StringComparison.Ordinal)
                    ? "{\"device_code\":\"dev-123\",\"user_code\":\"WXYZ-1234\",\"verification_uri\":\"https://ai-game.dev/device\",\"expires_in\":900,\"interval\":1}"
                    : "{\"access_token\":\"agent-jwt\",\"refresh_token\":\"agent-refresh\",\"token_type\":\"Bearer\",\"expires_in\":3600}";
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(body, Encoding.UTF8, "application/json"),
                });
            }
        }
    }
}
