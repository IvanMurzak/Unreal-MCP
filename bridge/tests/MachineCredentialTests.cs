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

            /// <summary>Runs inside each exchange call (e.g. to switch the connection mode mid-commit).</summary>
            public Action? OnExchange { get; set; }

            public string ClientId => DeviceCodeAuthenticator.DefaultClientId;

            public Task<TokenExchangeResult> ExchangeAsync(string subjectAccessToken, string? serverTarget, CancellationToken cancellationToken = default)
            {
                Calls++;
                LastSubjectToken = subjectAccessToken;
                LastServerTarget = serverTarget;
                OnExchange?.Invoke();
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
            await host.CommitAuthorizedSessionAsync(result, "https://ai-game.dev", CancellationToken.None);

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
            await host.CommitAuthorizedSessionAsync(result, "https://ai-game.dev", CancellationToken.None);

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

        // --- Review e1-B1: a failed derivation must never be followed by an "Authorized" status claim ----------

        [Fact]
        public async Task DeviceAuthCommit_FailedDerivation_NeverEmitsAuthorizedStatus()
        {
            // The B1 scenario: the exchange endpoint is down, retries exhaust, the honest "Partially
            // authorized" device-auth frame goes out — and the commit's terminal status fallthrough must NOT
            // carry cloudAuthState "Authorized", because the C++ ApplyStatus LATCHES Authorized and never
            // demotes: an unconditional claim would clobber the failure milliseconds after it surfaced, leaving
            // an "Authorized" light over a hub dial the a4-hardened hub rejects.
            var dir = NewStoreDir();
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir),
                exchangeClient: new FakeExchangeClient(TokenExchangeResult.Failure("exchange down")));
            var statuses = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { statuses.Add(s); return Task.CompletedTask; });
            host.LoginDerivationRetryAttempts = 1;
            host.LoginDerivationRetryDelayMs = 0;
            host.SetDeviceAuthEmitterForTest(_ => Task.CompletedTask);
            host.ApplyConnectionConfig(CloudConfig());

            await host.CommitAuthorizedSessionAsync(
                DeviceAuthResult.Authorized("agent-jwt", "agent-refresh", DateTimeOffset.UtcNow.AddHours(1)),
                "https://ai-game.dev", CancellationToken.None);

            Assert.NotEmpty(statuses); // the terminal status DID go out (the claim below cannot pass vacuously)
            Assert.DoesNotContain(statuses, s => s.CloudAuthState == "Authorized");
        }

        [Fact]
        public async Task DeviceAuthCommit_FullCommit_EmitsAuthorizedStatus()
        {
            // The positive control for the test above: the SAME capture channel observes "Authorized" when the
            // derivation actually completed — proving the failure-path assert fails for the right reason and not
            // because the emitter never carries the indicator.
            var dir = NewStoreDir();
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir),
                exchangeClient: new FakeExchangeClient(PluginExchangeSuccess()));
            var statuses = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { statuses.Add(s); return Task.CompletedTask; });
            host.ApplyConnectionConfig(CloudConfig());

            await host.CommitAuthorizedSessionAsync(
                DeviceAuthResult.Authorized("agent-jwt", "agent-refresh", DateTimeOffset.UtcNow.AddHours(1)),
                "https://ai-game.dev", CancellationToken.None);

            Assert.Contains(statuses, s => s.CloudAuthState == "Authorized");
        }

        [Fact]
        public async Task ConnectStatus_AgentOnlyStore_DoesNotLightAuthorized()
        {
            // Review e1-B1 / design 04 §1 ("Exists alone must never be used as a signed-in signal"): the
            // connect-status indicator must key on the provider's plugin-plane IsSignedIn, never bare store
            // existence. An AGENT-ONLY store — exactly what a failed F1 derivation leaves behind — exists
            // without being signed in, and must not light "Authorized".
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
                },
            });
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));
            Assert.True(new MachineCredentialStore(dir).Exists);        // the store EXISTS…
            Assert.False(host.CredentialProvider!.IsSignedIn);          // …but the plugin plane is NOT signed in

            var statuses = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { statuses.Add(s); return Task.CompletedTask; });
            host.ApplyConnectionConfig(CloudConfig());                  // Cloud mode, no in-session bearer
            host.SetPluginForTest(new FakeMcpPlugin(_ => Task.FromResult(false))); // connect=false skips roster seeding
            await host.ConnectSignalRAsync();

            var status = Assert.Single(statuses);
            Assert.Null(status.CloudAuthState);
        }

        [Fact]
        public async Task ConnectStatus_PluginPlaneStore_LightsAuthorized()
        {
            // Positive control: the SAME connect-status path DOES light "Authorized" for a store the provider is
            // actually signed in on (zero-button boot) — so the agent-only assert above fails for the right reason.
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                ServerTarget = "https://ai-game.dev",
                Families = new MachineCredentialFamilies
                {
                    Plugin = new MachineCredentialFamily
                    {
                        AccessToken = "plugin-jwt", RefreshToken = "plugin-refresh",
                        ExpiresAt = DateTimeOffset.UtcNow.AddHours(1),
                        ClientId = "unreal-mcp-plugin", Scope = "mcp:plugin",
                    },
                },
            });
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir));
            Assert.True(host.CredentialProvider!.IsSignedIn);

            var statuses = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { statuses.Add(s); return Task.CompletedTask; });
            host.ApplyConnectionConfig(CloudConfig());
            host.SetPluginForTest(new FakeMcpPlugin(_ => Task.FromResult(false)));
            await host.ConnectSignalRAsync();

            var status = Assert.Single(statuses);
            Assert.Equal("Authorized", status.CloudAuthState);
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

            await host.CommitAuthorizedSessionAsync(DeviceAuthResult.Authorized("jwt-only", refreshToken: null), "https://ai-game.dev", CancellationToken.None);

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

        // --- DEV-272: a credential is presented only to the server that issued it, and only that server's rejection
        // refreshes it; an in-flight refresh is never cancelled except at host shutdown ----------------------------

        private static string SeedStore(string serverTarget)
        {
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                Version = 1,
                AccessToken = "machine-jwt",
                RefreshToken = "machine-refresh",
                ExpiresAt = DateTimeOffset.UtcNow.AddHours(1),
                ServerTarget = serverTarget,
            });
            return dir;
        }

        // The plugin-plane refresh token on disk (a v1 seed is adopted as families.legacy).
        private static string? StoredRefreshToken(string dir)
        {
            var credentials = new MachineCredentialStore(dir).TryRead().Credentials;
            return credentials?.Families?.Plugin?.RefreshToken ?? credentials?.Families?.Legacy?.RefreshToken;
        }

        private static async Task WaitUntil(Func<bool> condition, string what)
        {
            var deadline = DateTime.UtcNow.AddSeconds(10);
            while (!condition())
            {
                if (DateTime.UtcNow > deadline)
                    throw new TimeoutException(what);
                await Task.Delay(10);
            }
        }

        /// <summary>Counts refresh attempts; succeeds with a rotated pair.</summary>
        internal sealed class CountingRefresher : ITokenRefresher
        {
            private int _calls;
            public int Calls => Volatile.Read(ref _calls);
            public Task<TokenRefreshResult> RefreshAsync(string refreshToken, string? serverTarget, CancellationToken cancellationToken = default)
            {
                Interlocked.Increment(ref _calls);
                return Task.FromResult(TokenRefreshResult.Success("rotated-jwt", "rotated-refresh", DateTimeOffset.UtcNow.AddHours(1)));
            }
        }

        /// <summary>Blocks inside the refresh POST until released, so a test can act while it is in flight.</summary>
        internal sealed class BlockingRefresher : ITokenRefresher
        {
            public readonly TaskCompletionSource Entered = new(TaskCreationOptions.RunContinuationsAsynchronously);
            public readonly TaskCompletionSource Release = new(TaskCreationOptions.RunContinuationsAsynchronously);
            public CancellationToken Token { get; private set; }
            public async Task<TokenRefreshResult> RefreshAsync(string refreshToken, string? serverTarget, CancellationToken cancellationToken = default)
            {
                Token = cancellationToken;
                Entered.TrySetResult();
                await Release.Task; // the server has rotated; only the answer is still on its way
                cancellationToken.ThrowIfCancellationRequested();
                return TokenRefreshResult.Success("rotated-jwt", "rotated-refresh", DateTimeOffset.UtcNow.AddHours(1));
            }
        }

        [Theory]
        [InlineData("https://ai-game.dev", "https://ai-game.dev/mcp", true)]
        [InlineData("https://ai-game.dev/", "https://AI-GAME.dev/mcp/", true)]
        [InlineData(null, "https://ai-game.dev/mcp", true)]                   // no target: the hosted default
        [InlineData("http://127.0.0.1", "http://127.0.0.1:80/mcp", true)]     // the scheme's default port is the same origin
        [InlineData("http://127.0.0.1", "https://ai-game.dev/mcp", false)]    // a stand's local sign-in, Cloud host
        [InlineData("https://ai-game.dev", "http://127.0.0.1/mcp", false)]
        [InlineData("http://127.0.0.1", "http://127.0.0.1:8080/mcp", false)]  // another port is another server
        public void CredentialIssuedFor_ComparesOrigins(string? serverTarget, string dialHost, bool expected)
            => Assert.Equal(expected, SidecarHost.CredentialIssuedFor(serverTarget, dialHost));

        [Fact]
        public void CloudMode_ForeignCredential_IsNeitherPresentedNorRefreshed()
        {
            // StormValley 2026-11-26: headless editors started outside the App (Cloud mode, the default) inherited a
            // development stand's credential directory. ai-game.dev rejected the stand's token and every rejection
            // refreshed the shared family, until the server's reuse detection revoked it for the live editor too.
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(SeedStore("http://127.0.0.1")));
            host.ApplyConnectionConfig(CloudConfig());
            host.Build();

            Assert.True(host.CredentialProvider!.IsSignedIn);
            Assert.Null(host.CurrentBearer);                  // not presented to the host that did not issue it
            Assert.False(host.RejectionRefreshesCredential);  // so a rejection cannot rotate it
            Assert.False(host.SignInRequiredStopsConnection);
            Assert.True(host.HasCredentialCoordinator);       // wired for the host lifetime, gated
        }

        [Fact]
        public void CloudMode_ForeignCredential_SlotAccountTokenIsNotPresentedEither()
        {
            // Review U1: signed in on a development stand (cloudUrl = the stand), cloudUrl switched back to production.
            // The plugin still pushes that sign-in's account token as its CloudToken; it must not reach production.
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(SeedStore("http://127.0.0.1")));
            host.Build();
            var config = CloudConfig();
            config["token"] = "stand-account-jwt";
            host.ApplyConnectionConfig(config);

            Assert.Null(host.CurrentBearer);
        }

        [Fact]
        public void CloudMode_ForeignCredential_ExplicitEnvTokenIsStillPresented()
        {
            // UNREAL_MCP_TOKEN is the user's own choice for this sidecar, not a sign-in's token.
            using var host = new SidecarHost(NewIpc(), "0.1.0", fallbackToken: "env-token",
                credentialStore: new MachineCredentialStore(SeedStore("http://127.0.0.1")));
            host.Build();
            host.ApplyConnectionConfig(CloudConfig());

            Assert.Equal("env-token", host.CurrentBearer);
            Assert.False(host.RejectionRefreshesCredential);
        }

        [Fact]
        public void CloudMode_OwnCredential_IsPresentedAndItsRejectionRefreshes()
        {
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(SeedStore("https://ai-game.dev")));
            host.ApplyConnectionConfig(CloudConfig());
            host.Build();

            Assert.False(host.RejectionRefreshesCredential); // nothing presented yet
            Assert.Equal("machine-jwt", host.CurrentBearer); // the dial
            Assert.True(host.RejectionRefreshesCredential);
            Assert.True(host.SignInRequiredStopsConnection);
        }

        [Fact]
        public void RejectionRefreshes_FollowsAnAdoptedCredentialWithoutAnAuthStateEdge()
        {
            // A sign-in (or a peer's store write) swaps the credential while the provider stays SignedIn: what the next
            // dial presents decides whether a rejection may refresh it.
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(SeedStore("http://127.0.0.1")));
            host.ApplyConnectionConfig(CloudConfig());
            host.Build();
            Assert.Null(host.CurrentBearer);
            Assert.False(host.RejectionRefreshesCredential);

            host.CredentialProvider!.Adopt(new MachineCredentials
            {
                Version = 1, AccessToken = "cloud-jwt", RefreshToken = "cloud-refresh",
                ExpiresAt = DateTimeOffset.UtcNow.AddHours(1), ServerTarget = "https://ai-game.dev",
            });
            Assert.Equal("cloud-jwt", host.CurrentBearer); // the dial
            Assert.True(host.RejectionRefreshesCredential);
        }

        [Fact]
        public void RejectionRefreshes_FollowsTheModeAndHost()
        {
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(SeedStore("http://127.0.0.1")));
            host.Build();
            Assert.False(host.RejectionRefreshesCredential); // no config yet: nothing presents the credential

            // The App's development stand: Custom + oauth on the loopback server that issued the credential.
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "http://127.0.0.1:80/mcp", ["authOption"] = "oauth" });
            Assert.Equal("machine-jwt", host.CurrentBearer);
            Assert.True(host.RejectionRefreshesCredential);
            Assert.True(host.SignInRequiredStopsConnection);

            // Custom with a static token presents that token: a rejection must not refresh the machine credential, and
            // a dead machine sign-in does not stop this link.
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "http://127.0.0.1:80/mcp", ["authOption"] = "token", ["token"] = "local-tok" });
            Assert.False(host.RejectionRefreshesCredential); // the host is unchanged, but the auth option is not
            Assert.Equal("local-tok", host.CurrentBearer);
            Assert.False(host.RejectionRefreshesCredential);
            Assert.False(host.SignInRequiredStopsConnection);

            // Cloud mode on the server that issued it, then on one that did not.
            host.ApplyConnectionConfig(CloudConfig("http://127.0.0.1"));
            Assert.Equal("machine-jwt", host.CurrentBearer);
            Assert.True(host.RejectionRefreshesCredential);
            host.ApplyConnectionConfig(CloudConfig());
            Assert.False(host.RejectionRefreshesCredential); // the last dial went to another host
            Assert.Null(host.CurrentBearer);
            Assert.False(host.RejectionRefreshesCredential);
        }

        [Fact]
        public async Task CustomOAuth_LoopbackServerThatDidNotIssueTheCredential_RejectionNeverRotatesIt()
        {
            // Review U2: a production sign-in presented to a localhost dev stand (the shared Custom + oauth policy sends
            // the account token to any loopback host). The stand rejects it; rotating the production family for that
            // would end in reuse detection revoking it machine-wide.
            var refresher = new CountingRefresher();
            var dir = SeedStore("https://ai-game.dev");
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir), tokenRefresher: refresher);
            host.Build();
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "http://127.0.0.1:5200", ["authOption"] = "oauth" });

            Assert.Equal("machine-jwt", host.CurrentBearer); // presented per the shared policy
            Assert.False(host.RejectionRefreshesCredential);
            host.RejectionGate!.InjectRejectionForTest();
            host.RejectionGate!.InjectRejectionForTest();
            await Task.Delay(200);
            Assert.Equal(0, refresher.Calls);
            Assert.Equal("machine-refresh", StoredRefreshToken(dir));
        }

        [Fact]
        public async Task CloudMode_ForeignCredential_RejectionNeverReachesTheRefresher()
        {
            var refresher = new CountingRefresher();
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(SeedStore("http://127.0.0.1")), tokenRefresher: refresher);
            host.ApplyConnectionConfig(CloudConfig());
            host.Build();

            Assert.Null(host.CurrentBearer);
            host.RejectionGate!.InjectRejectionForTest();
            await Task.Delay(200);
            Assert.Equal(0, refresher.Calls);
        }

        [Theory]
        [InlineData(false)] // a config push: Cloud -> Custom, then the transition's re-dial disconnects first
        [InlineData(true)]  // keepConnected off: the transition only disconnects
        public async Task InFlightRefresh_IsNotCancelledByAConfigPushOrDisconnect_AndItsRotationIsStored(bool disconnectOnly)
        {
            // Review U3 + H1: the refresh POST is in flight when the user switches Cloud -> Custom or disconnects. The
            // coordinator ends its pass on that Disconnect (OnDisconnectRequested); cancelling the POST after the
            // server rotated would drop the new refresh token, and the next refresh would present the old one (reuse
            // detection -> machine-wide revocation). The POST completes and its rotation is stored.
            var dir = SeedStore("https://ai-game.dev");
            var refresher = new BlockingRefresher();
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(dir), tokenRefresher: refresher);
            host.ApplyConnectionConfig(CloudConfig());
            host.Build();
            Assert.Equal("machine-jwt", host.CurrentBearer);

            host.RejectionGate!.InjectRejectionForTest();
            await refresher.Entered.Task.WaitAsync(TimeSpan.FromSeconds(10));

            if (!disconnectOnly)
                host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "http://127.0.0.1:5200", ["authOption"] = "token", ["token"] = "local-tok" });
            await host.Plugin!.Disconnect(); // what ReconnectAsync / HandleDisconnectAsync run first
            Assert.False(refresher.Token.IsCancellationRequested);
            Assert.True(host.HasCredentialCoordinator);

            refresher.Release.SetResult();
            await WaitUntil(() => StoredRefreshToken(dir) == "rotated-refresh", "the rotated refresh token was never stored");
        }

        [Fact]
        public async Task ShutdownOnlyTokenRefresher_IgnoresTheCallersToken()
        {
            var inner = new BlockingRefresher();
            using var shutdown = new CancellationTokenSource();
            var refresher = new ShutdownOnlyTokenRefresher(inner, shutdown.Token);
            using var caller = new CancellationTokenSource();
            caller.Cancel();
            var refresh = ((ITokenRefresher)refresher).RefreshAsync(new TokenRefreshRequest("refresh", "https://ai-game.dev", "client"), caller.Token);
            await inner.Entered.Task.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.False(inner.Token.IsCancellationRequested);
            shutdown.Cancel();
            Assert.True(inner.Token.IsCancellationRequested);
            inner.Release.SetResult();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => refresh);
        }

        [Fact]
        public void RejectionRefreshes_NotAfterAnAdoptedCredentialOfAnotherServer()
        {
            // Review L1: the dial presented the production credential to production; a peer's store write then swaps
            // in a development stand's credential (the state stays SignedIn). The burst must not rotate the new one.
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(SeedStore("https://ai-game.dev")));
            host.ApplyConnectionConfig(CloudConfig());
            host.Build();
            Assert.Equal("machine-jwt", host.CurrentBearer);
            Assert.True(host.RejectionRefreshesCredential);

            host.CredentialProvider!.Adopt(new MachineCredentials
            {
                Version = 1, AccessToken = "stand-jwt", RefreshToken = "stand-refresh",
                ExpiresAt = DateTimeOffset.UtcNow.AddHours(1), ServerTarget = "http://127.0.0.1",
            });
            Assert.False(host.RejectionRefreshesCredential);
        }

        [Fact]
        public async Task CustomOAuth_DeadSignIn_DoesNotStopALinkThatPresentsTheExplicitToken()
        {
            // Review L2: Custom + oauth on loopback with an explicit token. While signed in the dial sends the account
            // token (a dead-family verdict stops that link); once the family is dead it sends the explicit token, and
            // the user's own token link keeps running.
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(SeedStore("http://127.0.0.1:5200")), tokenRefresher: new InvalidGrantRefresher());
            host.Build();
            host.ApplyConnectionConfig(new JsonObject
            {
                ["mode"] = "Custom", ["host"] = "http://127.0.0.1:5200", ["authOption"] = "oauth", ["explicitToken"] = "configured-token",
            });
            Assert.Equal("machine-jwt", host.CurrentBearer);
            Assert.True(host.SignInRequiredStopsConnection);

            Assert.False(await host.CredentialProvider!.RefreshAsync()); // invalid_grant: the family is dead
            Assert.Equal("configured-token", host.CurrentBearer);
            Assert.False(host.SignInRequiredStopsConnection);
        }

        [Fact]
        public void ClosureOfAnOldHost_NeverPresentsTheMachineCredential()
        {
            // Review L3: McpPlugin reads the URL and the credential closure separately, so a HubConnection can pair the
            // new host with the old closure. A closure whose host is no longer the target sends no machine credential.
            using var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(SeedStore("https://ai-game.dev")));
            host.ApplyConnectionConfig(CloudConfig());
            host.Build();
            var oldClosure = host.Config.CredentialProvider!;
            Assert.Equal("machine-jwt", oldClosure().GetAwaiter().GetResult());

            host.ApplyConnectionConfig(CloudConfig("http://127.0.0.1:5300"));
            Assert.Null(oldClosure().GetAwaiter().GetResult());
            Assert.False(host.RejectionRefreshesCredential);
        }

        [Fact]
        public async Task HostDispose_IsTheOnlyThingThatCancelsAnInFlightRefresh()
        {
            var refresher = new BlockingRefresher();
            var host = new SidecarHost(NewIpc(), "0.1.0", credentialStore: new MachineCredentialStore(SeedStore("https://ai-game.dev")), tokenRefresher: refresher);
            host.ApplyConnectionConfig(CloudConfig());
            host.Build();
            Assert.Equal("machine-jwt", host.CurrentBearer);
            host.RejectionGate!.InjectRejectionForTest();
            await refresher.Entered.Task.WaitAsync(TimeSpan.FromSeconds(10));

            host.Dispose();
            Assert.True(refresher.Token.IsCancellationRequested);
            refresher.Release.SetResult();
        }

        // --- oauth-client-error-hygiene d1: dead-family verdict → "SignInRequired" on the status feed ----------

        /// <summary>A refresher whose every attempt is the dead-family verdict (invalid_grant, 04 §3.5).</summary>
        internal sealed class InvalidGrantRefresher : ITokenRefresher
        {
            public Task<TokenRefreshResult> RefreshAsync(string refreshToken, string? serverTarget, CancellationToken cancellationToken = default)
                => Task.FromResult(TokenRefreshResult.Failure("refresh token expired", TokenRefreshFailureKind.InvalidGrant));
        }

        [Fact]
        public async Task TerminalRefreshFailure_EmitsSignInRequiredStatus()
        {
            // d1 contract: SidecarHost.Build wires provider.OnSignInRequired → a status emit carrying
            // cloudAuthState "SignInRequired" (the third documented StatusMessage value). Drive the only
            // signal source — a terminal invalid_grant verdict, confirmed dead by the provider's
            // post-failure re-read — through the REAL subscription, and pin the exact wire spelling the
            // C++ ApplyStatus consumer matches on. Store + refresher are local fakes; no network.
            var dir = NewStoreDir();
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                ServerTarget = "https://ai-game.dev",
                Families = new MachineCredentialFamilies
                {
                    Plugin = new MachineCredentialFamily
                    {
                        AccessToken = "plugin-jwt", RefreshToken = "dead-refresh",
                        ExpiresAt = DateTimeOffset.UtcNow.AddHours(1),
                        ClientId = "unreal-mcp-plugin", Scope = "mcp:plugin",
                    },
                },
            });
            using var host = new SidecarHost(NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(dir),
                tokenRefresher: new InvalidGrantRefresher());
            var statuses = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { statuses.Add(s); return Task.CompletedTask; });
            host.ApplyConnectionConfig(CloudConfig());
            host.Build(); // wires the OnSignInRequired → status subscription

            Assert.True(host.CredentialProvider!.IsSignedIn);
            // Control (both halves): before the verdict, NO status claims SignInRequired — so the positive
            // assert below cannot pass on an emit that predates the refresh failure.
            Assert.DoesNotContain(statuses, s => s.CloudAuthState == "SignInRequired");

            Assert.False(await host.CredentialProvider!.RefreshAsync()); // invalid_grant → dead family

            Assert.Contains(statuses, s => s.CloudAuthState == "SignInRequired");
        }
    }
}
