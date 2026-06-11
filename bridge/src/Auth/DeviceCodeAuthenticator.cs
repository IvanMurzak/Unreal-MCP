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
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Auth
{
    /// <summary>
    /// The real Cloud device-code authorization flow (docs/ARCHITECTURE.md §7 item 4 / §1.3), replacing PR #8's
    /// <c>auth-start</c> / <c>auth-cancel</c> / <c>auth-revoke</c> stubs for the happy path. It is the RFC 8628
    /// device-authorization grant against the ai-game.dev cloud — the SAME contract Unity-MCP's
    /// <c>DeviceAuthService</c> uses: <c>POST {cloudUrl}/api/auth/device/authorize</c> →
    /// <c>POST {cloudUrl}/api/auth/device/token</c> polled until an <c>access_token</c> is issued, the flow is
    /// denied/expired, or it is cancelled.
    ///
    /// As each step progresses it invokes the <c>emit</c> callback with a <see cref="DeviceAuthMessage"/> so the
    /// sidecar can forward it to the plugin (which renders the verification URL + user code, §7). The HTTP layer
    /// and the inter-poll delay are injectable so the xUnit suite drives the full flow against a fake handler
    /// with no network and no real waiting. The issued token is returned to the caller and NEVER logged (§8).
    /// </summary>
    public sealed class DeviceCodeAuthenticator
    {
        private static readonly JsonSerializerOptions JsonOptions = new()
        {
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
            PropertyNameCaseInsensitive = true,
        };

        private readonly HttpClient _http;
        private readonly ILogger? _logger;
        private readonly Func<TimeSpan, CancellationToken, Task> _delay;

        public DeviceCodeAuthenticator(
            HttpClient http,
            ILogger? logger = null,
            Func<TimeSpan, CancellationToken, Task>? delay = null)
        {
            _http = http ?? throw new ArgumentNullException(nameof(http));
            _logger = logger;
            _delay = delay ?? ((d, ct) => Task.Delay(d, ct));
        }

        /// <summary>
        /// Run the full device-code flow against <paramref name="cloudUrl"/>. Emits a <c>pending</c>
        /// device-auth (with the verification URL + user code) as soon as the authorize call returns, polls the
        /// token endpoint honouring the server's <c>interval</c> (and <c>slow_down</c>), and returns the issued
        /// bearer on success. On denial / expiry / cancellation it emits a terminal <c>failed</c> (or the flow's
        /// own cancellation) and returns a non-success result. Never throws for an expected protocol outcome;
        /// a transport exception bubbles as a failed result with the message.
        /// </summary>
        public async Task<DeviceAuthResult> AuthorizeAsync(
            string cloudUrl,
            string? clientLabel,
            Func<DeviceAuthMessage, Task> emit,
            CancellationToken ct)
        {
            if (string.IsNullOrWhiteSpace(cloudUrl))
                return DeviceAuthResult.Failed("No cloud URL configured.");

            var baseUrl = cloudUrl.TrimEnd('/');

            DeviceAuthorizeResponse authorize;
            try
            {
                authorize = await InitiateAsync(baseUrl, clientLabel, ct).ConfigureAwait(false);
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

            while (!ct.IsCancellationRequested)
            {
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
                    await SafeEmit(emit, new DeviceAuthMessage
                    {
                        State = "authorized",
                        Token = token.AccessToken,
                    }).ConfigureAwait(false);
                    _logger?.LogInformation("Device-code flow authorized; cloud token stored."); // token NEVER logged (§8)
                    return DeviceAuthResult.Authorized(token.AccessToken!);
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

        private async Task<DeviceAuthorizeResponse> InitiateAsync(string baseUrl, string? clientLabel, CancellationToken ct)
        {
            var body = clientLabel != null
                ? JsonSerializer.Serialize(new { client_label = clientLabel }, JsonOptions)
                : "{}";
            using var content = new StringContent(body, Encoding.UTF8, "application/json");
            using var response = await _http.PostAsync($"{baseUrl}/api/auth/device/authorize", content, ct).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();
            var json = await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            return JsonSerializer.Deserialize<DeviceAuthorizeResponse>(json, JsonOptions)
                ?? throw new InvalidOperationException("Empty device authorize response.");
        }

        private async Task<DeviceTokenResponse> PollAsync(string baseUrl, string deviceCode, CancellationToken ct)
        {
            var body = JsonSerializer.Serialize(
                new { device_code = deviceCode, grant_type = "urn:ietf:params:oauth:grant-type:device_code" },
                JsonOptions);
            using var content = new StringContent(body, Encoding.UTF8, "application/json");
            using var response = await _http.PostAsync($"{baseUrl}/api/auth/device/token", content, ct).ConfigureAwait(false);
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
            [JsonPropertyName("token_type")] public string? TokenType { get; set; }
            [JsonPropertyName("error")] public string? Error { get; set; }
            [JsonPropertyName("error_description")] public string? ErrorDescription { get; set; }
        }
    }

    /// <summary>Terminal outcome of a device-code flow. The token is present only on <see cref="Success"/>.</summary>
    public sealed class DeviceAuthResult
    {
        public bool Success { get; private init; }
        public bool WasCancelled { get; private init; }
        public string? Token { get; private init; }
        public string? Error { get; private init; }

        public static DeviceAuthResult Authorized(string token) => new() { Success = true, Token = token };
        public static DeviceAuthResult Cancelled() => new() { WasCancelled = true, Error = "cancelled" };
        public static DeviceAuthResult Failed(string error) => new() { Error = error };

        // A device-auth failed message for the UI feed (never carries a secret).
        public static DeviceAuthMessage FailedMessage(string message) => new() { State = "failed", Message = message };
    }
}
