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
using System.Net;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Auth;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Device-code flow specs (docs/ARCHITECTURE.md §7 item 4 / §1.3) — the real RFC 8628 grant the sidecar
    /// now runs in place of PR #8's stub. Drives <see cref="DeviceCodeAuthenticator"/> against a scripted HTTP
    /// handler (no network, no real waiting): asserts it emits a pending device-auth with the verification URL
    /// + user code, polls until an access token is issued, surfaces denial/expiry, and honours cancellation.
    /// </summary>
    public class DeviceCodeAuthenticatorTests
    {
        // A scripted handler: the /authorize call returns a fixed JSON; each /token call dequeues the next body.
        private sealed class ScriptedHandler : HttpMessageHandler
        {
            private readonly string _authorizeJson;
            private readonly Queue<string> _tokenJson;
            public int AuthorizeCalls { get; private set; }
            public int TokenCalls { get; private set; }

            public ScriptedHandler(string authorizeJson, IEnumerable<string> tokenJson)
            {
                _authorizeJson = authorizeJson;
                _tokenJson = new Queue<string>(tokenJson);
            }

            protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                var url = request.RequestUri!.AbsoluteUri;
                string body;
                if (url.EndsWith("/api/auth/device/authorize", StringComparison.Ordinal))
                {
                    AuthorizeCalls++;
                    body = _authorizeJson;
                }
                else
                {
                    TokenCalls++;
                    body = _tokenJson.Count > 0 ? _tokenJson.Dequeue() : "{\"error\":\"authorization_pending\"}";
                }
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(body, System.Text.Encoding.UTF8, "application/json"),
                });
            }
        }

        private static DeviceCodeAuthenticator MakeAuth(ScriptedHandler handler) =>
            // Zero delay so polling does not wait real seconds.
            new(new HttpClient(handler), logger: null, delay: (_, _) => Task.CompletedTask);

        private const string AuthorizeJson =
            "{\"device_code\":\"dev-123\",\"user_code\":\"WXYZ-1234\"," +
            "\"verification_uri\":\"https://ai-game.dev/device\"," +
            "\"verification_uri_complete\":\"https://ai-game.dev/device?code=WXYZ-1234\"," +
            "\"expires_in\":900,\"interval\":1}";

        [Fact]
        public async Task HappyPath_EmitsPendingThenAuthorizesAndReturnsToken()
        {
            var handler = new ScriptedHandler(AuthorizeJson, new[]
            {
                "{\"error\":\"authorization_pending\"}",
                "{\"error\":\"authorization_pending\"}",
                "{\"access_token\":\"cloud-bearer-abc\",\"token_type\":\"Bearer\"}",
            });
            var auth = MakeAuth(handler);

            var emitted = new List<DeviceAuthMessage>();
            var result = await auth.AuthorizeAsync("https://ai-game.dev", "Unreal", m => { emitted.Add(m); return Task.CompletedTask; }, CancellationToken.None);

            Assert.True(result.Success);
            Assert.Equal("cloud-bearer-abc", result.Token);

            // First emit is the pending state with the verification URL + user code (what the UI renders, §7).
            Assert.Contains(emitted, m => m.State == "pending" && m.UserCode == "WXYZ-1234"
                && m.VerificationUrl == "https://ai-game.dev/device?code=WXYZ-1234");
            // The authorized emit carries the issued token.
            Assert.Contains(emitted, m => m.State == "authorized" && m.Token == "cloud-bearer-abc");
            Assert.Equal(3, handler.TokenCalls); // pending, pending, success
        }

        [Fact]
        public async Task SlowDown_IncreasesIntervalButStillSucceeds()
        {
            var handler = new ScriptedHandler(AuthorizeJson, new[]
            {
                "{\"error\":\"slow_down\"}",
                "{\"access_token\":\"tok\"}",
            });
            var auth = MakeAuth(handler);

            var result = await auth.AuthorizeAsync("https://ai-game.dev", null, _ => Task.CompletedTask, CancellationToken.None);
            Assert.True(result.Success);
            Assert.Equal("tok", result.Token);
        }

        [Fact]
        public async Task AccessDenied_FailsAndEmitsFailed()
        {
            var handler = new ScriptedHandler(AuthorizeJson, new[] { "{\"error\":\"access_denied\"}" });
            var auth = MakeAuth(handler);

            var emitted = new List<DeviceAuthMessage>();
            var result = await auth.AuthorizeAsync("https://ai-game.dev", null, m => { emitted.Add(m); return Task.CompletedTask; }, CancellationToken.None);

            Assert.False(result.Success);
            Assert.Equal("access_denied", result.Error);
            Assert.Contains(emitted, m => m.State == "failed");
        }

        [Fact]
        public async Task Cancellation_StopsPollingWithCancelledResult()
        {
            // An endless stream of authorization_pending — only cancellation ends it.
            var pending = new List<string>();
            for (var i = 0; i < 100; i++) pending.Add("{\"error\":\"authorization_pending\"}");
            var handler = new ScriptedHandler(AuthorizeJson, pending);

            using var cts = new CancellationTokenSource();
            // Cancel after the first poll by counting emits is racy; instead inject a delay that cancels.
            var auth = new DeviceCodeAuthenticator(new HttpClient(handler), logger: null,
                delay: (_, ct) => { cts.Cancel(); return Task.CompletedTask; });

            var result = await auth.AuthorizeAsync("https://ai-game.dev", null, _ => Task.CompletedTask, cts.Token);
            Assert.False(result.Success);
            Assert.True(result.WasCancelled);
        }

        [Fact]
        public async Task ExpiresIn_StopsPollingWithExpiredFailure()
        {
            // A token endpoint that never resolves (endless authorization_pending) must NOT poll forever: once
            // the client-side expires_in deadline is breached the flow terminates with expired_token. The clock
            // is injected so the deadline is reached deterministically without real waiting.
            const string authorizeJson =
                "{\"device_code\":\"dev-123\",\"user_code\":\"WXYZ-1234\"," +
                "\"verification_uri\":\"https://ai-game.dev/device\",\"expires_in\":10,\"interval\":1}";
            var pending = new List<string>();
            for (var i = 0; i < 100; i++) pending.Add("{\"error\":\"authorization_pending\"}");
            var handler = new ScriptedHandler(authorizeJson, pending);

            var start = DateTimeOffset.UtcNow;
            var clockReads = 0;
            // First read sets the deadline (start + 10 s); subsequent reads jump 20 s past it so the very first
            // poll iteration sees the deadline breached.
            var auth = new DeviceCodeAuthenticator(new HttpClient(handler), logger: null,
                delay: (_, _) => Task.CompletedTask,
                now: () => clockReads++ == 0 ? start : start.AddSeconds(20));

            var emitted = new List<DeviceAuthMessage>();
            var result = await auth.AuthorizeAsync("https://ai-game.dev", null, m => { emitted.Add(m); return Task.CompletedTask; }, CancellationToken.None);

            Assert.False(result.Success);
            Assert.Equal("expired_token", result.Error);
            Assert.Contains(emitted, m => m.State == "failed");
        }

        [Fact]
        public async Task EmptyCloudUrl_FailsImmediately()
        {
            var handler = new ScriptedHandler(AuthorizeJson, Array.Empty<string>());
            var auth = MakeAuth(handler);
            var result = await auth.AuthorizeAsync("", null, _ => Task.CompletedTask, CancellationToken.None);
            Assert.False(result.Success);
            Assert.Equal(0, handler.AuthorizeCalls);
        }

        [Fact]
        public async Task CommitAuthorizedSession_CancelledFlow_DoesNotResurrectToken()
        {
            // The cancel race (docs/ARCHITECTURE.md §7 item 4): an auth-cancel/auth-revoke can land between the
            // device-code flow's final successful poll and the commit. The commit MUST NOT resurrect a bearer the
            // user just cancelled/revoked. (Port 39998 is never dialed — the guard returns before SignalR.)
            var ipc = new IpcClient("127.0.0.1", 39998, token: "ipc-token", sidecarVersion: "0.1.0");
            using var host = new SidecarHost(ipc, "0.1.0");

            using var cts = new CancellationTokenSource();
            cts.Cancel();
            await host.CommitAuthorizedSessionAsync("cloud-bearer-abc", cts.Token);
            Assert.Null(host.Config.Token); // the guard bailed — the just-cancelled bearer is NOT stored

            // Control: an uncancelled commit DOES store the bearer, proving the guard (not a no-op) blocks it.
            await host.CommitAuthorizedSessionAsync("cloud-bearer-abc", CancellationToken.None);
            Assert.Equal("cloud-bearer-abc", host.Config.Token);
        }

        [Theory]
        // expected uses ConfigTransition's underlying values (None=0, Disconnect=1, Reconnect=2) — the internal
        // enum cannot appear in this public xUnit signature, so it is cast to int inside the body.
        // keepConnected edges: armed→disarmed tears down; disarmed→armed (re)connects.
        [InlineData(true, false, false, (int)SidecarHost.ConfigTransition.Disconnect)]
        [InlineData(false, true, false, (int)SidecarHost.ConfigTransition.Reconnect)]
        // Still armed + host/token changed → re-dial so SignalR leaves the stale endpoint (the §7 medium fix).
        [InlineData(true, true, true, (int)SidecarHost.ConfigTransition.Reconnect)]
        // Still armed, nothing changed → no churn; not armed + host changed → no connect.
        [InlineData(true, true, false, (int)SidecarHost.ConfigTransition.None)]
        [InlineData(false, false, true, (int)SidecarHost.ConfigTransition.None)]
        public void DecideConfigTransition_CoversTheKeepConnectedAndEndpointChangeMatrix(
            bool wasKeepConnected, bool nowKeepConnected, bool hostOrTokenChanged, int expected)
        {
            Assert.Equal(expected, (int)SidecarHost.DecideConfigTransition(wasKeepConnected, nowKeepConnected, hostOrTokenChanged));
        }

        [Theory]
        // Armed re-dial reports the ACTUAL connect outcome — never the optimistic "Connecting…" left stuck after
        // a successful re-dial (the §7 medium fix for the OnConfigReceived Reconnect path + the device-auth commit).
        [InlineData(true, true, "Connected")]
        [InlineData(true, false, "Connecting")]
        // Disarmed (the user clicked Disconnect before authorizing): the commit stores the bearer but NEVER claims
        // a live link — "Disconnected" — so Disconnect stays genuine and no un-tearable link is built.
        [InlineData(false, false, "Disconnected")]
        [InlineData(false, true, "Disconnected")]
        public void ResolveConnectionStatusAfterReconnect_HonestStatusAndDisarmedNeverConnected(
            bool keepConnected, bool connected, string expected)
        {
            Assert.Equal(expected, SidecarHost.ResolveConnectionStatusAfterReconnect(keepConnected, connected));
        }

        [Fact]
        public async Task CommitAuthorizedSession_WhileDisarmed_StoresTokenWithoutRearming()
        {
            // The disarmed-authorize path (docs/ARCHITECTURE.md §7): the user clicked Disconnect (keepConnected=
            // false persisted) and then authorized. The commit MUST store the issued bearer but MUST NOT silently
            // re-arm/claim Connected — otherwise it builds a SignalR link Disconnect (which maps a false→false
            // config push to None) could never tear down. With no live plugin a dial is a no-op, so the no-dial
            // status contract is locked by ResolveConnectionStatusAfterReconnect above; here we assert the disarmed
            // commit still COMMITS the token and leaves keepConnected false. (Port 39998 is never dialed.)
            var ipc = new IpcClient("127.0.0.1", 39998, token: "ipc-token", sidecarVersion: "0.1.0");
            using var host = new SidecarHost(ipc, "0.1.0");
            host.Config.KeepConnected = false;

            await host.CommitAuthorizedSessionAsync("cloud-bearer-abc", CancellationToken.None);

            Assert.Equal("cloud-bearer-abc", host.Config.Token); // the authorized bearer is stored even while disarmed
            Assert.False(host.Config.KeepConnected); // and the disarmed intent is untouched — no silent re-arm
        }
    }
}
