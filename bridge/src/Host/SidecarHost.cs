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
using System.Threading;
using System.Threading.Tasks;
using System.Text.Json.Nodes;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.ReflectorNet;
using com.IvanMurzak.Unreal.MCP.Bridge.Auth;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Microsoft.Extensions.Logging;
using McpVersion = com.IvanMurzak.McpPlugin.Common.Version;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// Owns the reused <c>com.IvanMurzak.McpPlugin</c> SignalR client for the sidecar (docs/ARCHITECTURE.md
    /// §0, §2). Unlike Unity/Godot — which embed the host in-process and scan an assembly for
    /// <c>[AiTool]</c> methods — the sidecar hosts NO static tools: its entire tool set is the dynamic
    /// manifest the C++ plugin pushes over IPC, registered as <see cref="ProxyTool"/>s (§2.2). The plugin
    /// is built once with an empty tool set and the <see cref="ManifestRegistrar"/> is wired BEFORE the
    /// IPC run loop starts, so the first <c>tool-manifest</c> (which can arrive immediately after the
    /// handshake-ack, §1.5) is never dropped. SignalR connect is deferred until the handshake-ack delivers
    /// the effective connection config (§1.5 — "on Ready the plugin immediately pushes tool-manifest + config").
    /// </summary>
    public sealed class SidecarHost : IDisposable
    {
        private readonly IpcClient _ipc;
        private readonly string _sidecarVersion;
        private readonly ILoggerProvider? _loggerProvider;
        private readonly ILogger? _logger;
        private readonly string? _fallbackHost;
        private readonly string? _fallbackToken;

        private readonly ConnectionConfig _config = new()
        {
            KeepConnected = true,
            // The sidecar does not author SKILL.md files — that is the plugin/agent's job. Disabling also
            // avoids the skills-path resolver throwing when no project root is set (MCP-Plugin issue #107).
            GenerateSkillFiles = false,
        };

        private IMcpPlugin? _plugin;
        private ManifestRegistrar? _registrar;
        private Reflector? _reflector;
        private int _signalRConnectStarted;

        // §7 Cloud device-code flow (replaces PR #8's auth stubs). The authenticator is injectable so the xUnit
        // suite drives the flow against a fake HTTP handler; _authCts cancels an in-progress flow (auth-cancel).
        private readonly DeviceCodeAuthenticator _authenticator;
        private readonly string _clientLabel;
        private CancellationTokenSource? _authCts;
        // The HttpClient the default-path authenticator uses, owned here so Dispose() releases it (the
        // injected-authenticator path supplies its own client and leaves this null).
        private readonly HttpClient? _ownedHttpClient;
        // Whether the last applied connection config selected Cloud mode. Gates the §7 cloud-auth indicator:
        // a Custom-mode bearer is a LOCAL token, not a cloud authorization, so it must not light "Authorized".
        private bool _isCloudMode;

        // Serialize SignalR connect/disconnect transitions so rapid Connect/Disconnect toggles (or a device-auth
        // reconnect racing a config-driven disconnect) apply in submission order — last-write-wins — instead of
        // two in-flight plugin.Connect()/Disconnect() calls interleaving and landing in the wrong final state.
        // _transitionCts is the CURRENT transition's cancellation source: a newly-submitted transition cancels
        // it so a wedged/slow connect cannot pin every later transition behind it (the #35 root cause).
        private readonly object _transitionLock = new();
        private Task _connectionTransition = Task.CompletedTask;
        private CancellationTokenSource? _transitionCts;

        public SidecarHost(
            IpcClient ipc,
            string sidecarVersion,
            ILoggerProvider? loggerProvider = null,
            string? fallbackHost = null,
            string? fallbackToken = null,
            DeviceCodeAuthenticator? authenticator = null,
            string clientLabel = "Unreal-MCP-Bridge")
        {
            _ipc = ipc ?? throw new ArgumentNullException(nameof(ipc));
            _sidecarVersion = sidecarVersion;
            _loggerProvider = loggerProvider;
            _logger = loggerProvider?.CreateLogger(nameof(SidecarHost));
            _fallbackHost = fallbackHost;
            _fallbackToken = fallbackToken;
            _clientLabel = clientLabel;
            if (authenticator != null)
            {
                _authenticator = authenticator;
            }
            else
            {
                // We OWN this HttpClient (the injected-authenticator path supplies its own); the authenticator
                // never disposes it, so we track it here and release it in Dispose() for symmetry.
                _ownedHttpClient = new HttpClient();
                _authenticator = new DeviceCodeAuthenticator(_ownedHttpClient, loggerProvider?.CreateLogger(nameof(DeviceCodeAuthenticator)));
            }
        }

        public IMcpPlugin? Plugin => _plugin;
        public ConnectionConfig Config => _config;

        /// <summary>
        /// Test seam: substitute a fake <see cref="IMcpPlugin"/> for the real one <see cref="Build"/> creates,
        /// so the bridge xUnit suite can drive the connection-transition orchestration (re-dial / supersede /
        /// status) against a controllable connect — including a connect that stalls until cancelled, the #35
        /// repro — without standing up a live SignalR endpoint.
        /// </summary>
        internal void SetPluginForTest(IMcpPlugin plugin) => _plugin = plugin;

        /// <summary>
        /// Build the plugin (empty tool set) + reflector, wire the manifest registrar to the live
        /// <c>ToolManager</c>, and subscribe to the IPC handshake/shutdown events. Idempotent: a second
        /// call is a no-op. Does NOT start the IPC run loop (the caller owns that) and does NOT connect
        /// SignalR (that happens on the first handshake-ack).
        /// </summary>
        public void Build()
        {
            if (_plugin != null)
                return;

            if (!string.IsNullOrWhiteSpace(_fallbackHost))
                _config.Host = _fallbackHost!;
            if (!string.IsNullOrWhiteSpace(_fallbackToken))
                _config.Token = _fallbackToken;

            // ProxyTools are schema-blind (raw JSON in/out, §2.1), so a bare reflector suffices — no
            // engine type converters are needed sidecar-side.
            _reflector = new Reflector();

            var version = new McpVersion
            {
                Api = Consts.ApiVersion,
                Plugin = _sidecarVersion,
                Environment = "Unreal-MCP-Bridge",
            };

            // No WithToolsFromAssembly — the sidecar's tools are 100% dynamic (the plugin's manifest).
            var builder = new McpPluginBuilder(version, _loggerProvider).SetConfig(_config);
            _plugin = builder.Build(_reflector);

            var toolManager = _plugin.McpManager.ToolManager
                ?? throw new InvalidOperationException("Built McpPlugin has no ToolManager.");

            _registrar = new ManifestRegistrar(
                new ToolManagerSink(toolManager),
                _ipc,
                _loggerProvider?.CreateLogger(nameof(ManifestRegistrar)));

            // Wire the registrar BEFORE the IPC loop can deliver a manifest.
            _ipc.Registrar = _registrar;
            _ipc.HandshakeAccepted += OnHandshakeAccepted;
            _ipc.ConfigReceived += OnConfigReceived;
            _ipc.AuthMessageReceived += HandleAuthMessage;

            _logger?.LogInformation("Sidecar host built (version {Version}); awaiting IPC handshake.", _sidecarVersion);
        }

        private void OnHandshakeAccepted(HandshakeAckMessage ack)
        {
            // Apply the effective connection config the plugin sent in the handshake-ack (§1.5, §8) — the
            // mode-aware host/token/keepConnected selection lives in ApplyConnectionConfig.
            ApplyConnectionConfig(ack.Config);

            _logger?.LogInformation("Handshake accepted (plugin {PluginVersion}, engine {Engine}); connecting SignalR to {Host}.",
                ack.PluginVersion, ack.EngineVersion, _config.Host);

            // Connect SignalR only on the FIRST accepted handshake. A re-dial's ack (after an IPC drop and
            // reconnect) must NOT kick a second connect — KeepConnected already drives SignalR reconnection
            // inside the client, so connecting again would spin up a duplicate connection.
            if (Interlocked.CompareExchange(ref _signalRConnectStarted, 1, 0) == 0)
                _ = ConnectSignalRAsync();
            else
                _logger?.LogDebug("Handshake re-accepted; SignalR connect already initiated (KeepConnected handles reconnection).");
        }

        internal void OnConfigReceived(JsonObject config)
        {
            // §8 "on change": the plugin re-pushed the effective connection config. Apply it, then honour a
            // keepConnected transition: false → genuinely tear down SignalR (the §7 / Godot M9b Disconnect
            // lesson — not merely drop one link); false→true → (re)connect. A host/token change WHILE still
            // armed must also re-dial (below) — otherwise SignalR stays bound to the stale endpoint.
            var wasKeepConnected = _config.KeepConnected;
            var priorHost = _config.Host;
            var priorToken = _config.Token;
            ApplyConnectionConfig(config);
            _logger?.LogInformation("Applied updated connection config (mode-aware host {Host}).", _config.Host);

            var hostOrTokenChanged =
                !string.Equals(priorHost, _config.Host, StringComparison.Ordinal) ||
                !string.Equals(priorToken, _config.Token, StringComparison.Ordinal);

            switch (DecideConfigTransition(wasKeepConnected, _config.KeepConnected, hostOrTokenChanged))
            {
                case ConfigTransition.Disconnect:
                    _ = RunConnectionTransition(HandleDisconnectAsync);
                    break;
                case ConfigTransition.Reconnect:
                    // Re-dial AND surface the honest result — a bare RunConnectionTransition(ReconnectAsync) would
                    // reconnect silently, leaving the UI stuck on the optimistic "Connecting…" after a successful
                    // re-dial (and a stale "Connected" after a failed one). The §7 medium fix.
                    _ = ReconnectAndEmitStatusAsync();
                    break;
            }
        }

        internal enum ConfigTransition { None, Disconnect, Reconnect }

        /// <summary>
        /// Decide how a §8 config push affects the live SignalR link. A keepConnected edge drives a full
        /// disconnect (true→false, the §7 / Godot M9b Disconnect lesson — not merely drop one link) or a
        /// reconnect (false→true). A host/token change WHILE still armed ALSO forces a reconnect: otherwise the
        /// user switching Cloud↔Custom, editing the Server URL, or changing/generating the token while connected
        /// leaves SignalR bound to the stale endpoint while the UI still reports "Connected". Pure so the bridge
        /// xUnit suite locks the matrix without a live plugin; the serialized-transition queue keeps the issued
        /// reconnect last-write-wins safe.
        /// </summary>
        internal static ConfigTransition DecideConfigTransition(bool wasKeepConnected, bool nowKeepConnected, bool hostOrTokenChanged)
        {
            if (wasKeepConnected && !nowKeepConnected)
                return ConfigTransition.Disconnect;
            if (!wasKeepConnected && nowKeepConnected)
                return ConfigTransition.Reconnect;
            if (nowKeepConnected && hostOrTokenChanged)
                return ConfigTransition.Reconnect;
            return ConfigTransition.None;
        }

        /// <summary>
        /// Apply the plugin-resolved effective connection config (§1.3 / §8) onto <see cref="Config"/> with
        /// mode-aware routing: <c>Cloud</c> → <c>cloudUrl</c>, <c>Custom</c> → <c>host</c>; the token and
        /// <c>keepConnected</c> are taken verbatim (the plugin already resolved them — the sidecar never
        /// re-resolves §8). Absent/blank fields leave the existing value (e.g. the Build()-time env fallback)
        /// intact. Public so the bridge xUnit suite can assert the routing without a live plugin.
        /// </summary>
        public void ApplyConnectionConfig(JsonObject? config)
        {
            if (config == null)
                return;

            // Read each field defensively: a non-string JSON node (e.g. a number/bool from a malformed
            // message) must not throw out of Dispatch and tear down the read loop — treat it as absent.
            static string? ReadString(JsonObject config, string key) =>
                config[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;

            var mode = ReadString(config, "mode");
            var host = ReadString(config, "host");
            var cloudUrl = ReadString(config, "cloudUrl");
            var token = ReadString(config, "token");

            // Mode-aware host selection: Cloud connects to cloudUrl, Custom to host (§8). Fall back to the
            // other field only when the mode's own field is blank, so a partial message still resolves a host.
            var isCloud = string.Equals(mode, "Cloud", StringComparison.OrdinalIgnoreCase);
            // Remember the mode so the §7 cloud-auth indicator does not treat a Custom-mode bearer as a cloud
            // token. Only update when the message actually carried a mode — a partial config push (mode absent)
            // must not silently flip us out of Cloud.
            if (mode != null)
                _isCloudMode = isCloud;
            var selected = isCloud ? cloudUrl : host;
            if (string.IsNullOrWhiteSpace(selected))
                selected = isCloud ? host : cloudUrl;
            if (!string.IsNullOrWhiteSpace(selected))
                _config.Host = selected!;

            // Token: the plugin already applied mode+auth resolution (empty in Custom+None). An absent key
            // leaves the existing token; an explicit empty value clears it (anonymous connection).
            if (token != null)
                _config.Token = string.IsNullOrEmpty(token) ? null : token;

            if (config["keepConnected"] is JsonValue keepNode && keepNode.TryGetValue<bool>(out var keepConnected))
                _config.KeepConnected = keepConnected;
        }

        /// <summary>
        /// Handle a §1.3 auth message (<c>auth-start</c> / <c>auth-cancel</c> / <c>auth-revoke</c>), driving the
        /// REAL Cloud device-code flow (§7 item 4) — the PR #8 stubs are gone for the happy path.
        /// <c>auth-start</c> launches <see cref="DeviceCodeAuthenticator"/> against the resolved cloud URL,
        /// forwarding <c>device-auth</c> progress to the plugin; <c>auth-cancel</c> cancels an in-progress flow;
        /// <c>auth-revoke</c> cancels any flow and clears the stored cloud bearer. Public so the bridge xUnit
        /// suite can drive it directly. Never throws (the flow runs on a background task).
        /// </summary>
        public void HandleAuthMessage(string type)
        {
            switch (type)
            {
                case IpcProtocol.Type.AuthStart:
                    StartDeviceAuth();
                    break;
                case IpcProtocol.Type.AuthCancel:
                    _authCts?.Cancel();
                    _logger?.LogInformation("auth-cancel received; cancelling the in-progress device-code flow (if any).");
                    break;
                case IpcProtocol.Type.AuthRevoke:
                    _authCts?.Cancel();
                    // Clear the stored cloud bearer so a subsequent (re)connect is anonymous until re-authorized.
                    _config.Token = null;
                    _logger?.LogInformation("Cloud token revoked (auth-revoke); cleared the in-memory bearer.");
                    break;
            }
        }

        private void StartDeviceAuth()
        {
            // Cancel AND dispose the previous flow's source before replacing it (cancel-before-dispose is safe:
            // the in-flight flow observes an already-cancelled token, so no post-dispose registration throws).
            _authCts?.Cancel();
            _authCts?.Dispose();
            var cts = new CancellationTokenSource();
            _authCts = cts;
            var cloudUrl = _config.Host; // Cloud mode resolved Host to cloudUrl (ApplyConnectionConfig).
            _logger?.LogInformation("auth-start received; beginning device-code flow against {Host}.", cloudUrl);
            _ = RunDeviceAuthAsync(cloudUrl, cts.Token);
        }

        private async Task RunDeviceAuthAsync(string cloudUrl, CancellationToken ct)
        {
            try
            {
                var result = await _authenticator.AuthorizeAsync(
                    cloudUrl,
                    _clientLabel,
                    emit: msg => _ipc.SendToPluginAsync(msg, CancellationToken.None),
                    ct).ConfigureAwait(false);

                if (result.Success && result.Token != null)
                    await CommitAuthorizedSessionAsync(result.Token, ct).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                _logger?.LogWarning("Device-code flow ended with an error: {Message}", ex.Message);
            }
        }

        /// <summary>
        /// Store the issued cloud bearer and reconnect SignalR so it takes effect. Guards the success-to-store
        /// window: an auth-cancel/auth-revoke can land between the flow's final successful poll and here, and
        /// (for revoke) already cleared the token — storing it now would resurrect a bearer the user just
        /// cancelled/revoked. Bail before storing/reconnecting/emitting when the flow was cancelled. Internal so
        /// the bridge xUnit suite can assert the guard without driving the whole HTTP flow.
        /// </summary>
        internal async Task CommitAuthorizedSessionAsync(string token, CancellationToken ct)
        {
            if (ct.IsCancellationRequested)
                return;
            _config.Token = token; // the issued cloud bearer is NEVER logged (§8)
            // Re-dial (only when still armed) and surface the HONEST status + the cloud-authorized indicator. When
            // the user is DISARMED — they clicked Disconnect (keepConnected=false) before authorizing — store the
            // bearer WITHOUT dialing: claiming Connected here would build a live link Disconnect could never tear
            // down (DecideConfigTransition maps a false→false push to None), violating the §7 Disconnect AC.
            await ReconnectAndEmitStatusAsync(cloudAuthState: "Authorized").ConfigureAwait(false);
        }

        /// <summary>
        /// Re-dial SignalR (only when still armed) and push the HONEST resulting status to the plugin, optionally
        /// carrying a cloud-auth indicator. BOTH §7 re-dial paths route here — a config-driven host/token change
        /// (<see cref="OnConfigReceived"/>) and the device-auth commit (<see cref="CommitAuthorizedSessionAsync"/>)
        /// — so neither leaves the window stuck on the optimistic "Connecting…" after a successful connect nor
        /// claims "Connected" after a failed re-dial. When DISARMED (keepConnected=false) it does NOT dial.
        /// </summary>
        private async Task ReconnectAndEmitStatusAsync(string? cloudAuthState = null)
        {
            if (!_config.KeepConnected)
            {
                await EmitStatusAsync(ResolveConnectionStatusAfterReconnect(keepConnected: false, connected: false), cloudAuthState).ConfigureAwait(false);
                return;
            }

            // Emit INSIDE the transition so a supersede (the user's next action queued a newer re-dial) skips
            // this now-stale status and lets the winning transition emit the authoritative one — otherwise a
            // slow connect here could land "Connecting"/"Connected" AFTER the newer transition reported the truth.
            await RunConnectionTransition(async ct =>
            {
                var connected = await ReconnectAsync(ct).ConfigureAwait(false);
                if (ct.IsCancellationRequested)
                    return; // superseded — the newer transition owns the authoritative status emit
                await EmitStatusAsync(ResolveConnectionStatusAfterReconnect(keepConnected: true, connected), cloudAuthState).ConfigureAwait(false);
            }).ConfigureAwait(false);
        }

        /// <summary>
        /// The connection-state string to surface after a (possible) re-dial: a disarmed commit never claims a
        /// live link ("Disconnected"); an armed re-dial reports the ACTUAL connect outcome instead of an
        /// optimistic guess. Pure so the bridge xUnit suite locks the matrix without a live plugin or IPC link.
        /// </summary>
        internal static string ResolveConnectionStatusAfterReconnect(bool keepConnected, bool connected)
        {
            if (!keepConnected)
                return "Disconnected";
            return connected ? "Connected" : "Connecting";
        }

        /// <summary>Fully disconnect SignalR (auth-revoke / Disconnect), then surface a Disconnected status.</summary>
        private async Task HandleDisconnectAsync(CancellationToken ct)
        {
            var plugin = _plugin;
            if (plugin != null)
            {
                try { await plugin.Disconnect(ct).ConfigureAwait(false); }
                catch (Exception ex) { _logger?.LogDebug("SignalR disconnect failed: {Message}", ex.Message); }
            }
            // A superseded disconnect must not emit its now-stale "Disconnected" after the winning transition's
            // status — the newer transition owns the authoritative emit (last-write-wins).
            if (!ct.IsCancellationRequested)
                await EmitStatusAsync("Disconnected").ConfigureAwait(false);
        }

        /// <summary>
        /// Disconnect then reconnect SignalR so a newly-applied token/host takes effect. Returns whether the
        /// connect attempt reported success, so a caller (the device-code flow) can surface the real state
        /// instead of an unconditional "Connected".
        /// </summary>
        private async Task<bool> ReconnectAsync(CancellationToken ct)
        {
            var plugin = _plugin;
            if (plugin == null)
                return false;
            try { await plugin.Disconnect(ct).ConfigureAwait(false); } catch { /* may not be connected / superseded */ }
            try
            {
                var ok = await plugin.Connect(ct).ConfigureAwait(false);
                _logger?.LogInformation(ok ? "SignalR reconnected." : "SignalR reconnect returned false; client keeps retrying.");
                return ok;
            }
            catch (OperationCanceledException)
            {
                // Superseded by a newer transition (the user's latest action wins); it owns the next dial + status.
                _logger?.LogDebug("SignalR reconnect superseded before it completed.");
                return false;
            }
            catch (Exception ex)
            {
                _logger?.LogWarning("SignalR reconnect failed: {Message}", ex.Message);
                return false;
            }
        }

        /// <summary>
        /// Run a SignalR connect/disconnect transition serialized after any previously-queued one (last-write-wins
        /// in submission order). Prevents two in-flight transitions from interleaving their plugin.Connect()/
        /// Disconnect() calls and leaving the client in a state that contradicts the user's last action.
        /// </summary>
        internal Task RunConnectionTransition(Func<CancellationToken, Task> transition)
        {
            lock (_transitionLock)
            {
                // Supersede the in-flight transition (the documented last-write-wins): cancel its token so a
                // connect that is slow or wedged cannot pin every later transition behind it. McpPlugin's
                // StartConnectionLoop retries every 5 s while a transport keeps failing and holds its gate for
                // the loop's whole duration, so a token-less plugin.Connect() can NEVER return on its own and
                // can NEVER be interrupted — this is the #35 root cause: after the device-auth Cloud reconnect
                // the user's switch-back-to-Custom re-dial was enqueued behind the still-running Cloud connect
                // and never ran, so the bridge held no link and the UI kept a stale "Connected". Cancel-before-
                // dispose is safe (the in-flight transition observes an already-cancelled token, so no post-
                // dispose registration throws), mirroring StartDeviceAuth's CTS handling.
                _transitionCts?.Cancel();
                _transitionCts?.Dispose();
                var cts = new CancellationTokenSource();
                _transitionCts = cts;

                // Still CHAIN off the previous transition so two transitions never interleave their Connect/
                // Disconnect calls (the original serialization reason) — but because the predecessor was just
                // cancelled it now completes promptly, so this freshly-submitted transition runs without waiting
                // on a wedged connect. The continuation passes THIS transition's token to the body.
                var queued = _connectionTransition.ContinueWith(
                    _ => transition(cts.Token), CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default).Unwrap();
                _connectionTransition = queued;
                return queued;
            }
        }

        /// <summary>Push a §1.3 <c>status</c> message to the plugin (the §7 live connection indicator).</summary>
        private Task EmitStatusAsync(string connectionState, string? cloudAuthState = null)
        {
            var status = new StatusMessage
            {
                ConnectionState = connectionState,
                KeepConnected = _config.KeepConnected,
                CloudAuthState = cloudAuthState,
            };
            return _ipc.SendToPluginAsync(status, CancellationToken.None);
        }

        private Task ConnectSignalRAsync()
        {
            // Route the initial connect through the SAME serialized + supersedable queue as every re-dial, so a
            // config push arriving right after the handshake-ack cannot run a second plugin.Connect() concurrently
            // (interleaving the two), and so a slow initial connect is superseded by — not a blocker of — that push.
            return RunConnectionTransition(async ct =>
            {
                var plugin = _plugin;
                if (plugin == null)
                    return;
                try
                {
                    var ok = await plugin.Connect(ct).ConfigureAwait(false);
                    _logger?.LogInformation(ok ? "SignalR connected." : "SignalR initial connect returned false; client will keep retrying.");
                    if (ct.IsCancellationRequested)
                        return; // superseded — a newer transition owns the status emit
                    // §7 live status: surface the connection result to the plugin's view-model. Only a CLOUD-mode
                    // bearer is a cloud authorization — a Custom-mode token is a local bearer and must NOT light the
                    // "Authorized — cloud token stored" indicator (ApplyStatus latches it and never demotes).
                    var cloudAuthState = _isCloudMode && !string.IsNullOrEmpty(_config.Token) ? "Authorized" : null;
                    await EmitStatusAsync(ok ? "Connected" : "Connecting", cloudAuthState).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    _logger?.LogDebug("SignalR initial connect superseded before it completed.");
                }
                catch (Exception ex)
                {
                    _logger?.LogWarning("SignalR connect failed: {Message}", ex.Message);
                }
            });
        }

        public void Dispose()
        {
            _ipc.HandshakeAccepted -= OnHandshakeAccepted;
            _ipc.ConfigReceived -= OnConfigReceived;
            _ipc.AuthMessageReceived -= HandleAuthMessage;
            try { _authCts?.Cancel(); } catch { /* ignore */ }
            _authCts?.Dispose();
            try { _transitionCts?.Cancel(); } catch { /* ignore */ }
            _transitionCts?.Dispose();
            _ownedHttpClient?.Dispose(); // released here — the authenticator does not own it (default path)
            try { _plugin?.Dispose(); } catch { /* ignore */ }
            _plugin = null;
        }
    }
}
