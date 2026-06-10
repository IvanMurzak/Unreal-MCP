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

            _logger?.LogInformation("Sidecar host built (version {Version}); awaiting IPC handshake.", _sidecarVersion);
        }

        private void OnHandshakeAccepted(HandshakeAckMessage ack)
        {
            // Apply the effective connection config the plugin sent in the handshake-ack (§1.5). The plugin
            // resolves Cloud/Custom host, token and mode (§8) and hands the sidecar the result; the sidecar
            // never re-resolves it. Falls back to the values seeded in Build() when a field is absent.
            var host = ack.Config?["host"]?.GetValue<string>();
            var token = ack.Config?["token"]?.GetValue<string>();
            if (!string.IsNullOrWhiteSpace(host))
                _config.Host = host!;
            if (token != null)
                _config.Token = string.IsNullOrEmpty(token) ? null : token;

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
            try { _plugin?.Dispose(); } catch { /* ignore */ }
            _plugin = null;
        }
    }
}
