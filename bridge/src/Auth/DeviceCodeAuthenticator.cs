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
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Auth
{
    /// <summary>
    /// The Cloud device-authorization sign-in flow (docs/ARCHITECTURE.md §7 / unified-machine-auth
    /// 03-auth-flows.md F1), on the **RFC 8628-conformant** ai-game.dev alias:
    /// <c>POST {base}/oauth/device_authorization</c> (form: <c>client_id</c> + <c>scope=mcp:agent</c> —
    /// the F1 AGENT scope: the verification page shows the client label + the plain-language scope, D11) →
    /// <c>POST {base}/oauth/token</c> polled with the device-code grant
    /// (<c>grant_type=urn:ietf:params:oauth:grant-type:device_code</c>) until the AS issues an ES256 JWT
    /// (<c>access_token</c>) + a <c>refresh_token</c>, the flow is denied/expired,
    /// or it is cancelled. This replaces the legacy <c>/api/auth/device/*</c> opaque-token path (retired with
    /// the mcp-authorize breaking release); the caller commits the issued AGENT family into the shared
    /// machine credential store via the two-lock-hold login commit (04 §4) and derives the plugin family by
    /// RFC 8693 token exchange, so sign-in happens once per machine (D12) and every other tool adopts it (F1.5).
    ///
    /// As each step progresses it invokes the <c>emit</c> callback with a <see cref="DeviceAuthMessage"/> so the
    /// sidecar can forward it to the plugin (which renders the verification URL + user code, §7). The HTTP layer
    /// and the inter-poll delay are injectable so the xUnit suite drives the full flow against a fake handler
    /// with no network and no real waiting. The issued token is returned to the caller and NEVER logged (§8).
    /// </summary>
    public sealed class DeviceCodeAuthenticator
    {
        /// <summary>The public device-flow client identifier this engine plugin presents to the AS
        /// (RFC 8628; the AS metadata advertises <c>token_endpoint_auth_methods_supported: ["none"]</c>,
        /// so no client secret is used). The exact registered value is confirmed by the live-e2e against
        /// the 9.0 server (mcp-authorize PR 6); the mocked-AS suites here do not depend on it.</summary>
        public const string DefaultClientId = "unreal-mcp-plugin";

        /// <summary>
        /// The F1 device-flow scope (unified-machine-auth 03 F1 / e1): a first login mints the machine-wide
        /// AGENT family; the plugin family is then derived from it by RFC 8693 token exchange (04 §4), never
        /// minted directly. The D11 verification page shows this scope in plain language before approval.
        /// </summary>
        public const string AgentScope = "mcp:agent";

        /// <summary>The tools-only scope (design O10) — kept for the explicit tools-only flows; NOT the default.</summary>
        public const string PluginScope = "mcp:plugin";

        private static readonly JsonSerializerOptions JsonOptions = new()
        {
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
            PropertyNameCaseInsensitive = true,
        };

        private const string DeviceCodeGrantType = "urn:ietf:params:oauth:grant-type:device_code";

        private readonly HttpClient _http;
        private readonly string _clientId;
        private readonly string _scope;
        private readonly ILogger? _logger;
        private readonly Func<TimeSpan, CancellationToken, Task> _delay;
        private readonly Func<DateTimeOffset> _now;

        public DeviceCodeAuthenticator(
            HttpClient http,
            ILogger? logger = null,
            Func<TimeSpan, CancellationToken, Task>? delay = null,
            Func<DateTimeOffset>? now = null,
            string clientId = DefaultClientId,
            string scope = AgentScope)
        {
            _http = http ?? throw new ArgumentNullException(nameof(http));
            _clientId = clientId;
            _scope = scope;
            _logger = logger;
            _delay = delay ?? ((d, ct) => Task.Delay(d, ct));
            // Injectable like _delay so the xUnit suite can drive the expires_in deadline deterministically.
            _now = now ?? (() => DateTimeOffset.UtcNow);
        }

        /// <summary>
        /// The OAuth client id this flow presents to the AS — and therefore the id the minted agent family
        /// must be stamped with (design D8: written from the value actually presented, never inferred).
        /// </summary>
        public string ClientId => _clientId;

        /// <summary>The scope this flow requests (the family stamp falls back to it when the AS omits <c>scope</c>).</summary>
        public string Scope => _scope;

        /// <summary>
        /// Run the full device-code flow against <paramref name="cloudUrl"/>. Emits a <c>pending</c>
        /// device-auth (with the verification URL + user code) as soon as the authorize call returns, polls the
        /// token endpoint honouring the server's <c>interval</c> (and <c>slow_down</c>), and returns the issued
        /// ES256 JWT + refresh token on success. On denial / expiry / cancellation it emits a terminal
        /// <c>failed</c> (or the flow's own cancellation) and returns a non-success result. Never throws for an
        /// expected protocol outcome; a transport exception bubbles as a failed result with the message.
        /// </summary>
        public async Task<DeviceAuthResult> AuthorizeAsync(
            string cloudUrl,
            Func<DeviceAuthMessage, Task> emit,
            CancellationToken ct)
        {
            if (string.IsNullOrWhiteSpace(cloudUrl))
                return DeviceAuthResult.Failed("No cloud URL configured.");

            var baseUrl = cloudUrl.TrimEnd('/');

            DeviceAuthorizeResponse authorize;
            try
            {
                authorize = await InitiateAsync(baseUrl, ct).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested)
            {
                return DeviceAuthResult.Cancelled();
            }
            catch (Exception ex)
            {
                _logger?.LogWarning("Device-code authorize request failed: {Message}", ex.Message);
                await SafeEmit(emit, DeviceAuthResult.FailedMessage("Could not start authorization.")).ConfigureAwait(false);
                return DeviceAuthResult.Failed(ex.Message);
            }

            var verificationUrl = !string.IsNullOrEmpty(authorize.VerificationUriComplete)
                ? authorize.VerificationUriComplete
                : authorize.VerificationUri;

            await SafeEmit(emit, new DeviceAuthMessage
            {
                State = "pending",
                VerificationUrl = verificationUrl,
                UserCode = authorize.UserCode,
            }).ConfigureAwait(false);

            // Poll honouring the server interval (default 5 s per RFC 8628 §3.5), backing off on slow_down.
            var interval = TimeSpan.FromSeconds(Math.Max(1, authorize.Interval > 0 ? authorize.Interval : 5));

            // Client-side deadline from expires_in (RFC 8628 §3.2). Without this the loop relies entirely on the
            // server returning expired_token — and a misbehaving/unreachable token endpoint (whose transport
            // errors are swallowed as transient below) would poll forever. On breach we emit a terminal expired
            // failure and stop. Fall back to 15 min only if the server omits a sane expires_in.
            var expiresInSeconds = authorize.ExpiresIn > 0 ? authorize.ExpiresIn : 900;
            var deadline = _now() + TimeSpan.FromSeconds(expiresInSeconds);

            while (!ct.IsCancellationRequested)
            {
                if (_now() >= deadline)
                {
                    await SafeEmit(emit, DeviceAuthResult.FailedMessage("The authorization request expired.")).ConfigureAwait(false);
                    return DeviceAuthResult.Failed("expired_token");
                }

                try
                {
                    await _delay(interval, ct).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    return DeviceAuthResult.Cancelled();
                }

                DeviceTokenResponse token;
                try
                {
                    token = await PollAsync(baseUrl, authorize.DeviceCode, ct).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (ct.IsCancellationRequested)
                {
                    return DeviceAuthResult.Cancelled();
                }
                catch (Exception ex)
                {
                    _logger?.LogDebug("Device-code poll transient error: {Message}", ex.Message);
                    continue; // transient — keep polling until the server gives a terminal answer or we cancel
                }

                if (!string.IsNullOrEmpty(token.AccessToken))
                {
                    var expiresAt = token.ExpiresIn > 0 ? _now() + TimeSpan.FromSeconds(token.ExpiresIn) : (DateTimeOffset?)null;
                    await SafeEmit(emit, new DeviceAuthMessage
                    {
                        State = "authorized",
                        Token = token.AccessToken,
                    }).ConfigureAwait(false);
                    _logger?.LogInformation("Device-code flow authorized; cloud credential issued."); // token/refresh NEVER logged (§8)
                    return DeviceAuthResult.Authorized(token.AccessToken!, token.RefreshToken, expiresAt, token.Scope);
                }

                switch (token.Error)
                {
                    case "authorization_pending":
                        continue; // user has not finished yet — keep polling at the current interval
                    case "slow_down":
                        interval += TimeSpan.FromSeconds(5); // RFC 8628 §3.5
                        continue;
                    case "access_denied":
                        await SafeEmit(emit, DeviceAuthResult.FailedMessage("Authorization was denied.")).ConfigureAwait(false);
                        return DeviceAuthResult.Failed("access_denied");
                    case "expired_token":
                        await SafeEmit(emit, DeviceAuthResult.FailedMessage("The authorization request expired.")).ConfigureAwait(false);
                        return DeviceAuthResult.Failed("expired_token");
                    default:
                        // An unknown error string is terminal — do not spin forever.
                        var reason = token.Error ?? "unknown error";
                        await SafeEmit(emit, DeviceAuthResult.FailedMessage("Authorization failed.")).ConfigureAwait(false);
                        return DeviceAuthResult.Failed(reason);
                }
            }

            return DeviceAuthResult.Cancelled();
        }

        private async Task<DeviceAuthorizeResponse> InitiateAsync(string baseUrl, CancellationToken ct)
        {
            // RFC 8628 §3.1: application/x-www-form-urlencoded { client_id, scope }.
            using var content = new FormUrlEncodedContent(new[]
            {
                new KeyValuePair<string, string>("client_id", _clientId),
                new KeyValuePair<string, string>("scope", _scope),
            });
            using var response = await _http.PostAsync($"{baseUrl}/oauth/device_authorization", content, ct).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();
            var json = await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            return JsonSerializer.Deserialize<DeviceAuthorizeResponse>(json, JsonOptions)
                ?? throw new InvalidOperationException("Empty device authorize response.");
        }

        private async Task<DeviceTokenResponse> PollAsync(string baseUrl, string deviceCode, CancellationToken ct)
        {
            // RFC 8628 §3.4: application/x-www-form-urlencoded device-code grant. The token endpoint returns a
            // 400 with { error } while pending — do NOT EnsureSuccessStatusCode; read + decode the body either way.
            using var content = new FormUrlEncodedContent(new[]
            {
                new KeyValuePair<string, string>("grant_type", DeviceCodeGrantType),
                new KeyValuePair<string, string>("device_code", deviceCode),
                new KeyValuePair<string, string>("client_id", _clientId),
            });
            using var response = await _http.PostAsync($"{baseUrl}/oauth/token", content, ct).ConfigureAwait(false);
            var json = await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            return JsonSerializer.Deserialize<DeviceTokenResponse>(json, JsonOptions)
                ?? throw new InvalidOperationException("Empty device token response.");
        }

        private static async Task SafeEmit(Func<DeviceAuthMessage, Task> emit, DeviceAuthMessage message)
        {
            try { await emit(message).ConfigureAwait(false); } catch { /* a UI feed hiccup never breaks the flow */ }
        }

        // --- DTOs (ai-game.dev contract; snake_case on the wire) --------------------------------------

        public sealed class DeviceAuthorizeResponse
        {
            [JsonPropertyName("device_code")] public string DeviceCode { get; set; } = "";
            [JsonPropertyName("user_code")] public string UserCode { get; set; } = "";
            [JsonPropertyName("verification_uri")] public string VerificationUri { get; set; } = "";
            [JsonPropertyName("verification_uri_complete")] public string VerificationUriComplete { get; set; } = "";
            [JsonPropertyName("expires_in")] public int ExpiresIn { get; set; }
            [JsonPropertyName("interval")] public int Interval { get; set; }
        }

        public sealed class DeviceTokenResponse
        {
            [JsonPropertyName("access_token")] public string? AccessToken { get; set; }
            [JsonPropertyName("refresh_token")] public string? RefreshToken { get; set; }
            [JsonPropertyName("token_type")] public string? TokenType { get; set; }
            [JsonPropertyName("expires_in")] public int ExpiresIn { get; set; }
            [JsonPropertyName("scope")] public string? Scope { get; set; }
            [JsonPropertyName("error")] public string? Error { get; set; }
            [JsonPropertyName("error_description")] public string? ErrorDescription { get; set; }
        }
    }

    /// <summary>
    /// Terminal outcome of a device-code flow. The credential (access token + refresh token + expiry) is present
    /// only on <see cref="Success"/>; the caller persists it into the machine credential store (D12).
    /// </summary>
    public sealed class DeviceAuthResult
    {
        public bool Success { get; private init; }
        public bool WasCancelled { get; private init; }
        public string? Token { get; private init; }
        public string? RefreshToken { get; private init; }
        public DateTimeOffset? ExpiresAt { get; private init; }
        /// <summary>The granted scope as echoed by the AS's token response (RFC 6749 §5.1 — optional when
        /// identical to the requested scope); null means "as requested". Used to stamp the agent family.</summary>
        public string? GrantedScope { get; private init; }
        public string? Error { get; private init; }

        public static DeviceAuthResult Authorized(string token, string? refreshToken = null, DateTimeOffset? expiresAt = null, string? grantedScope = null) =>
            new() { Success = true, Token = token, RefreshToken = refreshToken, ExpiresAt = expiresAt, GrantedScope = grantedScope };
        public static DeviceAuthResult Cancelled() => new() { WasCancelled = true, Error = "cancelled" };
        public static DeviceAuthResult Failed(string error) => new() { Error = error };

        // A device-auth failed message for the UI feed (never carries a secret).
        public static DeviceAuthMessage FailedMessage(string message) => new() { State = "failed", Message = message };
    }
}
