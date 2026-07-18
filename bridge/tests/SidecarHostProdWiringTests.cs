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
    /// B14 regression (design 01 §7d / auth-fixes V11) — the PRODUCTION sidecar wiring
    /// (<see cref="SidecarHost.CreateForProduction"/>, the exact path <see cref="Program"/> boots through)
    /// ALWAYS wires the shared machine credential store + token refresher, so a Cloud sign-in can never silently
    /// degrade to a static bearer without refresh. Before B14 the store was optional on the raw constructor and
    /// only wired if the entry point remembered to pass it (`if (credentialStore != null)`); the factory now makes
    /// that structural. A per-test temp store isolates these from the real <c>~/.ai-game-dev</c> — no network, no
    /// shared machine state.
    /// </summary>
    public class SidecarHostProdWiringTests
    {
        private static string NewStoreDir()
        {
            var dir = Path.Combine(Path.GetTempPath(), "umcp-prodwire-" + Guid.NewGuid().ToString("N")[..12]);
            Directory.CreateDirectory(dir);
            return dir;
        }

        private static IpcClient NewIpc() => new("127.0.0.1", 39998, token: "ipc-token", sidecarVersion: "0.1.0");

        private static JsonObject CloudConfig(string cloudUrl = "https://ai-game.dev") =>
            new() { ["mode"] = "Cloud", ["cloudUrl"] = cloudUrl };

        // A scripted HTTP handler for the /oauth/token refresh endpoint: returns one fixed body + status.
        private sealed class RefreshHandler : HttpMessageHandler
        {
            private readonly string _json;
            private readonly HttpStatusCode _status;
            public int Calls { get; private set; }

            public RefreshHandler(string json, HttpStatusCode status)
            {
                _json = json;
                _status = status;
            }

            protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                Calls++;
                return Task.FromResult(new HttpResponseMessage(_status)
                {
                    Content = new StringContent(_json, Encoding.UTF8, "application/json"),
                });
            }
        }

        [Fact]
        public void CreateForProduction_AlwaysWiresCredentialStore_EvenWithNoExplicitStore()
        {
            // The B14 core assertion: the prod factory NEVER yields an unwired sidecar. Even with NO explicit store
            // — the exact shape a future Program.cs edit that dropped the store arg would hit — the machine-credential
            // provider is wired, so Cloud mode presents the refreshable credential rather than a static-only bearer.
            // Asserts only the wiring (creds-independent), so it holds on CI (no credential file) and locally alike.
            using var host = SidecarHost.CreateForProduction(NewIpc(), "0.1.0");
            Assert.NotNull(host.CredentialProvider);
        }

        [Fact]
        public void CreateForProduction_WithSeededStore_DrivesCloudBearerFromStore_NotStatic()
        {
            // Prod wiring auto-adopts a seeded machine credential (zero-button boot, D12) and drives the Cloud bearer
            // from it — proving the store the factory wires actually feeds the connection layer's credential provider.
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                Version = 1,
                AccessToken = "seeded-jwt",
                RefreshToken = "seeded-refresh",
                ExpiresAt = DateTimeOffset.UtcNow.AddHours(1),
                ServerTarget = "https://ai-game.dev",
            });

            using var host = SidecarHost.CreateForProduction(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));
            Assert.NotNull(host.CredentialProvider);
            Assert.True(host.CredentialProvider!.IsSignedIn);

            host.ApplyConnectionConfig(CloudConfig());
            Assert.Equal("seeded-jwt", host.CurrentBearer);
        }

        [Fact]
        public async Task CreateForProduction_RefreshPath_RotatesExpiredStoredCredential()
        {
            // Exercise the refresh path through the PROD wiring: a stale (past-exp) stored credential is refreshed
            // via the wired token refresher before the connection layer is handed a bearer. A static-only sidecar
            // (the B14 defect) would instead present the stale token forever with no refresh.
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                Version = 1,
                AccessToken = "stale-jwt",
                RefreshToken = "old-refresh",
                ExpiresAt = DateTimeOffset.UtcNow.AddMinutes(-5), // already expired → forces a refresh
                ServerTarget = "https://ai-game.dev",
            });

            var handler = new RefreshHandler(
                "{\"access_token\":\"rotated-jwt\",\"refresh_token\":\"new-refresh\",\"token_type\":\"Bearer\",\"expires_in\":3600}",
                HttpStatusCode.OK);
            var refresher = new OAuthTokenRefresher(new HttpClient(handler));

            using var host = SidecarHost.CreateForProduction(
                NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir),
                tokenRefresher: refresher);
            host.ApplyConnectionConfig(CloudConfig());

            var bearer = await Task.Run(() => host.CurrentBearer);

            Assert.True(handler.Calls >= 1, "the wired refresher must be invoked to rotate the expired credential");
            Assert.Equal("rotated-jwt", bearer);
        }
    }
}
