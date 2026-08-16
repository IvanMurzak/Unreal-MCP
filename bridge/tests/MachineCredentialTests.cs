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
using System.Collections.Generic;
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
    /// unified-machine-auth (design 03 F1/F6, 04, task e1) machine-credential specs — the once-per-machine
    /// sign-in the sidecar wires on top of McpPlugin 8.1's <c>MachineCredentialStore</c> +
    /// <c>PluginCredentialProvider</c> + the b3 helpers. Covers: the F1 two-lock-hold login commit (agent
    /// family + RFC 8693-derived plugin family + v1 mirror); the F1 failure path (exchange fails → agent
    /// family stays committed, failure surfaced over IPC); boot auto-adopt of a seeded store (zero-button);
    /// Custom-mode precedence over a cloud credential; and the F6 machine-wide sign-out (per-family RFC 7009
    /// revocation + lock-protocol delete). A per-test temp directory isolates the store from the real
    /// <c>~/.ai-game-dev</c> (and from other tests) — no network, no shared machine state.
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

        // A fake RFC 8693 exchange client (unified-machine-auth 04 §4): scripts the derivation outcome so the
        // login-commit path runs with no network. Captures the subject token + target it was handed.
        internal sealed class FakeExchangeClient : ITokenExchangeClient
        {
            private readonly Queue<TokenExchangeResult> _results;
            public int Calls { get; private set; }
            public string? LastSubjectToken { get; private set; }
            public string? LastServerTarget { get; private set; }

            public FakeExchangeClient(params TokenExchangeResult[] results) => _results = new Queue<TokenExchangeResult>(results);

            public string ClientId => DeviceCodeAuthenticator.DefaultClientId;

            public Task<TokenExchangeResult> ExchangeAsync(string subjectAccessToken, string? serverTarget, CancellationToken cancellationToken = default)
            {
                Calls++;
                LastSubjectToken = subjectAccessToken;
                LastServerTarget = serverTarget;
                var result = _results.Count > 1 ? _results.Dequeue() : _results.Peek();
                return Task.FromResult(result);
            }
        }

        // A fake RFC 7009 revocation client capturing every (token, clientId) pair presented (F6.2).
        internal sealed class FakeRevocationClient : ITokenRevocationClient
        {
            public List<(string Token, string? ClientId)> Revoked { get; } = new();

            public Task<bool> RevokeAsync(string token, string? clientId, string? serverTarget, CancellationToken cancellationToken = default)
            {
                Revoked.Add((token, clientId));
                return Task.FromResult(true);
            }
        }

        internal static TokenExchangeResult PluginExchangeSuccess(
            string accessToken = "plugin-jwt", string refreshToken = "plugin-refresh", string? subject = "usr_1") =>
            TokenExchangeResult.Success(accessToken, refreshToken, DateTimeOffset.UtcNow.AddHours(1),
                scope: "mcp:plugin", subject: subject, issuedTokenType: "urn:ietf:params:oauth:token-type:access_token");

        // --- F1 login commit → v2 store (agent + plugin families + mirror) --------------------------------------

        [Fact]
        public async Task DeviceAuthCommit_RunsTwoLockHoldCommit_AgentAndPluginFamilies_PlusMirror()
        {
            var dir = NewStoreDir();
            var exchange = new FakeExchangeClient(PluginExchangeSuccess());
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir), exchangeClient: exchange);
            host.SetStatusEmitterForTest(_ => Task.CompletedTask); // no live IPC socket

            // Cloud mode so the server-target resolves to the cloud base and CurrentBearer prefers the stored JWT.
            host.ApplyConnectionConfig(CloudConfig());

            var result = DeviceAuthResult.Authorized("agent-jwt", "agent-refresh", DateTimeOffset.UtcNow.AddHours(1));
            await host.CommitAuthorizedSessionAsync(result, CancellationToken.None);

            // The v2 document (04 §1): agent family stamped with the presented clientId + mcp:agent scope (D8),
            // plugin family from the exchange stamped with the exchanging client's own id + mcp:plugin, the
            // v1 compat mirror re-stamped from the PLUGIN family, and the exchange's `sub` backfilled (O5).
            var creds = new MachineCredentialStore(dir).Read();
            Assert.NotNull(creds);
            Assert.Equal(MachineCredentials.CurrentVersion, creds!.Version);
            Assert.Equal("https://ai-game.dev", creds.ServerTarget); // stripped of the /mcp hub prefix
            Assert.Equal("usr_1", creds.Subject);

            var agent = creds.Families?.Agent;
            Assert.NotNull(agent);
            Assert.Equal("agent-jwt", agent!.AccessToken);
            Assert.Equal("agent-refresh", agent.RefreshToken);
            Assert.Equal(DeviceCodeAuthenticator.DefaultClientId, agent.ClientId);
            Assert.Equal(DeviceCodeAuthenticator.AgentScope, agent.Scope);

            var plugin = creds.Families?.Plugin;
            Assert.NotNull(plugin);
            Assert.Equal("plugin-jwt", plugin!.AccessToken);
            Assert.Equal("plugin-refresh", plugin.RefreshToken);
            Assert.Equal(DeviceCodeAuthenticator.DefaultClientId, plugin.ClientId);
            Assert.Equal("mcp:plugin", plugin.Scope);

            // v1 compat mirror = the plugin family's triple (old readers keep working, 04 §1).
            Assert.Equal("plugin-jwt", creds.AccessToken);
            Assert.Equal("plugin-refresh", creds.RefreshToken);

            // The exchange was fed the AGENT access token (03 F1.4), and the provider adopted the committed
            // document live → the Cloud dial presents the PLUGIN-plane JWT (the hub-audience token).
            Assert.Equal("agent-jwt", exchange.LastSubjectToken);
            Assert.True(host.CredentialProvider!.IsSignedIn);
            Assert.Equal("plugin-jwt", host.CurrentBearer);
        }

        [Fact]
        public async Task DeviceAuthCommit_ExchangeFails_AgentFamilyStaysCommitted_AndFailureSurfacedOverIpc()
        {
            // The F1 failure path (03 F1): the agent family is committed on the FIRST lock hold, so a failed
            // exchange leaves a valid agent credential; the sidecar retries with backoff and then surfaces
            // "partially authorized" status text to the editor UI over the device-auth feed.
            var dir = NewStoreDir();
            var exchange = new FakeExchangeClient(TokenExchangeResult.Failure("exchange down"));
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir), exchangeClient: exchange);
            host.SetStatusEmitterForTest(_ => Task.CompletedTask);
            host.LoginDerivationRetryAttempts = 2;
            host.LoginDerivationRetryDelayMs = 0; // no real waiting
            var frames = new List<DeviceAuthMessage>();
            host.SetDeviceAuthEmitterForTest(m => { frames.Add(m); return Task.CompletedTask; });
            host.ApplyConnectionConfig(CloudConfig());

            var result = DeviceAuthResult.Authorized("agent-jwt", "agent-refresh", DateTimeOffset.UtcNow.AddHours(1));
            await host.CommitAuthorizedSessionAsync(result, CancellationToken.None);

            var creds = new MachineCredentialStore(dir).Read();
            Assert.NotNull(creds?.Families?.Agent);                       // the agent family survived (F1 failure path)
            Assert.Equal("agent-jwt", creds!.Families!.Agent!.AccessToken);
            Assert.Null(creds.Families.Plugin);                           // no plugin family was derived
            Assert.Null(creds.AccessToken);                               // and the v1 mirror has no plugin-plane source

            Assert.Equal(1 + 2, exchange.Calls);                          // initial + the bounded retries
            Assert.Contains(frames, f => f.State == "failed" && f.Message != null
                && f.Message.Contains("Partially authorized", StringComparison.Ordinal));

            // The session itself still connects on the in-memory agent bearer (no signed-in plugin plane).
            Assert.False(host.CredentialProvider!.IsSignedIn);
            Assert.Equal("agent-jwt", host.CurrentBearer);
        }

        [Fact]
        public async Task DeviceAuthCommit_WithoutRefreshToken_DoesNotWriteStore()
        {
            // A response missing a refresh token is not a persistable machine credential — don't half-write it.
            var dir = NewStoreDir();
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir), exchangeClient: new FakeExchangeClient(PluginExchangeSuccess()));
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

        // --- Sign-out (auth-revoke) = F6 machine-wide sign-out --------------------------------------------------

        [Fact]
        public async Task AuthRevoke_RevokesEveryFamilyWithItsStoredClientId_ThenDeletesStore()
        {
            // F6 (unified-machine-auth 03, task e1): sign-out revokes EVERY stored family's refresh token via
            // RFC 7009 — each with the clientId the family was minted under (the component default only for a
            // legacy family of unknown id) — BEFORE the lock-protocol delete unlinks them.
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                ServerTarget = "https://ai-game.dev",
                Families = new MachineCredentialFamilies
                {
                    Agent = new MachineCredentialFamily
                    {
                        AccessToken = "agent-jwt", RefreshToken = "agent-refresh",
                        ClientId = "unreal-mcp-plugin", Scope = "mcp:agent",
                    },
                    Plugin = new MachineCredentialFamily
                    {
                        AccessToken = "plugin-jwt", RefreshToken = "plugin-refresh",
                        ClientId = "some-other-surface", Scope = "mcp:plugin",
                    },
                },
            });
            var revocation = new FakeRevocationClient();
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir), revocationClient: revocation);
            Assert.True(host.CredentialProvider!.IsSignedIn);

            host.HandleAuthMessage(IpcProtocol.Type.AuthRevoke);
            Assert.NotNull(host.PendingSignOut);                    // runs off the IPC reader thread
            await host.PendingSignOut!;

            // Each family was revoked presenting ITS OWN stored clientId (F6.2 — never a blanket component id).
            Assert.Contains(("agent-refresh", (string?)"unreal-mcp-plugin"), revocation.Revoked);
            Assert.Contains(("plugin-refresh", (string?)"some-other-surface"), revocation.Revoked);
            Assert.Equal(2, revocation.Revoked.Count);

            Assert.False(new MachineCredentialStore(dir).Exists);   // the stored refresh tokens are gone (lock-protocol delete)
            Assert.False(host.CredentialProvider!.IsSignedIn);      // and the provider is signed out
        }

        [Fact]
        public async Task AuthRevoke_OfflineRevocationFailure_StillDeletesStoreLocally()
        {
            // F6.4 offline sign-out: revoke calls fail → the store is STILL deleted locally; families die
            // naturally server-side (≤30 d) and remain listed on the website until then.
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                ServerTarget = "https://ai-game.dev",
                Families = new MachineCredentialFamilies
                {
                    Plugin = new MachineCredentialFamily
                    {
                        AccessToken = "plugin-jwt", RefreshToken = "plugin-refresh",
                        ClientId = "unreal-mcp-plugin", Scope = "mcp:plugin",
                    },
                },
            });
            var failing = new ThrowingRevocationClient();
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir), revocationClient: failing);

            host.HandleAuthMessage(IpcProtocol.Type.AuthRevoke);
            await host.PendingSignOut!;

            Assert.False(new MachineCredentialStore(dir).Exists);
            Assert.False(host.CredentialProvider!.IsSignedIn);
        }

        internal sealed class ThrowingRevocationClient : ITokenRevocationClient
        {
            public Task<bool> RevokeAsync(string token, string? clientId, string? serverTarget, CancellationToken cancellationToken = default)
                => Task.FromException<bool>(new HttpRequestException("offline"));
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
