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
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using Microsoft.AspNetCore.SignalR.Client;
using R3;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// DEV-272 review U2/U3: the connection the on-401 coordinator sees. A rejection passes only when admitted, the
    /// post-refresh reconnect is skipped after a presentation change, the dead-family stop reaches only a connection
    /// that presents the machine credential, and the gate never disposes the real connection.
    /// </summary>
    public class CredentialRejectionGateTests
    {
        private sealed class FakeConnection : IConnection, IConnectionRecovery
        {
            public readonly Subject<Unit> Rejections = new();
            public int Connects, RecoveryConnects, Disconnects, Disposes;
            public ReadOnlyReactiveProperty<bool> KeepConnected { get; } = new ReactiveProperty<bool>(false);
            public ReadOnlyReactiveProperty<HubConnectionState> ConnectionState { get; } = new ReactiveProperty<HubConnectionState>(HubConnectionState.Disconnected);
            public Observable<Unit> OnAuthorizationRejected => Rejections;
            public Observable<Unit> OnDisconnectRequested => Observable.Empty<Unit>();
            public Task<bool> Connect(CancellationToken cancellationToken = default) { Connects++; return Task.FromResult(true); }
            public Task<bool> ConnectForRecovery(CancellationToken cancellationToken) { RecoveryConnects++; return Task.FromResult(true); }
            public Task Disconnect(CancellationToken cancellationToken = default) { Disconnects++; return Task.CompletedTask; }
            public void DisconnectImmediate() { }
            public bool WaitForImmediateTeardown(TimeSpan timeout) => true;
            public void Dispose() => Disposes++;
        }

        [Fact]
        public void Rejection_PassesOnlyWhenAdmitted()
        {
            var inner = new FakeConnection();
            var admit = false;
            using var gate = new CredentialRejectionGate(inner, () => admit, () => true, () => 1);
            var seen = new List<Unit>();
            using var sub = gate.OnAuthorizationRejected.Subscribe(seen.Add);

            inner.Rejections.OnNext(Unit.Default);
            Assert.Empty(seen);
            admit = true;
            inner.Rejections.OnNext(Unit.Default);
            Assert.Single(seen);
        }

        [Fact]
        public async Task RecoveryReconnect_IsSkippedAfterAPresentationChange()
        {
            var inner = new FakeConnection();
            var epoch = 1;
            using var gate = new CredentialRejectionGate(inner, () => true, () => true, () => epoch);
            using var sub = gate.OnAuthorizationRejected.Subscribe(_ => { });

            inner.Rejections.OnNext(Unit.Default);           // admitted in epoch 1
            Assert.True(await gate.ConnectForRecovery(default));
            Assert.Equal(1, inner.RecoveryConnects);

            inner.Rejections.OnNext(Unit.Default);
            epoch = 2;                                       // mode/host changed while the refresh ran
            Assert.True(await gate.ConnectForRecovery(default)); // handled: the config transition re-dials
            Assert.Equal(1, inner.RecoveryConnects);
        }

        [Fact]
        public async Task DeadFamilyStop_ReachesOnlyALinkPresentingTheMachineCredential()
        {
            var inner = new FakeConnection();
            var presents = false;
            using var gate = new CredentialRejectionGate(inner, () => true, () => presents, () => 1);

            await gate.Disconnect();
            Assert.Equal(0, inner.Disconnects);
            presents = true;
            await gate.Disconnect();
            Assert.Equal(1, inner.Disconnects);
        }

        [Fact]
        public void Dispose_LeavesTheConnectionToItsOwner()
        {
            var inner = new FakeConnection();
            new CredentialRejectionGate(inner, () => true, () => true, () => 1).Dispose();
            Assert.Equal(0, inner.Disposes);
        }
    }
}
