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
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Auth
{
    /// <summary>
    /// The <see cref="ITokenRefresher"/> the shared <c>PluginCredentialProvider</c> (McpPlugin 7.0) calls to
    /// renew the machine-stored credential — both PROACTIVELY (before <c>exp</c>, driven by the provider's
    /// refresh-skew) and REACTIVELY (on the connection layer's <c>OnAuthorizationRejected</c> signal, wired via
    /// <c>ConnectionCredentialCoordinator</c>). It runs the OAuth 2.1 refresh-token grant against the AS's
    /// <c>POST {serverTarget}/oauth/token</c> (public client — <c>token_endpoint_auth_methods_supported: ["none"]</c>,
    /// so no client secret). The AS ROTATES the refresh token (design 03 / a3), so the response's new
    /// <c>refresh_token</c> is threaded back into the store by the provider; replaying the old one revokes the
    /// family. The HTTP layer is injectable so the xUnit suite drives it with a scripted handler (no network).
    /// Tokens are NEVER logged (§8).
    /// </summary>
    public sealed class OAuthTokenRefresher : ITokenRefresher
    {
        private static readonly JsonSerializerOptions JsonOptions = new()
        {
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
            PropertyNameCaseInsensitive = true,
        };

        private readonly HttpClient _http;
        private readonly string _clientId;
        private readonly ILogger? _logger;
        private readonly Func<DateTimeOffset> _now;

        public OAuthTokenRefresher(
            HttpClient http,
            string clientId = DeviceCodeAuthenticator.DefaultClientId,
            ILogger? logger = null,
            Func<DateTimeOffset>? now = null)
        {
            _http = http ?? throw new ArgumentNullException(nameof(http));
            _clientId = clientId;
            _logger = logger;
            _now = now ?? (() => DateTimeOffset.UtcNow);
        }

        public async Task<TokenRefreshResult> RefreshAsync(string refreshToken, string? serverTarget, CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(refreshToken))
                return TokenRefreshResult.Failure("No refresh token.");
            if (string.IsNullOrWhiteSpace(serverTarget))
                return TokenRefreshResult.Failure("No server target.");

            var baseUrl = serverTarget.TrimEnd('/');

            string json;
            try
            {
                // OAuth 2.1 refresh grant, form-encoded (RFC 6749 §6). A rotated/expired refresh token comes back
                // as a 400 with { error } — read + decode the body regardless of status, like the device poll.
                using var content = new FormUrlEncodedContent(new[]
                {
                    new KeyValuePair<string, string>("grant_type", "refresh_token"),
                    new KeyValuePair<string, string>("refresh_token", refreshToken),
                    new KeyValuePair<string, string>("client_id", _clientId),
                });
                // Dispose the response (as DeviceCodeAuthenticator does) so a long-lived session's proactive/on-401
                // refreshes do not leak the HttpResponseMessage + its pooled connection until finalization.
                using var response = await _http.PostAsync($"{baseUrl}/oauth/token", content, cancellationToken).ConfigureAwait(false);
                json = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return TokenRefreshResult.Failure("cancelled");
            }
            catch (Exception ex)
            {
                _logger?.LogDebug("Token refresh transport error: {Message}", ex.Message);
                return TokenRefreshResult.Failure(ex.Message);
            }

            RefreshTokenResponse? parsed;
            try
            {
                parsed = JsonSerializer.Deserialize<RefreshTokenResponse>(json, JsonOptions);
            }
            catch (Exception ex)
            {
                _logger?.LogDebug("Token refresh response parse error: {Message}", ex.Message);
                return TokenRefreshResult.Failure("Malformed token response.");
            }

            if (parsed == null || string.IsNullOrEmpty(parsed.AccessToken))
            {
                var reason = parsed?.Error ?? "no access token";
                _logger?.LogWarning("Token refresh failed: {Reason}", reason); // never logs the token
                return TokenRefreshResult.Failure(reason);
            }

            var expiresAt = parsed.ExpiresIn > 0 ? _now() + TimeSpan.FromSeconds(parsed.ExpiresIn) : (DateTimeOffset?)null;
            // The AS may or may not rotate the refresh token; when it omits one, keep the existing (pass null so
            // the provider retains it rather than clearing it).
            var newRefresh = string.IsNullOrEmpty(parsed.RefreshToken) ? null : parsed.RefreshToken;
            return TokenRefreshResult.Success(parsed.AccessToken!, newRefresh, expiresAt);
        }

        private sealed class RefreshTokenResponse
        {
            [JsonPropertyName("access_token")] public string? AccessToken { get; set; }
            [JsonPropertyName("refresh_token")] public string? RefreshToken { get; set; }
            [JsonPropertyName("token_type")] public string? TokenType { get; set; }
            [JsonPropertyName("expires_in")] public int ExpiresIn { get; set; }
            [JsonPropertyName("error")] public string? Error { get; set; }
            [JsonPropertyName("error_description")] public string? ErrorDescription { get; set; }
        }
    }
}
