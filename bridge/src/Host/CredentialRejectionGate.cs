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
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using Microsoft.AspNetCore.SignalR.Client;
using Microsoft.Extensions.Logging;
using R3;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// The connection the sidecar hands McpPlugin's <see cref="ConnectionCredentialCoordinator"/> (DEV-272 review
    /// U1-U3). The coordinator refreshes (rotates) the machine credential on every authorization-rejection burst,
    /// whatever bearer was rejected, and stops the connection on a dead-family verdict. This decorator lets it act
    /// only when that is right, so the coordinator can live for the whole host lifetime and is never disposed while
    /// a refresh is in flight (disposing it cancels the refresh POST: a rotation the server committed but whose
    /// answer was dropped makes the next refresh present the old token, and reuse detection then revokes the family
    /// for every editor on the machine).
    /// <list type="bullet">
    ///   <item>A rejection reaches the coordinator only when <c>admitRejection</c> says the rejected dial presented
    ///   the machine credential to the server that issued it.</item>
    ///   <item>The reconnect that follows a refresh is skipped (reported as handled) when the presentation context
    ///   (mode, host, auth option) changed since the rejection was admitted: the host's own config transition
    ///   re-dials then. The refresh itself always completes.</item>
    ///   <item>The dead-family stop (<see cref="Disconnect"/>) reaches the connection only while it presents the
    ///   machine credential (<c>admitStop</c>); a Custom token link is never stopped for a machine sign-in.</item>
    /// </list>
    /// Disposing the gate never disposes the wrapped connection (the host owns it).
    /// </summary>
    internal sealed class CredentialRejectionGate : IConnection, IConnectionRecovery
    {
        private readonly IConnection _inner;
        private readonly Func<bool> _admitRejection;
        private readonly Func<bool> _admitStop;
        private readonly Func<int> _presentationEpoch;
        private readonly ILogger? _logger;
        private readonly Subject<Unit> _injectedRejections = new();
        private readonly Observable<Unit> _rejections;
        private int _admittedEpoch = -1;

        public CredentialRejectionGate(IConnection inner, Func<bool> admitRejection, Func<bool> admitStop,
            Func<int> presentationEpoch, ILogger? logger = null)
        {
            _inner = inner ?? throw new ArgumentNullException(nameof(inner));
            _admitRejection = admitRejection ?? throw new ArgumentNullException(nameof(admitRejection));
            _admitStop = admitStop ?? throw new ArgumentNullException(nameof(admitStop));
            _presentationEpoch = presentationEpoch ?? throw new ArgumentNullException(nameof(presentationEpoch));
            _logger = logger;
            _rejections = _inner.OnAuthorizationRejected.Merge(_injectedRejections).Where(_ => Admit());
        }

        public ReadOnlyReactiveProperty<bool> KeepConnected => _inner.KeepConnected;
        public ReadOnlyReactiveProperty<HubConnectionState> ConnectionState => _inner.ConnectionState;
        public Observable<Unit> OnAuthorizationRejected => _rejections;

        public Observable<Unit> OnDisconnectRequested =>
            (_inner as IConnectionRecovery)?.OnDisconnectRequested ?? Observable.Empty<Unit>();

        /// <summary>Test seam: a rejection burst as the connection layer would raise it.</summary>
        internal void InjectRejectionForTest() => _injectedRejections.OnNext(Unit.Default);

        private bool Admit()
        {
            // Capture the epoch first: a context change racing the check then reads as changed, never as unchanged.
            var epoch = _presentationEpoch();
            if (!_admitRejection())
            {
                _logger?.LogWarning("The server rejected this connection, but it did not issue the machine credential (or the credential was not presented); the credential is not refreshed, so the sign-in the other editors on this machine share stays valid.");
                return false;
            }
            Volatile.Write(ref _admittedEpoch, epoch);
            return true;
        }

        public Task<bool> ConnectForRecovery(CancellationToken cancellationToken)
        {
            if (Volatile.Read(ref _admittedEpoch) != _presentationEpoch())
            {
                _logger?.LogInformation("Credential refreshed; the connection target changed meanwhile, so its own transition re-dials.");
                return Task.FromResult(true);
            }
            return (_inner as IConnectionRecovery)?.ConnectForRecovery(cancellationToken) ?? _inner.Connect(cancellationToken);
        }

        public Task<bool> Connect(CancellationToken cancellationToken = default) => _inner.Connect(cancellationToken);

        public Task Disconnect(CancellationToken cancellationToken = default)
        {
            if (_admitStop())
                return _inner.Disconnect(cancellationToken);
            _logger?.LogInformation("Machine sign-in required; this connection does not present the machine credential, so it stays up.");
            return Task.CompletedTask;
        }

        public void DisconnectImmediate() => _inner.DisconnectImmediate();
        public bool WaitForImmediateTeardown(TimeSpan timeout) => _inner.WaitForImmediateTeardown(timeout);

        public void Dispose() => _injectedRejections.Dispose();
    }
}
