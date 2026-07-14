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
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Auth;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// mcp-authorize (design 03 Flow B / 06, D12) machine-credential specs — the once-per-machine sign-in the
    /// sidecar wires on top of McpPlugin 7.0's <c>MachineCredentialStore</c> + <c>PluginCredentialProvider</c>.
    /// Covers: the OAuth refresh-token grant (<see cref="OAuthTokenRefresher"/>); persisting a successful device
    /// sign-in into the shared store; boot auto-adopt of a seeded store (zero-button); Custom-mode precedence over
    /// a cloud credential; and sign-out wiping the store. A per-test temp directory isolates the store from the
    /// real <c>~/.ai-game-dev</c> (and from other tests) — no network, no shared machine state.
    /// </summary>
    public class MachineCredentialTests
    {
        private static string NewStoreDir()
        {
            var dir = Path.Combine(Path.GetTempPath(), "umcp-cred-" + Guid.NewGuid().ToString("N")[..12]);
            Directory.CreateDirectory(dir);
            return dir;
        }

        private static IpcClient NewIpc() => new("127.0.0.1", 39998, token: "ipc-token", sidecarVersion: "0.1.0");

        private static JsonObject CloudConfig(string cloudUrl = "https://ai-game.dev") =>
            new() { ["mode"] = "Cloud", ["cloudUrl"] = cloudUrl };

        // A scripted HTTP handler for the token endpoint: returns one fixed body + status, capturing the request.
        private sealed class RefreshHandler : HttpMessageHandler
        {
            private readonly string _json;
            private readonly HttpStatusCode _status;
            public string? LastUrl { get; private set; }
            public string? LastBody { get; private set; }

            public RefreshHandler(string json, HttpStatusCode status)
            {
                _json = json;
                _status = status;
            }

            protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                LastUrl = request.RequestUri!.AbsoluteUri;
                LastBody = request.Content != null ? await request.Content.ReadAsStringAsync(cancellationToken) : "";
                return new HttpResponseMessage(_status)
                {
                    Content = new StringContent(_json, Encoding.UTF8, "application/json"),
                };
            }
        }

        // --- OAuthTokenRefresher (ITokenRefresher) --------------------------------------------------------------

        [Fact]
        public async Task Refresher_ReturnsRotatedCredential_OnSuccess()
        {
            var handler = new RefreshHandler(
                "{\"access_token\":\"new-jwt\",\"refresh_token\":\"rotated-refresh\",\"token_type\":\"Bearer\",\"expires_in\":3600}",
                HttpStatusCode.OK);
            var refresher = new OAuthTokenRefresher(new HttpClient(handler));

            var result = await refresher.RefreshAsync("old-refresh", "https://ai-game.dev", CancellationToken.None);

            Assert.True(result.Succeeded);
            Assert.Equal("new-jwt", result.AccessToken);
            Assert.Equal("rotated-refresh", result.RefreshToken); // the AS rotates the refresh token (design 03 / a3)
            Assert.NotNull(result.ExpiresAt);

            // OAuth 2.1 refresh grant, form-encoded, to /oauth/token.
            Assert.EndsWith("/oauth/token", handler.LastUrl);
            Assert.Contains("grant_type=refresh_token", handler.LastBody);
            Assert.Contains("refresh_token=old-refresh", handler.LastBody);
            Assert.Contains("client_id=unreal-mcp-plugin", handler.LastBody);
        }

        [Fact]
        public async Task Refresher_ReturnsFailure_OnInvalidGrant()
        {
            // A revoked/expired (or reuse-detected) refresh token comes back as a 400 { error } — surfaced as a
            // failure the provider raises OnSignInRequired for, NOT a thrown exception.
            var handler = new RefreshHandler("{\"error\":\"invalid_grant\"}", HttpStatusCode.BadRequest);
            var refresher = new OAuthTokenRefresher(new HttpClient(handler));

            var result = await refresher.RefreshAsync("stale-refresh", "https://ai-game.dev", CancellationToken.None);

            Assert.False(result.Succeeded);
            Assert.Contains("invalid_grant", result.FailureReason);
        }

        [Fact]
        public async Task Refresher_FailsCleanly_OnMissingInputs()
        {
            var refresher = new OAuthTokenRefresher(new HttpClient(new RefreshHandler("{}", HttpStatusCode.OK)));
            Assert.False((await refresher.RefreshAsync("", "https://ai-game.dev", CancellationToken.None)).Succeeded);
            Assert.False((await refresher.RefreshAsync("r", "", CancellationToken.None)).Succeeded);
        }

        // --- Device-auth commit → machine store (D12) -----------------------------------------------------------

        [Fact]
        public async Task DeviceAuthCommit_PersistsCredentialToStore_AndSignsInProvider()
        {
            var dir = NewStoreDir();
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));
            host.SetStatusEmitterForTest(_ => Task.CompletedTask); // no live IPC socket

            // Cloud mode so the server-target resolves to the cloud base and CurrentBearer prefers the stored JWT.
            host.ApplyConnectionConfig(CloudConfig());

            var result = DeviceAuthResult.Authorized("cloud-jwt", "refresh-1", DateTimeOffset.UtcNow.AddHours(1));
            await host.CommitAuthorizedSessionAsync(result, CancellationToken.None);

            // Persisted to the shared machine store, server target stripped of the /mcp hub prefix.
            var persisted = new MachineCredentialStore(dir);
            Assert.True(persisted.Exists);
            var creds = persisted.Read();
            Assert.Equal("cloud-jwt", creds!.AccessToken);
            Assert.Equal("refresh-1", creds.RefreshToken);
            Assert.Equal("https://ai-game.dev", creds.ServerTarget);

            // Adopted live → provider signed in → the Cloud dial presents the stored JWT (no UI).
            Assert.True(host.CredentialProvider!.IsSignedIn);
            Assert.Equal("cloud-jwt", host.CurrentBearer);
        }

        [Fact]
        public async Task DeviceAuthCommit_WithoutRefreshToken_DoesNotWriteStore()
        {
            // A response missing a refresh token is not a persistable machine credential — don't half-write it.
            var dir = NewStoreDir();
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));
            host.SetStatusEmitterForTest(_ => Task.CompletedTask);
            host.ApplyConnectionConfig(CloudConfig());

            await host.CommitAuthorizedSessionAsync(DeviceAuthResult.Authorized("jwt-only", refreshToken: null), CancellationToken.None);

            Assert.False(new MachineCredentialStore(dir).Exists);
        }

        // --- Boot auto-adopt (zero-button, D12) -----------------------------------------------------------------

        [Fact]
        public void BootAutoAdopt_SeededStore_SignsInWithoutUi_AndDrivesCloudBearer()
        {
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                Version = 1,
                AccessToken = "seeded-jwt",
                RefreshToken = "seeded-refresh",
                ExpiresAt = DateTimeOffset.UtcNow.AddHours(1),
                ServerTarget = "https://ai-game.dev",
            });

            // Fresh sidecar over the SAME store dir (what boot does): the provider auto-loads the seeded credential.
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));
            Assert.True(host.CredentialProvider!.IsSignedIn); // zero-button: no device flow, no UI

            host.ApplyConnectionConfig(CloudConfig());
            Assert.Equal("seeded-jwt", host.CurrentBearer); // the boot credential authenticates the first Cloud dial
        }

        [Fact]
        public void CustomMode_LocalTokenWins_OverSignedInCloudStore()
        {
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                Version = 1,
                AccessToken = "cloud-jwt",
                RefreshToken = "r",
                ExpiresAt = DateTimeOffset.UtcNow.AddHours(1),
                ServerTarget = "https://ai-game.dev",
            });
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));

            // Custom mode with a plugin-pushed local token: the LOCAL bearer is presented, never the cloud JWT.
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "http://localhost:8080", ["token"] = "local-tok" });
            Assert.Equal("local-tok", host.CurrentBearer);
        }

        // --- Sign-out (auth-revoke) wipes the store -------------------------------------------------------------

        [Fact]
        public void AuthRevoke_WipesMachineStore_AndClearsProvider()
        {
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                Version = 1,
                AccessToken = "cloud-jwt",
                RefreshToken = "r",
                ExpiresAt = DateTimeOffset.UtcNow.AddHours(1),
                ServerTarget = "https://ai-game.dev",
            });
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));
            Assert.True(host.CredentialProvider!.IsSignedIn);

            host.HandleAuthMessage(IpcProtocol.Type.AuthRevoke);

            Assert.False(new MachineCredentialStore(dir).Exists);   // the stored refresh token is gone
            Assert.False(host.CredentialProvider!.IsSignedIn);      // and the provider is signed out
        }

        // --- Build wiring smoke (on-401 coordinator + sign-in-required subscription) ----------------------------

        [Fact]
        public void Build_WithCredentialStore_WiresCoordinatorAndSubscription_WithoutThrowing()
        {
            // Build() constructs the ConnectionCredentialCoordinator (on-401 refresh→reconnect) against the real
            // built plugin's IConnection + subscribes to OnSignInRequired. Prove that wiring is inert-safe (no
            // connect, no throw) and that Dispose tears it down cleanly.
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                Version = 1,
                AccessToken = "cloud-jwt",
                RefreshToken = "r",
                ExpiresAt = DateTimeOffset.UtcNow.AddHours(1),
                ServerTarget = "https://ai-game.dev",
            });
            var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));

            host.Build(); // wires the coordinator + OnSignInRequired subscription against the real plugin
            Assert.NotNull(host.CredentialProvider);
            Assert.True(host.CredentialProvider!.IsSignedIn);

            host.Dispose(); // disposes the coordinator + subscription + provider without throwing
        }
    }
}
