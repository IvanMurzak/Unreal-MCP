/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Microsoft.AspNetCore.SignalR.Client;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Regression specs for issue #116 bug 2 (bridge half): "stopping the local MCP server doesn't disconnect the
    /// editor". Before the fix the sidecar re-emitted <c>status</c> only on the editor's OWN connection/auth
    /// transitions (config push, device-auth) and on roster changes — so a transport-level DROP (the user stopped the
    /// local gamedev-mcp-server the editor was connected to → McpPlugin's SignalR client falls Connected →
    /// Reconnecting/Disconnected with no config push) never reached the plugin, leaving the "Unreal" dot stale-green.
    /// The fix subscribes to <see cref="IConnection.ConnectionState"/> and emits a fresh non-green <c>status</c> on a
    /// drop. These lock both the pure edge-decision matrix and the wired emit through a fake whose ConnectionState the
    /// test drives (no live SignalR client).
    /// </summary>
    public class SidecarHostConnectionDropTests
    {
        private static SidecarHost NewHost(out IpcClient ipc)
        {
            // Port is never dialed — the fake plugin replaces the real SignalR client, so no socket opens.
            ipc = new IpcClient("127.0.0.1", 39996, token: "ipc-token", sidecarVersion: "0.1.0");
            return new SidecarHost(ipc, "0.1.0");
        }

        private static FakeMcpPlugin NewFake() =>
            new(onConnect: _ => Task.FromResult(true));

        // ── the pure edge-decision matrix ────────────────────────────────────────────────────────────────────

        [Fact]
        public void ShouldEmit_OnDropOffConnected_EmitsDisconnected()
        {
            // A drop off Connected emits "Disconnected" — the plugin's ParseConnectionState folds that into amber
            // Degraded while armed (no amber→blue flip vs. the editor's optimistic post-Stop Degraded).
            var emit = SidecarHost.ShouldEmitOnConnectionStateChange(
                (int)HubConnectionState.Connected, (int)HubConnectionState.Reconnecting, out var state);
            Assert.True(emit);
            Assert.Equal("Disconnected", state);

            // A hard drop to Disconnected is also an off-Connected edge.
            Assert.True(SidecarHost.ShouldEmitOnConnectionStateChange(
                (int)HubConnectionState.Connected, (int)HubConnectionState.Disconnected, out var state2));
            Assert.Equal("Disconnected", state2);
        }

        [Fact]
        public void ShouldEmit_OnRecoveryIntoConnected_EmitsConnected()
        {
            Assert.True(SidecarHost.ShouldEmitOnConnectionStateChange(
                (int)HubConnectionState.Reconnecting, (int)HubConnectionState.Connected, out var state));
            Assert.Equal("Connected", state);
        }

        [Fact]
        public void ShouldEmit_OnBaselineOrSameStateOrNonConnectedChurn_DoesNotEmit()
        {
            // Baseline first observation (previous < 0): the property publishes its current value on subscribe — this
            // establishes the latch, it is NOT a drop.
            Assert.False(SidecarHost.ShouldEmitOnConnectionStateChange(-1, (int)HubConnectionState.Disconnected, out _));
            Assert.False(SidecarHost.ShouldEmitOnConnectionStateChange(-1, (int)HubConnectionState.Connected, out _));

            // Same-state re-publish: no edge.
            Assert.False(SidecarHost.ShouldEmitOnConnectionStateChange(
                (int)HubConnectionState.Connected, (int)HubConnectionState.Connected, out _));

            // Non-Connected churn (Connecting↔Reconnecting): the green/non-green dot does not change.
            Assert.False(SidecarHost.ShouldEmitOnConnectionStateChange(
                (int)HubConnectionState.Connecting, (int)HubConnectionState.Reconnecting, out _));
            Assert.False(SidecarHost.ShouldEmitOnConnectionStateChange(
                (int)HubConnectionState.Disconnected, (int)HubConnectionState.Connecting, out _));
        }

        // ── the wired subscription: a real drop emits a fresh non-green status ────────────────────────────────

        [Fact]
        public void TransportDrop_AfterConnected_EmitsAFreshNonGreenStatus()
        {
            using var host = NewHost(out _);
            var fake = NewFake();
            host.SetPluginForTest(fake);

            var emitted = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { emitted.Add(s); return Task.CompletedTask; });

            // Subscribe AFTER wiring the capture. The property seeds Disconnected → that first publish establishes the
            // baseline latch and must NOT emit a status (it is not a drop from Connected).
            host.SubscribeToConnectionState(fake);
            Assert.Empty(emitted);

            // The editor connects: Disconnected → Connected. This recovery-into-Connected edge refreshes the dot green.
            fake.PushConnectionState(HubConnectionState.Connected);
            Assert.Single(emitted);
            Assert.Equal("Connected", emitted[^1].ConnectionState);

            // The user STOPS the local server the editor was connected to: the SignalR client falls Connected →
            // Reconnecting with NO editor config push. Pre-fix: nothing emitted, dot stays green. Post-fix: a fresh
            // non-green status drops the dot.
            fake.PushConnectionState(HubConnectionState.Reconnecting);
            Assert.Equal(2, emitted.Count);
            Assert.NotEqual("Connected", emitted[^1].ConnectionState);
            Assert.Equal("Disconnected", emitted[^1].ConnectionState);
        }

        [Fact]
        public void TransportDrop_ShipsAnEmptyAgentRoster()
        {
            using var host = NewHost(out _);
            var fake = NewFake();
            host.SetPluginForTest(fake);

            var emitted = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { emitted.Add(s); return Task.CompletedTask; });

            host.SubscribeToConnectionState(fake);
            fake.PushConnectionState(HubConnectionState.Connected);
            fake.PushConnectionState(HubConnectionState.Disconnected);

            // The §7 clear-on-non-connected invariant (issue #111) holds for this path too: a dropped link must ship an
            // empty "AI agents" row so the dot/list does not linger from the now-dead connection.
            Assert.Empty(emitted[^1].AiAgents);
        }

        [Fact]
        public void ConnectionStateSubscription_IsIdempotentAcrossReSubscribe()
        {
            using var host = NewHost(out _);
            var fake = NewFake();
            host.SetPluginForTest(fake);

            var emitted = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { emitted.Add(s); return Task.CompletedTask; });

            host.SubscribeToConnectionState(fake);
            host.SubscribeToConnectionState(fake); // a re-Build replaces the prior subscription — must not double-emit

            fake.PushConnectionState(HubConnectionState.Connected);
            fake.PushConnectionState(HubConnectionState.Reconnecting);

            // Exactly one Connected + one drop status — the superseded first subscription is disposed, so no duplicates.
            Assert.Equal(2, emitted.Count);
        }

        // mcp-authorize PR 5 (design 06 / D12): the cloud sign-in indicator surfaces the signed-in state in the editor
        // from EITHER an in-session bearer (an in-editor device flow) OR the machine credential provider being
        // SIGNED IN off the shared store (a zero-button boot — sign-in done out-of-editor via a CLI login /
        // enrollment / another engine or project). Review e1-B1 / design 04 §1: the third input is the provider's
        // plugin-plane IsSignedIn, NEVER bare store existence — an agent-only store (a failed F1 derivation's
        // leftover) exists without being signed in.
        [Theory]
        // Custom mode is never a cloud authorization — neither a local bearer nor a store sign-in lights it.
        [InlineData(false, false, false, null)]
        [InlineData(false, true, false, null)]
        [InlineData(false, false, true, null)]
        [InlineData(false, true, true, null)]
        // Cloud mode with no credential of either kind → not signed in.
        [InlineData(true, false, false, null)]
        // Cloud mode with an in-session bearer (in-editor device flow) → signed in.
        [InlineData(true, true, false, "Authorized")]
        // Cloud mode with ONLY a provider sign-in (zero-button boot, out-of-editor sign-in) → signed in.
        [InlineData(true, false, true, "Authorized")]
        [InlineData(true, true, true, "Authorized")]
        public void ResolveCloudAuthState_SurfacesSignedIn_FromSessionBearerOrProviderSignIn(
            bool isCloudMode, bool hasSessionBearer, bool machineCredentialSignedIn, string? expected)
        {
            Assert.Equal(expected, SidecarHost.ResolveCloudAuthState(isCloudMode, hasSessionBearer, machineCredentialSignedIn));
        }
    }
}
