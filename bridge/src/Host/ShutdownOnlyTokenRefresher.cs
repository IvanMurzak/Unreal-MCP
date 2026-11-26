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

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// The refresher the machine-credential provider uses (DEV-272 review U3): the refresh POST is cancelled only when
    /// the host shuts down, never by the caller's token. The on-401 coordinator links its pass to every explicit
    /// disconnect (a config push re-dial, keepConnected off, a sign-in commit) and to auth-state changes; once the
    /// server has rotated the family, cancelling the POST drops the new refresh token, the next refresh presents the
    /// old one, and the server's reuse detection revokes the family for every editor on the machine. The provider
    /// stores the answer whatever its caller's token says (it checks no token after the POST), so letting the POST
    /// finish is enough. The inner refresher's own timeout still bounds it.
    /// </summary>
    internal sealed class ShutdownOnlyTokenRefresher : ITokenRefresher
    {
        private readonly ITokenRefresher _inner;
        private readonly CancellationToken _shutdown;

        public ShutdownOnlyTokenRefresher(ITokenRefresher inner, CancellationToken shutdown)
        {
            _inner = inner ?? throw new ArgumentNullException(nameof(inner));
            _shutdown = shutdown;
        }

        public Task<TokenRefreshResult> RefreshAsync(string refreshToken, string? serverTarget, CancellationToken cancellationToken = default)
            => _inner.RefreshAsync(refreshToken, serverTarget, _shutdown);

        public Task<TokenRefreshResult> RefreshAsync(TokenRefreshRequest request, CancellationToken cancellationToken = default)
            => _inner.RefreshAsync(request, _shutdown);
    }
}
