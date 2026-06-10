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
using System.Text.Json.Nodes;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.ReflectorNet;
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

        public SidecarHost(
            IpcClient ipc,
            string sidecarVersion,
            ILoggerProvider? loggerProvider = null,
            string? fallbackHost = null,
            string? fallbackToken = null)
        {
            _ipc = ipc ?? throw new ArgumentNullException(nameof(ipc));
            _sidecarVersion = sidecarVersion;
            _loggerProvider = loggerProvider;
            _logger = loggerProvider?.CreateLogger(nameof(SidecarHost));
            _fallbackHost = fallbackHost;
            _fallbackToken = fallbackToken;
        }

        public IMcpPlugin? Plugin => _plugin;
        public ConnectionConfig Config => _config;

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

        private void OnConfigReceived(JsonObject config)
        {
            // §8 "on change": the plugin re-pushed the effective connection config. Apply it. A live mode/host
            // change that requires re-dialling SignalR is deferred to the UI task — for now KeepConnected
            // covers transient reconnects and the next reconnect epoch picks up the applied values.
            ApplyConnectionConfig(config);
            _logger?.LogInformation("Applied updated connection config (mode-aware host {Host}).", _config.Host);
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

            var mode = config["mode"]?.GetValue<string>();
            var host = config["host"]?.GetValue<string>();
            var cloudUrl = config["cloudUrl"]?.GetValue<string>();
            var token = config["token"]?.GetValue<string>();

            // Mode-aware host selection: Cloud connects to cloudUrl, Custom to host (§8). Fall back to the
            // other field only when the mode's own field is blank, so a partial message still resolves a host.
            var isCloud = string.Equals(mode, "Cloud", StringComparison.OrdinalIgnoreCase);
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
        /// Handle a §1.3 auth message (<c>auth-start</c> / <c>auth-cancel</c> / <c>auth-revoke</c>) as a
        /// graceful stub pending the full device-code UI flow (§8). <c>auth-revoke</c> clears the stored
        /// cloud bearer. Public so the bridge xUnit suite can assert the stub behavior directly.
        /// </summary>
        public void HandleAuthMessage(string type)
        {
            // §8 auth plumbing — graceful stubs until the device-code UI flow lands.
            switch (type)
            {
                case IpcProtocol.Type.AuthRevoke:
                    // Clear the stored cloud bearer so a subsequent (re)connect is anonymous until re-authorized.
                    _config.Token = null;
                    _logger?.LogInformation("Cloud token revoked (auth-revoke); cleared the in-memory bearer.");
                    break;
                case IpcProtocol.Type.AuthStart:
                    _logger?.LogInformation("auth-start received; the device-code flow is not implemented in the sidecar-bridge MVP (pending the UI task).");
                    break;
                case IpcProtocol.Type.AuthCancel:
                    _logger?.LogInformation("auth-cancel received; no in-progress device-code flow to cancel (pending the UI task).");
                    break;
            }
        }

        private async System.Threading.Tasks.Task ConnectSignalRAsync()
        {
            var plugin = _plugin;
            if (plugin == null)
                return;
            try
            {
                var ok = await plugin.Connect().ConfigureAwait(false);
                _logger?.LogInformation(ok ? "SignalR connected." : "SignalR initial connect returned false; client will keep retrying.");
            }
            catch (Exception ex)
            {
                _logger?.LogWarning("SignalR connect failed: {Message}", ex.Message);
            }
        }

        public void Dispose()
        {
            _ipc.HandshakeAccepted -= OnHandshakeAccepted;
            _ipc.ConfigReceived -= OnConfigReceived;
            _ipc.AuthMessageReceived -= HandleAuthMessage;
            try { _plugin?.Dispose(); } catch { /* ignore */ }
            _plugin = null;
        }
    }
}
