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
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using System.Text.Json;
using System.Text.Json.Nodes;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.McpPlugin.ServerLaunch;
using com.IvanMurzak.ReflectorNet;
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Auth;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Microsoft.AspNetCore.SignalR.Client;
using Microsoft.Extensions.Logging;
using R3;
using McpVersion = com.IvanMurzak.McpPlugin.Common.Version;
// The shared machine credential store + DTO (McpPlugin 7.0). Aliased to avoid colliding the two AgentConfig
// namespaces (this one + the bridge-local com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig imported above).
using McpAgentConfig = com.IvanMurzak.McpPlugin.AgentConfig;

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
        /// <summary>
        /// The URL-prefix the cloud serves the MCP server + SignalR hub behind (nginx routes
        /// <c>^/mcp(/|$)</c> → the mcp-server container, rewriting <c>/mcp/hub/... → /hub/...</c>). The
        /// McpPlugin SignalR client appends <c>Consts.Hub.RemoteApp</c> (<c>/hub/mcp-server</c>) to
        /// <see cref="ConnectionConfig.Host"/>, so in Cloud mode the host MUST carry this prefix — otherwise
        /// the client dials <c>https://ai-game.dev/hub/mcp-server</c>, which nginx routes to the frontend SPA
        /// (404) and the sidecar never connects. Mirrors Unity-MCP's <c>CloudServerUrl => base + "/mcp"</c> and
        /// Godot-MCP's <c>CloudHubPath = "/mcp"</c>. Device-code auth + token refresh hit the AS (<c>/oauth/...</c>),
        /// which is NOT behind this prefix, so they must use the stripped base (see <see cref="StripCloudHubPath"/>).
        /// </summary>
        internal const string CloudHubPath = "/mcp";

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

        // McpPlugin 7.0 removed the static ConnectionConfig.Token string; the connection layer now pulls the
        // bearer from ConnectionConfig.CredentialProvider (a Func<Task<string?>>) on each dial. In CUSTOM mode the
        // sidecar resolves a STATIC bearer — the plugin pushes the effective local token over IPC (§8) — held here
        // and returned by ResolveBearerAsync. In CLOUD mode the machine-stored ES256 JWT (below) wins. Volatile-
        // guarded: the provider callback runs on the connection layer's thread while the IPC reader thread /
        // transition tasks mutate it (the same cross-thread access the pre-7.0 auto-property already had).
        private string? _bearerToken;

        // mcp-authorize (design 03 Flow B / 06): the shared machine credential store + the McpPlugin 7.0 credential
        // provider that owns the once-per-machine ES256-JWT/refresh-token lifecycle. NULL unless the caller opted in
        // by supplying a store (Program.cs → ~/.ai-game-dev; the xUnit suite → a temp dir). When null the sidecar
        // behaves exactly as before (static bearer only), so the Custom/env paths are untouched. When present:
        //  - ResolveBearerAsync returns the provider's auto-refreshed JWT in Cloud mode (the device-flow / boot
        //    auto-adopt credential), else the static bearer;
        //  - a successful device-code sign-in is persisted here (CommitAuthorizedSessionAsync) + adopted live;
        //  - the coordinator refreshes on the connection's OnAuthorizationRejected (on-401 → refresh → reconnect);
        //  - OnSignInRequired surfaces "sign in again" to the editor.
        private readonly McpAgentConfig.MachineCredentialStore? _credentialStore;
        private readonly PluginCredentialProvider? _credentialProvider;
        private readonly ITokenRefresher? _tokenRefresher;
        private ConnectionCredentialCoordinator? _credentialCoordinator;
        private IDisposable? _signInRequiredSubscription;

        private IMcpPlugin? _plugin;
        private ManifestRegistrar? _registrar;
        // §A.1 (P1) prompt manifest registrar. Null when the built McpPlugin has no PromptManager (defensive —
        // the empty-then-manifest model means the manager is normally present, like ToolManager).
        private PromptManifestRegistrar? _promptRegistrar;
        // §A.1 (P2) resource manifest registrar. Null when the built McpPlugin has no ResourceManager (defensive
        // — same empty-then-manifest model as the prompt registrar).
        private ResourceManifestRegistrar? _resourceRegistrar;
        private Reflector? _reflector;
        private int _signalRConnectStarted;

        // mcp-authorize PR 3 (design 04/06): the instance-metadata handshake identity. _instanceId is minted ONCE
        // per sidecar process — the sidecar is a child of one editor session, so a single GUID reused across every
        // (re)connect is exactly the "InstanceId minted per editor session, stable across reconnects" the pairing
        // plane (b3) expects. _projectRootPath caches the project root the plugin reported in the handshake-ack
        // (FPaths::ProjectDir()); Volatile-guarded because ApplyProjectIdentity writes it on the IPC reader thread
        // while a later marker-write path could read it off a background task.
        private readonly string _instanceId = Guid.NewGuid().ToString();
        private string? _projectRootPath;

        // §7 AI-agent configurator service, backed by the shared com.IvanMurzak.McpPlugin.AgentConfig library
        // (the single cross-engine implementation). Serves the plugin's thin Slate panel over IPC.
        private readonly AgentConfigService _agentConfig;

        // §7 Cloud device-code flow (replaces PR #8's auth stubs). The authenticator is injectable so the xUnit
        // suite drives the flow against a fake HTTP handler; _authCts cancels an in-progress flow (auth-cancel).
        private readonly DeviceCodeAuthenticator _authenticator;
        private CancellationTokenSource? _authCts;
        // The HttpClient the default-path authenticator uses, owned here so Dispose() releases it (the
        // injected-authenticator path supplies its own client and leaves this null).
        private readonly HttpClient? _ownedHttpClient;
        // Whether the last applied connection config selected Cloud mode. Gates the §7 cloud-auth indicator:
        // a Custom-mode bearer is a LOCAL token, not a cloud authorization, so it must not light "Authorized".
        private bool _isCloudMode;

        // §7 connected-AI-agent roster (issue #109). The plugin's "AI agents" status row reflects
        // StatusMessage.AiAgents; before this it was hardcoded empty. The roster is the formatted label list
        // ("AI agent: {ClientName} ({ClientVersion})") for the currently-connected MCP clients, sourced from
        // McpManager.OnClientsChanged (push) and seeded via McpManagerHub.GetMcpClientData() (pull). Guarded by
        // _rosterLock: OnClientsChanged fires on an R3 callback thread while EmitStatusAsync reads it from the
        // transition queue / connect path, so the read and write must not race. A fresh status is emitted from the
        // OnClientsChanged handler so the editor refreshes live on agent join/leave — today status fired only on
        // the editor's own connection/auth transitions.
        private readonly object _rosterLock = new();
        private List<string> _connectedAgents = new();
        private IDisposable? _clientsChangedSubscription;

        // Bug #116: subscription to the transport-level SignalR connection state (IConnection.ConnectionState, an R3
        // ReadOnlyReactiveProperty<HubConnectionState>). When the link the editor is connected to drops — e.g. the user
        // stops the LOCAL gamedev-mcp-server the editor was connected to — McpPlugin's client transitions Connected →
        // Reconnecting/Disconnected WITHOUT any editor-initiated config push, so none of the existing status-emit paths
        // (config transition, device-auth, roster) fire and the plugin's dot stays stale-green. This subscription makes
        // that drop observable: on a transition AWAY from Connected (while still armed) it emits a fresh non-green
        // `status` so the dot leaves green and the action button updates. Replaced on re-Build; disposed in Dispose().
        private IDisposable? _connectionStateSubscription;
        // The last HubConnectionState we OBSERVED, so we only react to genuine Connected→non-Connected DROPS (not every
        // tick of a property that may re-publish its current value on subscribe). HubConnectionState lives in
        // Microsoft.AspNetCore.SignalR.Client; stored as the enum's int to avoid leaking the type across this field.
        private int _lastObservedConnectionState = -1;

        // The bridge's OWN SignalR client label (Version.Environment). Used to defensively drop a self-entry from
        // the roster: the server's GetMcpClientData/OnClientsChanged report MCP client (AI-agent) sessions, NOT the
        // asking plugin, so in practice no self-entry appears — but excluding by name keeps the row honest if a
        // future server build ever surfaces the plugin's own session. (McpClientData in the pinned McpPlugin 6.10.0
        // exposes no ClientType discriminator, so name-equality is the available self-filter — see issue #109.)
        private const string SelfClientName = "Unreal-MCP-Bridge";

        // §7 retry/backoff for the roster seed (mirrors Unity's 3×/3s): an agent's MCP session can lag the plugin's
        // SignalR connect, so an immediate GetMcpClientData() can return empty even though an agent is about to join.
        // Internal so the bridge xUnit suite can drive the seed without waiting real seconds (override to 0 delay).
        internal int RosterSeedRetryCount = 3;
        internal int RosterSeedRetryDelayMs = 3000;

        // Test seam: the status emitter. Defaults to the IPC send; the bridge xUnit suite swaps it for a capture so
        // it can assert AiAgents population + the live re-emit on a roster change without a live socket. Never null
        // after construction.
        private Func<StatusMessage, Task> _statusEmitter;

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
            McpAgentConfig.MachineCredentialStore? credentialStore = null,
            ITokenRefresher? tokenRefresher = null)
        {
            _ipc = ipc ?? throw new ArgumentNullException(nameof(ipc));
            _sidecarVersion = sidecarVersion;
            _loggerProvider = loggerProvider;
            _logger = loggerProvider?.CreateLogger(nameof(SidecarHost));
            _fallbackHost = fallbackHost;
            _fallbackToken = fallbackToken;
            // McpPlugin 7.0: the connection layer pulls the bearer from ConnectionConfig.CredentialProvider on each
            // dial instead of reading a static ConnectionConfig.Token. ResolveBearerAsync returns the Cloud machine-
            // stored JWT when signed in, else the static bearer the plugin pushes over IPC (or the Build()-time env
            // fallback) — behavior-identical to the pre-7.0 static-token path when no credential store is wired.
            _config.CredentialProvider = ResolveBearerAsync;
            // Default to the real IPC send; SetStatusEmitterForTest swaps it in the xUnit suite.
            _statusEmitter = status => _ipc.SendToPluginAsync(status, CancellationToken.None);
            _agentConfig = new AgentConfigService(loggerProvider?.CreateLogger(nameof(AgentConfigService)));

            // A single owned HttpClient covers the default device authenticator AND the default token refresher; the
            // injected-* paths (xUnit) supply their own, so we only mint one when a default actually needs it.
            if (authenticator == null || (credentialStore != null && tokenRefresher == null))
                _ownedHttpClient = new HttpClient();

            _authenticator = authenticator
                ?? new DeviceCodeAuthenticator(_ownedHttpClient!, loggerProvider?.CreateLogger(nameof(DeviceCodeAuthenticator)));

            // mcp-authorize: wire the machine-credential lifecycle when a store is supplied. B14: PRODUCTION MUST
            // build the sidecar through CreateForProduction, which ALWAYS supplies the store, so a Cloud sign-in can
            // never silently degrade to a static bearer without refresh (design 01 §7d, V11). A null store here is
            // ONLY the static-bearer TEST seam (Custom/env-mode config/transition tests that carry no cloud
            // credential); it mirrors the real pre-sign-in state (a wired-but-empty store also lands on the static
            // path). No prod path reaches this constructor with a null store.
            if (credentialStore != null)
            {
                _credentialStore = credentialStore;
                _tokenRefresher = tokenRefresher
                    ?? new OAuthTokenRefresher(_ownedHttpClient!, logger: loggerProvider?.CreateLogger(nameof(OAuthTokenRefresher)));
                _credentialProvider = new PluginCredentialProvider(
                    _credentialStore, _tokenRefresher, loggerProvider?.CreateLogger(nameof(PluginCredentialProvider)));
            }
        }

        /// <summary>
        /// B14 — the PRODUCTION construction path. Unlike the raw constructor (whose optional
        /// <paramref name="credentialStore"/> is a TEST seam for the static/Custom-bearer path), this factory
        /// GUARANTEES the shared machine credential store — and with it the on-401 refresh + token-refresher —
        /// is wired, so a Cloud sign-in can never silently degrade to a static bearer without refresh (the B14
        /// silent-degradation path). <see cref="Program"/> builds the sidecar through here; the store can no
        /// longer be dropped by editing the entry point. The store defaults to the real <c>~/.ai-game-dev</c>
        /// machine store; the bridge xUnit suite injects an isolated temp store (and optionally a scripted
        /// refresher) to prove the prod wiring includes the store and exercises the refresh path without touching
        /// real machine state.
        /// </summary>
        internal static SidecarHost CreateForProduction(
            IpcClient ipc,
            string sidecarVersion,
            ILoggerProvider? loggerProvider = null,
            string? fallbackHost = null,
            string? fallbackToken = null,
            DeviceCodeAuthenticator? authenticator = null,
            McpAgentConfig.MachineCredentialStore? credentialStore = null,
            ITokenRefresher? tokenRefresher = null)
            => new SidecarHost(
                ipc,
                sidecarVersion,
                loggerProvider,
                fallbackHost,
                fallbackToken,
                authenticator,
                // B14 guarantee: null collapses to the real machine store, so the store is ALWAYS wired in prod.
                credentialStore: credentialStore ?? new McpAgentConfig.MachineCredentialStore(),
                tokenRefresher: tokenRefresher);

        public IMcpPlugin? Plugin => _plugin;
        public ConnectionConfig Config => _config;

        /// <summary>
        /// The static bearer the sidecar currently holds — the value <see cref="ConnectionConfig.CredentialProvider"/>
        /// returns to the connection layer (McpPlugin 7.0 replaced the old <c>ConnectionConfig.Token</c> string with
        /// this async provider callback). Volatile so the provider's cross-thread read sees the latest write.
        /// </summary>
        private string? BearerToken
        {
            get => Volatile.Read(ref _bearerToken);
            set => Volatile.Write(ref _bearerToken, value);
        }

        /// <summary>
        /// Test seam: the bearer the connection layer would resolve from <see cref="ConnectionConfig.CredentialProvider"/>
        /// right now, obtained by invoking the provider (so it proves the provider is wired to the resolved bearer, not
        /// merely that the field is set). In the static-bearer path it completes synchronously; a Cloud signed-in path
        /// with a valid (non-expiring) stored token also returns synchronously. Replaces the pre-7.0
        /// <c>host.Config.Token</c> the bridge xUnit suite asserted.
        /// </summary>
        internal string? CurrentBearer =>
            _config.CredentialProvider is { } provider ? provider().GetAwaiter().GetResult() : null;

        /// <summary>Test seam: the wired machine-credential provider (null unless a store was supplied).</summary>
        internal PluginCredentialProvider? CredentialProvider => _credentialProvider;

        /// <summary>
        /// Test seam: the instance-metadata handshake payload the sidecar will attach to the SignalR hub connection
        /// (mcp-authorize PR 3), or null before a handshake-ack carried a project path. Set by
        /// <see cref="ApplyProjectIdentity"/>; read by the McpPlugin connection layer via
        /// <see cref="ConnectionConfig.InstanceMetadata"/>.
        /// </summary>
        internal ConnectionInstanceMetadata? InstanceMetadata => _config.InstanceMetadata;

        /// <summary>Test seam: the per-editor-session instance id (stable across reconnects) reported in the metadata.</summary>
        internal string InstanceId => _instanceId;

        /// <summary>
        /// Resolve the bearer the SignalR client should present on the next dial (wired to
        /// <see cref="ConnectionConfig.CredentialProvider"/>). In CLOUD mode, once the machine credential provider is
        /// signed in (device-flow sign-in or boot auto-adopt of a seeded store, D12), return its ES256 JWT — the
        /// provider proactively refreshes it before <c>exp</c>. Otherwise return the static bearer the plugin pushed
        /// over IPC (Custom mode) or the Build()-time env fallback — behavior-identical to the pre-mcp-authorize path.
        /// A Custom-mode local token is never overridden by a cloud credential (the <see cref="_isCloudMode"/> guard).
        /// </summary>
        private Task<string?> ResolveBearerAsync()
        {
            var provider = _credentialProvider;
            if (provider != null && _isCloudMode && provider.IsSignedIn)
                return ResolveCloudBearerAsync(provider);
            return Task.FromResult(BearerToken);
        }

        private async Task<string?> ResolveCloudBearerAsync(PluginCredentialProvider provider)
        {
            var jwt = await provider.GetAccessTokenAsync().ConfigureAwait(false);
            return !string.IsNullOrEmpty(jwt) ? jwt : BearerToken;
        }

        /// <summary>
        /// Test seam: substitute a fake <see cref="IMcpPlugin"/> for the real one <see cref="Build"/> creates,
        /// so the bridge xUnit suite can drive the connection-transition orchestration (re-dial / supersede /
        /// status) against a controllable connect — including a connect that stalls until cancelled, the #35
        /// repro — without standing up a live SignalR endpoint.
        /// </summary>
        internal void SetPluginForTest(IMcpPlugin plugin) => _plugin = plugin;

        /// <summary>
        /// Test seam: replace the status emitter (default: IPC send) with a capture so the bridge xUnit suite can
        /// assert <see cref="StatusMessage.AiAgents"/> population and the live re-emit on a roster change without a
        /// live socket. The captured emitter is invoked for EVERY status the host pushes.
        /// </summary>
        internal void SetStatusEmitterForTest(Func<StatusMessage, Task> emitter) =>
            _statusEmitter = emitter ?? throw new ArgumentNullException(nameof(emitter));

        /// <summary>Test seam: the current cached connected-agent roster (the labels surfaced in StatusMessage.AiAgents).</summary>
        internal IReadOnlyList<string> ConnectedAgentsSnapshot
        {
            get { lock (_rosterLock) return _connectedAgents.ToList(); }
        }

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
                BearerToken = _fallbackToken;

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

            // §A.1 (P1) prompts: wire the prompt manifest registrar to the live PromptManager, mirroring the
            // tool wiring. The manager is NULLABLE on IMcpManager; the empty-then-manifest model (no
            // WithPrompts* at build, the plugin's prompt-manifest populates it) means it is normally present.
            // PromptManifestRegistrar implements IManifestSink<PromptManifestMessage>, so this satisfies
            // IpcClient.PromptRegistrar directly. A v1-negotiated link never delivers a prompt-manifest.
            var promptManager = _plugin.McpManager.PromptManager;
            if (promptManager != null)
            {
                _promptRegistrar = new PromptManifestRegistrar(
                    new PromptManagerSink(promptManager),
                    _ipc,
                    _loggerProvider?.CreateLogger(nameof(PromptManifestRegistrar)));
                _ipc.PromptRegistrar = _promptRegistrar;
            }
            else
            {
                _logger?.LogWarning("Built McpPlugin has no PromptManager; prompts disabled this session.");
            }

            // §A.1 (P2) resources: wire the resource manifest registrar to the live ResourceManager, mirroring the
            // prompt wiring. The manager is NULLABLE on IMcpManager; the empty-then-manifest model (no WithResources*
            // at build, the plugin's resource-manifest populates it) means it is normally present.
            // ResourceManifestRegistrar implements IManifestSink<ResourceManifestMessage>, so this satisfies
            // IpcClient.ResourceRegistrar directly. A v1-negotiated link never delivers a resource-manifest.
            var resourceManager = _plugin.McpManager.ResourceManager;
            if (resourceManager != null)
            {
                _resourceRegistrar = new ResourceManifestRegistrar(
                    new ResourceManagerSink(resourceManager),
                    _ipc,
                    _loggerProvider?.CreateLogger(nameof(ResourceManifestRegistrar)));
                _ipc.ResourceRegistrar = _resourceRegistrar;
            }
            else
            {
                _logger?.LogWarning("Built McpPlugin has no ResourceManager; resources disabled this session.");
            }

            // §7 (issue #109): subscribe to the connected-AI-agent roster so the plugin's "AI agents" status row
            // refreshes live. OnClientsChanged fires with the full active-client list on every agent join/leave;
            // cache the formatted labels and push a FRESH status so the editor updates without a reconnect.
            SubscribeToClientRoster(_plugin);

            // Bug #116: subscribe to the transport-level connection state so a link DROP (e.g. the user stopped the
            // local server the editor was connected to) demotes the plugin's dot off green even though no editor
            // config push fired. This is the "subscribe to the McpPlugin connection-lost/reconnecting event and emit a
            // fresh status" half of the bug-2 fix; the ViewModel optimistic demote on Stop is the other half.
            SubscribeToConnectionState(_plugin);

            // mcp-authorize (design 03 Flow B / 06): when the machine-credential path is active, wire the on-401
            // refresh→reconnect and the "sign in again" surfacing. The coordinator subscribes to the connection's
            // OnAuthorizationRejected (3 consecutive SignalR rejections, §Flow E) and refreshes the credential via
            // the provider; the connection layer re-pulls the freshened JWT through ResolveBearerAsync on its next
            // dial. A terminal refresh failure raises OnSignInRequired → we emit a status carrying "SignInRequired".
            // Boot auto-adopt needs no action here: the provider auto-loads a seeded store on construction, so if a
            // credential exists the very first Cloud dial is already signed in (zero-button, D12).
            if (_credentialProvider != null)
            {
                _credentialCoordinator = new ConnectionCredentialCoordinator(
                    _plugin, _credentialProvider, _loggerProvider?.CreateLogger(nameof(ConnectionCredentialCoordinator)));
                _signInRequiredSubscription = _credentialProvider.OnSignInRequired
                    .Subscribe(signal => { _ = EmitStatusAsync(_config.KeepConnected ? "Connecting" : "Disconnected", cloudAuthState: "SignInRequired"); });
                if (_credentialStore!.Exists)
                    _logger?.LogInformation("Machine credential present; the sidecar will connect signed-in with no UI (zero-button boot, D12).");
            }

            _ipc.HandshakeAccepted += OnHandshakeAccepted;
            _ipc.ConfigReceived += OnConfigReceived;
            _ipc.AuthMessageReceived += HandleAuthMessage;
            _ipc.AgentConfigRequestReceived += HandleAgentConfigRequest;
            _ipc.ProjectConfigRequestReceived += HandleProjectConfigRequest;
            _ipc.ServerLaunchArgsRequestReceived += HandleServerLaunchArgsRequest;

            _logger?.LogInformation("Sidecar host built (version {Version}); awaiting IPC handshake.", _sidecarVersion);
        }

        private void OnHandshakeAccepted(HandshakeAckMessage ack)
        {
            // Apply the effective connection config the plugin sent in the handshake-ack (§1.5, §8) — the
            // mode-aware host/token/keepConnected selection lives in ApplyConnectionConfig.
            ApplyConnectionConfig(ack.Config);

            // mcp-authorize PR 3 (design 04/06): resolve THIS project's connection identity from the reported
            // project root and attach the instance-metadata handshake payload BEFORE the connect below dials —
            // ConnectionConfig.InstanceMetadata is read by the McpPlugin HubConnectionProvider on each dial.
            ApplyProjectIdentity(ack.ProjectPath);

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

        /// <summary>
        /// Resolve THIS project's connection identity (mcp-authorize PR 3, design 04/06) from the project root the
        /// plugin reported in the handshake-ack (<c>FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())</c>) and
        /// attach the instance-metadata handshake payload to <see cref="ConnectionConfig.InstanceMetadata"/> — the
        /// McpPlugin connection layer sends it as non-secret query params on the SignalR hub URL, so the server's
        /// account+instance pairing plane (b3) registers this editor session. Also caches the project root (for the
        /// marker-write path) and sets <see cref="ConnectionConfig.ProjectRootPath"/>. Idempotent: a re-handshake
        /// (IPC reconnect) re-resolves the same values with the SAME <see cref="_instanceId"/>. A blank project path
        /// leaves the metadata null (the pre-b7 behaviour — the server falls back to a synthetic single instance).
        /// Never throws (a malformed path must not tear down the read loop). Internal so the bridge xUnit suite
        /// asserts the resolved metadata without driving a live connect (mirrors <see cref="ApplyConnectionConfig"/>).
        /// </summary>
        internal void ApplyProjectIdentity(string? projectPath)
        {
            if (string.IsNullOrWhiteSpace(projectPath))
                return;

            Volatile.Write(ref _projectRootPath, projectPath);
            try
            {
                var resolved = ProjectConnectionResolver.Resolve(projectPath!, _instanceId);
                _config.InstanceMetadata = resolved.Metadata;
                _config.ProjectRootPath = projectPath;
                _logger?.LogInformation(
                    "Resolved project identity for '{ProjectName}' (pin {Pin}, port {Port}{Override}, instance {InstanceId}); attaching instance metadata to the hub handshake.",
                    resolved.ProjectName, resolved.Pin, resolved.Port, resolved.PortIsOverridden ? " [user override]" : string.Empty, _instanceId);
            }
            catch (Exception ex)
            {
                _logger?.LogWarning("Failed to resolve project identity from '{ProjectPath}': {Message}", projectPath, ex.Message);
            }
        }

        internal void OnConfigReceived(JsonObject config)
        {
            // §8 "on change": the plugin re-pushed the effective connection config. Apply it, then honour a
            // keepConnected transition: false → genuinely tear down SignalR (the §7 / Godot M9b Disconnect
            // lesson — not merely drop one link); false→true → (re)connect. A host/token change WHILE still
            // armed must also re-dial (below) — otherwise SignalR stays bound to the stale endpoint.
            var wasKeepConnected = _config.KeepConnected;
            var priorHost = _config.Host;
            var priorToken = BearerToken;
            ApplyConnectionConfig(config);
            _logger?.LogInformation("Applied updated connection config (mode-aware host {Host}).", _config.Host);

            var hostOrTokenChanged =
                !string.Equals(priorHost, _config.Host, StringComparison.Ordinal) ||
                !string.Equals(priorToken, BearerToken, StringComparison.Ordinal);

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
                // Cloud mode: suffix the SignalR host with the /mcp prefix the cloud serves the hub behind
                // (idempotent — never /mcp/mcp). Custom mode: use the host verbatim (a local/self-hosted
                // server exposes the hub at the root, like Unity/Godot Custom). Device-auth strips this back
                // off in StartDeviceAuth (the AS /oauth/... is NOT behind /mcp).
                _config.Host = isCloud ? AppendCloudHubPath(selected!) : selected!;

            // Token: the plugin already applied mode+auth resolution (empty in Custom+None). An absent key
            // leaves the existing token; an explicit empty value clears it (anonymous connection). The resolved
            // bearer flows to SignalR through ConnectionConfig.CredentialProvider (McpPlugin 7.0), which reads
            // this stored value on the next dial.
            if (token != null)
                BearerToken = string.IsNullOrEmpty(token) ? null : token;

            if (config["keepConnected"] is JsonValue keepNode && keepNode.TryGetValue<bool>(out var keepConnected))
                _config.KeepConnected = keepConnected;
        }

        /// <summary>
        /// Append the <see cref="CloudHubPath"/> (<c>/mcp</c>) prefix to a cloud host so the McpPlugin SignalR
        /// client dials <c>…/mcp/hub/mcp-server</c> (the cloud serves the hub behind <c>/mcp</c>). Idempotent —
        /// a host that already ends in <c>/mcp</c> (with or without a trailing slash) is returned with a single
        /// suffix, never <c>/mcp/mcp</c>. Trailing slashes on the base are trimmed first. Pure so the bridge
        /// xUnit suite can lock the round-trip/idempotency/trailing-slash matrix without a live plugin. Mirrors
        /// Unity-MCP's <c>CloudServerUrl</c> and Godot-MCP's <c>ResolveCloudUrl</c>.
        /// </summary>
        internal static string AppendCloudHubPath(string host)
        {
            var trimmed = (host ?? string.Empty).TrimEnd('/');
            if (trimmed.EndsWith(CloudHubPath, StringComparison.OrdinalIgnoreCase))
                return trimmed;
            return trimmed + CloudHubPath;
        }

        /// <summary>
        /// Strip a trailing <see cref="CloudHubPath"/> (<c>/mcp</c>) from a cloud host to recover the BASE URL
        /// the cloud backend + AS live at (<c>https://ai-game.dev</c>) — device-code auth + token refresh target
        /// <c>{base}/oauth/device_authorization</c> + <c>{base}/oauth/token</c>, which are NOT behind the
        /// <c>/mcp</c> nginx prefix. The inverse of <see cref="AppendCloudHubPath"/>; idempotent for a host with
        /// no suffix. Pure (unit-testable).
        /// </summary>
        internal static string StripCloudHubPath(string host)
        {
            var trimmed = (host ?? string.Empty).TrimEnd('/');
            if (trimmed.EndsWith(CloudHubPath, StringComparison.OrdinalIgnoreCase))
                return trimmed[..^CloudHubPath.Length];
            return trimmed;
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
                    BearerToken = null;
                    // mcp-authorize sign-out (design 03 Flow B): wipe the persisted refresh token + clear the provider
                    // so no signed-in state survives on this machine. (The /oauth/revoke network call to the AS and the
                    // editor-UI sign-out button land in later mcp-authorize PRs; the local wipe is the security-critical half.)
                    try { _credentialProvider?.SignOut(); _credentialStore?.Delete(); }
                    catch (Exception ex) { _logger?.LogDebug("Sign-out credential wipe failed: {Message}", ex.Message); }
                    _logger?.LogInformation("Cloud token revoked (auth-revoke); cleared the in-memory bearer + machine credential store.");
                    break;
            }
        }

        /// <summary>
        /// Serve a §7 AI-agent configurator request against the shared <see cref="AgentConfigService"/> and send
        /// the terminal <c>agent-config-result</c> back to the plugin. Deserializes the concrete request by its
        /// <paramref name="type"/>, runs the (synchronous, file-IO) handler off the reader thread, and frames the
        /// result. Never throws — a malformed request or a handler exception becomes an <c>ok == false</c> result
        /// (or, when even the requestId cannot be recovered, a logged drop), so it cannot tear down the read loop.
        /// Public so the bridge xUnit suite can drive the dispatch without a live socket.
        /// </summary>
        public void HandleAgentConfigRequest(string type, JsonObject node)
        {
            // Run off the reader thread: the handlers touch the filesystem (configure/remove write config files).
            _ = Task.Run(async () =>
            {
                AgentConfigResultMessage result;
                try
                {
                    result = ServeAgentConfigRequest(type, node);
                }
                catch (Exception ex)
                {
                    // Best-effort: recover the requestId so the plugin can fail the pending UI action instead of
                    // hanging. A request with no requestId is dropped (logged) — there is nothing to correlate.
                    var requestId = node["requestId"]?.GetValue<string>();
                    if (string.IsNullOrEmpty(requestId))
                    {
                        _logger?.LogWarning("Dropped malformed agent-config '{Type}' request: {Message}", type, ex.Message);
                        return;
                    }
                    result = new AgentConfigResultMessage
                    {
                        RequestId = requestId!,
                        Op = type,
                        Ok = false,
                        Error = $"Sidecar failed to serve '{type}': {ex.Message}",
                    };
                }
                await _ipc.SendToPluginAsync(result, CancellationToken.None).ConfigureAwait(false);
            });
        }

        /// <summary>
        /// Deserialize + dispatch one agent-config request to the matching <see cref="AgentConfigService"/> handler.
        /// Internal so the xUnit suite asserts the routing (and the produced DTO) without the IPC send.
        /// </summary>
        internal AgentConfigResultMessage ServeAgentConfigRequest(string type, JsonObject node)
        {
            switch (type)
            {
                case IpcProtocol.Type.AgentsList:
                    return _agentConfig.HandleList(node.Deserialize<AgentsListRequestMessage>(IpcProtocol.JsonOptions)!);
                case IpcProtocol.Type.AgentStatus:
                    return _agentConfig.HandleStatus(node.Deserialize<AgentStatusRequestMessage>(IpcProtocol.JsonOptions)!);
                case IpcProtocol.Type.AgentConfigure:
                    return _agentConfig.HandleConfigure(node.Deserialize<AgentConfigureRequestMessage>(IpcProtocol.JsonOptions)!);
                case IpcProtocol.Type.AgentRemove:
                    return _agentConfig.HandleRemove(node.Deserialize<AgentRemoveRequestMessage>(IpcProtocol.JsonOptions)!);
                case IpcProtocol.Type.AgentSkillsPath:
                    return _agentConfig.HandleSkillsPath(node.Deserialize<AgentSkillsPathRequestMessage>(IpcProtocol.JsonOptions)!);
                case IpcProtocol.Type.AgentGenerateSkills:
                    // The SKILL.md bodies come from the tool manifest the plugin already pushed (the ProxyTool
                    // catalog the registrar holds) — pass that snapshot to the generator. An empty catalog (no
                    // manifest applied yet) yields a clean "no tools" result rather than a crash.
                    return _agentConfig.HandleGenerateSkills(
                        node.Deserialize<AgentGenerateSkillsRequestMessage>(IpcProtocol.JsonOptions)!,
                        _registrar?.AppliedDescriptors ?? new System.Collections.Generic.List<ToolDescriptor>());
                default:
                    return new AgentConfigResultMessage
                    {
                        RequestId = node["requestId"]?.GetValue<string>() ?? string.Empty,
                        Op = type,
                        Ok = false,
                        Error = $"Unknown agent-config request type '{type}'.",
                    };
            }
        }

        /// <summary>
        /// Serve a mcp-authorize PR 4 <c>project-config</c> request (design 04/06): resolve THIS project's
        /// {pin, derived local-server port, portIsOverridden, serverTarget} via <see cref="ProjectConnectionResolver"/>
        /// and send the terminal <c>project-config-result</c> back to the plugin. Runs the (file-IO) resolve off the
        /// reader thread. Never throws — a malformed request or a resolve failure becomes an <c>ok == false</c> result
        /// (or, when even the requestId cannot be recovered, a logged drop), so it cannot tear down the read loop.
        /// Public so the bridge xUnit suite can drive the dispatch without a live socket.
        /// </summary>
        public void HandleProjectConfigRequest(string type, JsonObject node)
        {
            // Run off the reader thread: ProjectConnectionResolver.Resolve reads the on-disk project marker.
            _ = Task.Run(async () =>
            {
                ProjectConfigResultMessage result;
                try
                {
                    result = BuildProjectConfigResult(node);
                }
                catch (Exception ex)
                {
                    // Recover the requestId defensively: this catch runs in a fire-and-forget Task.Run, so it must
                    // itself never throw (GetValue<string>() would throw on a non-string requestId token — exactly the
                    // malformed-request case that lands us here — turning a handled drop into an unobserved exception).
                    string? requestId = null;
                    if (node["requestId"] is JsonValue requestIdValue)
                        requestIdValue.TryGetValue(out requestId);
                    if (string.IsNullOrEmpty(requestId))
                    {
                        _logger?.LogWarning("Dropped malformed project-config '{Type}' request: {Message}", type, ex.Message);
                        return;
                    }
                    result = new ProjectConfigResultMessage
                    {
                        RequestId = requestId!,
                        Ok = false,
                        Error = $"Sidecar failed to resolve project config: {ex.Message}",
                    };
                }
                await _ipc.SendToPluginAsync(result, CancellationToken.None).ConfigureAwait(false);
            });
        }

        /// <summary>
        /// Resolve a <c>project-config</c> request into its terminal result. Prefers the project root the request
        /// carries (race-free — the C++ plugin knows <c>FPaths::ProjectDir()</c>); falls back to the handshake-reported
        /// root cached by <see cref="ApplyProjectIdentity"/>. Resolution goes through the SAME
        /// <see cref="ProjectConnectionResolver.Resolve"/> PR 3 uses, so the returned pin/port carry byte-for-byte
        /// <c>ProjectIdentity</c> golden-vector parity and the marker's <c>portOverride</c> precedence. Internal so the
        /// bridge xUnit suite asserts the resolved DTO (parity + override) without the IPC send.
        /// </summary>
        internal ProjectConfigResultMessage BuildProjectConfigResult(JsonObject node)
        {
            var request = node.Deserialize<ProjectConfigRequestMessage>(IpcProtocol.JsonOptions) ?? new ProjectConfigRequestMessage();

            var projectRoot = !string.IsNullOrWhiteSpace(request.ProjectPath)
                ? request.ProjectPath
                : Volatile.Read(ref _projectRootPath);

            if (string.IsNullOrWhiteSpace(projectRoot))
                return new ProjectConfigResultMessage
                {
                    RequestId = request.RequestId,
                    Ok = false,
                    Error = "No project path available to resolve the connection identity (handshake not applied and request carried none).",
                };

            var resolved = ProjectConnectionResolver.Resolve(projectRoot!, _instanceId);
            return new ProjectConfigResultMessage
            {
                RequestId = request.RequestId,
                Ok = true,
                Pin = resolved.Pin,
                Port = resolved.Port,
                PortIsOverridden = resolved.PortIsOverridden,
                ServerTarget = resolved.ServerTarget,
            };
        }

        /// <summary>
        /// Serve a mcp-authorize g5/g6 <c>server-launch-args</c> request: compose the LOCAL gamedev-mcp-server
        /// launch-arg string via the SHARED <see cref="ServerLaunchArguments"/> builder (none/oauth/token) so the C++
        /// <c>FUnrealMcpServerManager</c> never duplicates the arg logic, and send the terminal
        /// <c>server-launch-args-result</c> back to the plugin. Never throws — a malformed request or a builder
        /// <c>ArgumentException</c> (e.g. token mode with no secret) becomes an <c>ok == false</c> result (or, when even
        /// the requestId cannot be recovered, a logged drop) so it cannot tear down the read loop. Public so the bridge
        /// xUnit suite can drive the dispatch without a live socket.
        /// </summary>
        public void HandleServerLaunchArgsRequest(string type, JsonObject node)
        {
            _ = Task.Run(async () =>
            {
                ServerLaunchArgsResultMessage result;
                try
                {
                    result = BuildServerLaunchArgsResult(node);
                }
                catch (Exception ex)
                {
                    // Recover the requestId defensively — this runs in a fire-and-forget Task.Run and must never throw.
                    string? requestId = null;
                    if (node["requestId"] is JsonValue requestIdValue)
                        requestIdValue.TryGetValue(out requestId);
                    if (string.IsNullOrEmpty(requestId))
                    {
                        _logger?.LogWarning("Dropped malformed server-launch-args '{Type}' request: {Message}", type, ex.Message);
                        return;
                    }
                    result = new ServerLaunchArgsResultMessage
                    {
                        RequestId = requestId!,
                        Ok = false,
                        Error = $"Sidecar failed to compose launch args: {ex.Message}",
                    };
                }
                await _ipc.SendToPluginAsync(result, CancellationToken.None).ConfigureAwait(false);
            });
        }

        /// <summary>
        /// Compose a <c>server-launch-args</c> request into its terminal result by calling the SHARED
        /// <see cref="ServerLaunchArguments.BuildCommandLine"/> — the SAME builder Unity/Godot call in-process — with the
        /// plugin-resolved connection facts. The auth mode maps to <see cref="Consts.MCP.Server.AuthOption"/>
        /// (none/oauth/token; anything unrecognized fails closed as an error, never a silent downgrade). The builder is
        /// fail-closed: token mode needs a non-empty token; oauth mode needs both issuer and public-url — a missing
        /// credential throws <see cref="ArgumentException"/>, surfaced as <c>ok == false</c>. Internal so the bridge
        /// xUnit suite asserts the composed args without the IPC send. Never logs the token.
        /// </summary>
        internal ServerLaunchArgsResultMessage BuildServerLaunchArgsResult(JsonObject node)
        {
            var request = node.Deserialize<ServerLaunchArgsRequestMessage>(IpcProtocol.JsonOptions) ?? new ServerLaunchArgsRequestMessage();

            // Accept only the enum NAMES (none|oauth|token). Enum.TryParse ALSO accepts numeric strings — e.g. "0"
            // parses to the underlying enum value (none), which would let an unexpected numeric authMode silently spawn
            // an anonymous server: the exact silent auth=none downgrade the g5/g6 design forbids. Reject any numeric
            // form and fail closed (the plugin's FUnrealMcpConfig::AuthOptionToString only ever emits the names).
            var rawAuthMode = request.AuthMode ?? string.Empty;
            if (long.TryParse(rawAuthMode, out _)
                || !Enum.TryParse<Consts.MCP.Server.AuthOption>(rawAuthMode, ignoreCase: true, out var authOption)
                || (authOption != Consts.MCP.Server.AuthOption.none
                    && authOption != Consts.MCP.Server.AuthOption.oauth
                    && authOption != Consts.MCP.Server.AuthOption.token))
            {
                return new ServerLaunchArgsResultMessage
                {
                    RequestId = request.RequestId,
                    Ok = false,
                    Error = $"Unsupported auth mode '{request.AuthMode}' (expected none|oauth|token).",
                };
            }

            try
            {
                var args = ServerLaunchArguments.BuildCommandLine(
                    request.Port,
                    request.PluginTimeoutMs,
                    Consts.MCP.Server.TransportMethod.streamableHttp,
                    authOption,
                    token: request.Token,
                    authIssuer: request.AuthIssuer,
                    publicUrl: request.PublicUrl);
                return new ServerLaunchArgsResultMessage
                {
                    RequestId = request.RequestId,
                    Ok = true,
                    Args = args,
                };
            }
            catch (ArgumentException ex)
            {
                // Fail-closed: a mode's missing credential (token/issuer/public-url). Never echo the token.
                return new ServerLaunchArgsResultMessage
                {
                    RequestId = request.RequestId,
                    Ok = false,
                    Error = ex.Message,
                };
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
            // ApplyConnectionConfig suffixed the Cloud host with /mcp for the SignalR hub; the device-code flow
            // hits the AS ({base}/oauth/device_authorization + /oauth/token), which is NOT behind the /mcp nginx
            // prefix, so strip it back off to recover the base (https://ai-game.dev). A no-op for a host without the suffix.
            var cloudUrl = StripCloudHubPath(_config.Host);
            _logger?.LogInformation("auth-start received; beginning device-code flow against {Host}.", cloudUrl);
            _ = RunDeviceAuthAsync(cloudUrl, cts.Token);
        }

        private async Task RunDeviceAuthAsync(string cloudUrl, CancellationToken ct)
        {
            try
            {
                var result = await _authenticator.AuthorizeAsync(
                    cloudUrl,
                    emit: msg => _ipc.SendToPluginAsync(msg, CancellationToken.None),
                    ct).ConfigureAwait(false);

                if (result.Success && result.Token != null)
                    await CommitAuthorizedSessionAsync(result, ct).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                _logger?.LogWarning("Device-code flow ended with an error: {Message}", ex.Message);
            }
        }

        /// <summary>
        /// Commit a successful device-code sign-in: PERSIST the issued credential (ES256 JWT + refresh token +
        /// expiry) into the shared machine credential store (D12) so sign-in is once-per-machine and survives
        /// restarts, ADOPT it into the live provider, then store the in-memory bearer + reconnect. The persist step
        /// is a no-op when no store is wired (Custom/legacy path) or the AS returned no refresh token. Guards the
        /// same cancel/revoke race as the string overload — nothing is persisted once the flow was cancelled.
        /// Tokens are NEVER logged (§8). Internal so the bridge xUnit suite drives it without the whole HTTP flow.
        /// </summary>
        internal async Task CommitAuthorizedSessionAsync(DeviceAuthResult result, CancellationToken ct)
        {
            if (ct.IsCancellationRequested || result.Token == null)
                return;

            if (_credentialStore != null && !string.IsNullOrEmpty(result.RefreshToken))
            {
                // The server target is the cloud BASE (no /mcp hub prefix), the same base device-auth ran against —
                // the refresher reuses it for the refresh-token grant. Persist + adopt so the provider is signed in.
                var serverTarget = StripCloudHubPath(_config.Host);
                var creds = new McpAgentConfig.MachineCredentials
                {
                    Version = 1,
                    AccessToken = result.Token,
                    RefreshToken = result.RefreshToken,
                    ExpiresAt = result.ExpiresAt,
                    ServerTarget = string.IsNullOrWhiteSpace(serverTarget) ? null : serverTarget,
                };
                try
                {
                    _credentialStore.Write(creds);
                    _credentialProvider?.Adopt(creds);
                    _logger?.LogInformation("Cloud credential persisted to the machine store; signed in once-per-machine (D12).");
                }
                catch (Exception ex)
                {
                    // A store write failure must not lose the session — fall through to the in-memory bearer so this
                    // editor session still connects signed-in; only the cross-restart persistence is degraded.
                    _logger?.LogWarning("Failed to persist the cloud credential to the machine store: {Message}", ex.Message);
                }
            }

            await CommitAuthorizedSessionAsync(result.Token, ct).ConfigureAwait(false);
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
            BearerToken = token; // the issued cloud bearer is NEVER logged (§8)
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
                // §7 (issue #111): the reconnect is disarmed (the user clicked Disconnect / keepConnected=false), so
                // there will be no live link — clear the cached roster so a later re-arm re-seeds fresh.
                ClearRoster();
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
                // §7 (issue #109): a re-dial (config host/token change or device-auth commit) re-establishes the
                // SignalR link — re-seed the roster so the agents row reflects the post-reconnect state.
                if (connected)
                    _ = SeedRosterAsync();
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

        /// <summary>
        /// Resolve the cloud sign-in indicator for a status emit (mcp-authorize PR 5, design 06 / D12). Cloud mode
        /// reports <c>"Authorized"</c> when EITHER an in-session bearer was issued (an in-editor device flow) OR the
        /// shared machine credential store already holds a credential (zero-button sign-in — a CLI login, an
        /// enrollment, or another engine/project signed in once-per-machine). This is what surfaces the signed-in
        /// state in the editor even when sign-in happened out-of-editor. <c>null</c> in Custom mode, or when no
        /// credential of either kind is present. Pure + static so the bridge xUnit suite locks the matrix without a
        /// live SignalR link or machine store.
        /// </summary>
        internal static string? ResolveCloudAuthState(bool isCloudMode, bool hasSessionBearer, bool machineCredentialExists) =>
            isCloudMode && (hasSessionBearer || machineCredentialExists) ? "Authorized" : null;

        /// <summary>Fully disconnect SignalR (auth-revoke / Disconnect), then surface a Disconnected status.</summary>
        private async Task HandleDisconnectAsync(CancellationToken ct)
        {
            var plugin = _plugin;
            if (plugin != null)
            {
                try { await plugin.Disconnect(ct).ConfigureAwait(false); }
                catch (Exception ex) { _logger?.LogDebug("SignalR disconnect failed: {Message}", ex.Message); }
            }
            // §7 (issue #111): a user Disconnect tears the link down without firing OnClientsChanged, so clear the
            // cache here too — otherwise a later reconnect would re-seed the agents row from the frozen pre-disconnect
            // list before SeedRosterAsync pulls the live state.
            ClearRoster();
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
                // The real ConnectionManager returns false (not OCE) when its token is cancelled mid-dial —
                // distinguish a superseded dial from a genuine retry-in-progress so the log stays truthful.
                if (ok)
                    _logger?.LogInformation("SignalR reconnected.");
                else if (ct.IsCancellationRequested)
                    _logger?.LogDebug("SignalR reconnect superseded before it completed.");
                else
                    _logger?.LogInformation("SignalR reconnect returned false; client keeps retrying.");
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
            CancellationTokenSource? superseded;
            Task queued;
            lock (_transitionLock)
            {
                // Supersede the in-flight transition (the documented last-write-wins): cancel its token so a
                // connect that is slow or wedged cannot pin every later transition behind it. McpPlugin's
                // StartConnectionLoop retries every 5 s while a transport keeps failing and holds its gate for
                // the loop's whole duration, so a token-less plugin.Connect() can NEVER return on its own and
                // can NEVER be interrupted — this is the #35 root cause: after the device-auth Cloud reconnect
                // the user's switch-back-to-Custom re-dial was enqueued behind the still-running Cloud connect
                // and never ran, so the bridge held no link and the UI kept a stale "Connected". Capture the
                // predecessor here but cancel/dispose it OUTSIDE the lock (below): CancellationTokenSource.Cancel()
                // runs the loser's await-continuations SYNCHRONOUSLY, and running arbitrary continuation code under
                // _transitionLock is fragile against future edits.
                superseded = _transitionCts;
                var cts = new CancellationTokenSource();
                _transitionCts = cts;
                // Capture the token STRUCT, not the source: the queued continuation reads it when it RUNS, which
                // may be after a LATER submission Cancel()+Dispose()'d this source. CancellationTokenSource.Token's
                // getter still throws ObjectDisposedException post-dispose (would fault the loser transition task),
                // but a captured CancellationToken copy stays usable — and cancel-before-dispose guarantees it
                // reads cancelled, so the body observes the supersede correctly.
                var token = cts.Token;

                // Still CHAIN off the previous transition so two transitions never interleave their Connect/
                // Disconnect calls (the original serialization reason) — but because the predecessor was just
                // cancelled it now completes promptly, so this freshly-submitted transition runs without waiting
                // on a wedged connect. The continuation passes THIS transition's token to the body.
                queued = _connectionTransition.ContinueWith(
                    _ => transition(token), CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default).Unwrap();
                _connectionTransition = queued;
            }

            // Cancel-before-dispose is safe (the in-flight transition observes an already-cancelled token, so no
            // post-dispose registration throws), mirroring StartDeviceAuth's CTS handling. Done outside the lock so
            // the loser's synchronous continuations do not run under _transitionLock. Guard against a concurrent
            // Dispose() having already torn this source down.
            try { superseded?.Cancel(); } catch (ObjectDisposedException) { /* Dispose() raced us */ }
            superseded?.Dispose();
            return queued;
        }

        /// <summary>
        /// Subscribe to <see cref="IMcpManager.OnClientsChanged"/> so the §7 "AI agents" status row tracks the live
        /// roster (issue #109). On each change: cache the formatted labels and emit a FRESH <c>status</c> so the
        /// editor refreshes on agent join/leave (today status fired only on the editor's own connection/auth
        /// transitions). Internal + plugin-injected so the bridge xUnit suite can drive it with a fake manager whose
        /// observable it controls. Idempotent: a second subscribe (e.g. a re-Build) replaces the prior subscription.
        /// </summary>
        internal void SubscribeToClientRoster(IMcpPlugin plugin)
        {
            // Replace any prior subscription so a re-wire does not leak / double-emit.
            _clientsChangedSubscription?.Dispose();
            _clientsChangedSubscription = plugin.McpManager.OnClientsChanged
                .Subscribe(clients =>
                {
                    UpdateRoster(clients);
                    // Emit a fresh status carrying the new roster. The connection state is unchanged by a roster
                    // change, so report the resolved current state (armed → Connected once a link exists; the plugin
                    // latches the dot from aiAgents anyway). Fire-and-forget — never throw out of the R3 callback.
                    _ = EmitStatusAsync(_config.KeepConnected ? "Connected" : "Disconnected");
                });
        }

        /// <summary>
        /// Bug #116: subscribe to the transport-level SignalR connection state (<see cref="IConnection.ConnectionState"/>,
        /// an R3 <c>ReadOnlyReactiveProperty&lt;HubConnectionState&gt;</c>). On a transition AWAY from
        /// <see cref="HubConnectionState.Connected"/> (the link dropped — most importantly because the user stopped the
        /// local server the editor was connected to), emit a FRESH non-green <c>status</c> so the plugin's dot leaves
        /// green and the action button updates, WITHOUT requiring an editor-initiated config push. The drop emits
        /// "Disconnected", which the plugin's ParseConnectionState folds into amber Degraded while armed (the client is
        /// auto-retrying) and a true Disconnected when disarmed. A transition INTO Connected re-emits a
        /// Connected status so a transport-level recovery (the client reconnected on its own) also refreshes the dot.
        /// Internal + plugin-injected so the bridge xUnit suite drives it with a fake whose ConnectionState it controls.
        /// Idempotent: a second subscribe (a re-Build) replaces the prior subscription.
        /// </summary>
        internal void SubscribeToConnectionState(IMcpPlugin plugin)
        {
            _connectionStateSubscription?.Dispose();
            // Reset the observed-state latch so the first value the property publishes establishes the baseline rather
            // than being mis-read as a drop from a stale prior subscription's last value.
            _lastObservedConnectionState = -1;
            _connectionStateSubscription = plugin.ConnectionState
                .Subscribe(state =>
                {
                    var previous = _lastObservedConnectionState;
                    _lastObservedConnectionState = (int)state;
                    // Fire-and-forget; never throw out of the R3 callback (it runs on a client thread).
                    if (ShouldEmitOnConnectionStateChange(previous, (int)state, out var connectionState))
                        _ = EmitConnectionStateStatusAsync(connectionState);
                });
        }

        /// <summary>
        /// Decide whether a transport-level <see cref="HubConnectionState"/> transition warrants a fresh <c>status</c>
        /// emit, and what connection-state string to ship. We emit on a genuine EDGE only — a DROP off Connected (the
        /// bug-2 trigger) or a RECOVERY back into Connected — never on the baseline first observation (<paramref
        /// name="previous"/> &lt; 0) or a same-state re-publish, so a property that re-emits its current value on
        /// subscribe does not spam a redundant status. Pure + static so the bridge xUnit suite locks the matrix without a
        /// live client. The shipped string is mode-agnostic ("Disconnected"/"Connected"); <see cref="EmitStatusAsync"/>
        /// stamps the live <c>keepConnected</c>, which the plugin's ParseConnectionState folds into Degraded while armed.
        /// </summary>
        internal static bool ShouldEmitOnConnectionStateChange(int previous, int current, out string connectionState)
        {
            connectionState = string.Empty;
            // No prior observation (baseline) or no actual change — nothing to report.
            if (previous < 0 || previous == current)
                return false;

            var nowConnected = current == (int)HubConnectionState.Connected;
            var wasConnected = previous == (int)HubConnectionState.Connected;

            if (wasConnected && !nowConnected)
            {
                // The link the editor was on just dropped (stopped local server / network loss). Report "Disconnected"
                // — while armed the plugin's ParseConnectionState folds Disconnected into its amber Degraded dot (the
                // client auto-retries in the background), matching the ViewModel's optimistic post-Stop Degraded so the
                // dot drops straight to amber with no amber→blue flip; a disarmed drop reads as a true Disconnected.
                connectionState = "Disconnected";
                return true;
            }
            if (!wasConnected && nowConnected)
            {
                // A transport-level recovery (the client reconnected on its own) — refresh the dot back to green.
                connectionState = "Connected";
                return true;
            }
            // Connecting↔Reconnecting and other non-Connected churn: no edge that changes the green/non-green dot.
            return false;
        }

        /// <summary>
        /// Emit a <c>status</c> for a transport-level connection-state edge (Bug #116). A non-Connected drop must ALSO
        /// clear the cached roster so a later reconnect re-seeds fresh and the "AI agents" row does not linger from the
        /// dropped link (mirrors the disconnect paths' <see cref="ClearRoster"/>). <see cref="EmitStatusAsync"/> already
        /// ships an empty roster under any non-Connected state, but clearing the cache keeps it honest for the next seed.
        /// </summary>
        private Task EmitConnectionStateStatusAsync(string connectionState)
        {
            if (!string.Equals(connectionState, "Connected", StringComparison.Ordinal))
                ClearRoster();
            return EmitStatusAsync(connectionState);
        }

        /// <summary>
        /// Seed the roster on connect via <see cref="IMcpManagerHub.GetMcpClientData"/> (pull), with a small
        /// retry/backoff (Unity uses 3×/3s) because an agent's MCP session can lag the plugin's SignalR connect —
        /// an immediate pull can return empty even though an agent is about to join. Each successful pull updates
        /// the cache and emits a fresh status; if the pull yields no connected agent and retries remain (and we are
        /// still armed), it waits <see cref="RosterSeedRetryDelayMs"/> and tries again. Never throws (runs on a
        /// background task). Internal so the bridge xUnit suite can drive it directly.
        /// </summary>
        internal async Task SeedRosterAsync(CancellationToken ct = default)
        {
            var hub = _plugin?.McpManagerHub;
            if (hub == null)
                return;

            for (var attempt = 0; ; attempt++)
            {
                if (ct.IsCancellationRequested)
                    return;

                McpClientData[]? clients = null;
                try
                {
                    var task = hub.GetMcpClientData();
                    if (task != null)
                        clients = await task.ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    _logger?.LogDebug("Roster seed pull failed: {Message}", ex.Message);
                }

                if (clients != null)
                {
                    UpdateRoster(clients);
                    await EmitStatusAsync(_config.KeepConnected ? "Connected" : "Disconnected").ConfigureAwait(false);
                }

                var anyConnected = clients != null && clients.Any(c => c.IsConnected);
                // Retry only while no agent is connected yet, retries remain, and we are still armed — exactly
                // Unity's condition (an agent is expected to re-establish its session shortly after we connect).
                if (anyConnected || attempt >= RosterSeedRetryCount || !_config.KeepConnected)
                    return;

                _logger?.LogDebug("No AI agent in roster yet; retrying seed ({RetriesLeft} left).", RosterSeedRetryCount - attempt);
                try { await Task.Delay(RosterSeedRetryDelayMs, ct).ConfigureAwait(false); }
                catch (OperationCanceledException) { return; }
            }
        }

        /// <summary>Recompute + cache the connected-agent labels from a roster snapshot (under <see cref="_rosterLock"/>).</summary>
        private void UpdateRoster(IReadOnlyList<McpClientData>? clients)
        {
            var labels = BuildAgentLabels(clients);
            lock (_rosterLock)
                _connectedAgents = labels;
        }

        /// <summary>
        /// Clear the cached connected-agent roster (issue #111). Called on every disconnect/teardown path so a later
        /// reconnect re-seeds fresh from <see cref="SeedRosterAsync"/> rather than from a frozen pre-disconnect list.
        /// <see cref="EmitStatusAsync"/> already gates a stale cache from leaking under a non-connected state; this
        /// keeps the in-memory cache itself honest too (guarded by <see cref="_rosterLock"/>).
        /// </summary>
        private void ClearRoster()
        {
            lock (_rosterLock)
                _connectedAgents = new List<string>();
        }

        /// <summary>
        /// Project a roster snapshot to the §7 status labels: keep only connected clients, drop the bridge's own
        /// self-entry (defensive — the server reports MCP client sessions, not the asking plugin), DEDUPE by display
        /// identity (<c>ClientName</c>,<c>ClientVersion</c>), and format each as Unity's exact label
        /// <c>"AI agent: {ClientName} ({ClientVersion})"</c>. Pure + static so the bridge xUnit suite locks the
        /// filter/dedupe/format/self-exclusion matrix without a live plugin. Null/empty → empty list.
        ///
        /// Dedupe rationale (issue #111): the server's <c>GetMcpClientData</c>/<c>OnClientsChanged</c> reports one
        /// <see cref="McpClientData"/> per MCP SESSION (keyed by physicalId), and a single client (e.g. Claude Code)
        /// commonly holds 2+ live sessions — so without this collapse the row shows duplicate identical labels after
        /// merely reopening the project. Group by (ClientName, ClientVersion), NOT by SessionId: SessionId is
        /// per-session and would never collapse the duplicates we are trying to remove.
        /// </summary>
        internal static List<string> BuildAgentLabels(IReadOnlyList<McpClientData>? clients)
        {
            if (clients == null)
                return new List<string>();
            return clients
                .Where(c => c != null && c.IsConnected)
                .Where(c => !string.Equals(c.ClientName, SelfClientName, StringComparison.Ordinal))
                .GroupBy(c => (c.ClientName, c.ClientVersion))
                .Select(g => g.First())
                .Select(c => $"AI agent: {c.ClientName} ({c.ClientVersion})")
                .ToList();
        }

        /// <summary>Push a §1.3 <c>status</c> message to the plugin (the §7 live connection indicator).</summary>
        private Task EmitStatusAsync(string connectionState, string? cloudAuthState = null)
        {
            // §7 clear-on-non-connected invariant (issue #111, mirrors Unity's MainWindowEditor.Connection.cs):
            // the "AI agents" dot is driven by aiAgents.Num() > 0, so any non-"Connected" transition (Disconnect,
            // Connecting, Disconnected) MUST ship an EMPTY roster regardless of the cache — otherwise a user
            // Disconnect (which never fires OnClientsChanged on a torn-down link) leaves the row stale/green. The
            // disconnect paths ALSO clear _connectedAgents so a later reconnect re-seeds fresh, but this guard is
            // the authoritative gate: a stale cache can never leak out under a non-connected state.
            List<string> agents;
            if (!string.Equals(connectionState, "Connected", StringComparison.Ordinal))
            {
                agents = new List<string>();
            }
            else
            {
                lock (_rosterLock)
                    agents = _connectedAgents.ToList();
            }
            var status = new StatusMessage
            {
                ConnectionState = connectionState,
                KeepConnected = _config.KeepConnected,
                CloudAuthState = cloudAuthState,
                AiAgents = agents,
            };
            return _statusEmitter(status);
        }

        // Internal (not private) so the bridge xUnit suite can assert the initial connect is routed through the
        // serialized + supersedable transition queue (a bare plugin.Connect() would not be cancellable).
        internal Task ConnectSignalRAsync()
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
                    if (ct.IsCancellationRequested)
                    {
                        // Superseded — a newer transition owns the next dial + status emit. (The real
                        // ConnectionManager returns false rather than throwing OCE on a cancelled token.)
                        _logger?.LogDebug("SignalR initial connect superseded before it completed.");
                        return;
                    }
                    _logger?.LogInformation(ok ? "SignalR connected." : "SignalR initial connect returned false; client will keep retrying.");
                    // §7 live status: surface the connection result to the plugin's view-model. Only a CLOUD-mode
                    // credential is a cloud authorization — a Custom-mode token is a local bearer and must NOT light
                    // the "Authorized" indicator (ApplyStatus latches it and never demotes). mcp-authorize PR 5
                    // (design 06, D12): the machine credential store being populated ALSO counts as signed-in, so a
                    // zero-button boot (sign-in done out-of-editor: a CLI login, an enrollment, another engine/project)
                    // surfaces the signed-in state without an in-editor device flow.
                    var cloudAuthState = ResolveCloudAuthState(
                        _isCloudMode, !string.IsNullOrEmpty(BearerToken), _credentialStore?.Exists == true);
                    await EmitStatusAsync(ok ? "Connected" : "Connecting", cloudAuthState).ConfigureAwait(false);
                    // §7 (issue #109): once connected, seed the AI-agent roster (with retry/backoff) so the row is
                    // populated even before the first OnClientsChanged push. Fire-and-forget on a background task —
                    // it has its own retry delays and must not block the transition queue.
                    if (ok)
                        _ = SeedRosterAsync();
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
            _ipc.AgentConfigRequestReceived -= HandleAgentConfigRequest;
            try { _clientsChangedSubscription?.Dispose(); } catch { /* ignore */ }
            _clientsChangedSubscription = null;
            // Bug #116: drop the transport connection-state subscription so a late R3 callback cannot touch a
            // half-disposed host.
            try { _connectionStateSubscription?.Dispose(); } catch { /* ignore */ }
            _connectionStateSubscription = null;
            // §7 (issue #111): teardown clears the roster cache so it never outlives the host.
            ClearRoster();
            try { _authCts?.Cancel(); } catch { /* ignore */ }
            _authCts?.Dispose();
            // mcp-authorize: drop the on-401 coordinator + sign-in-required subscription + credential provider so no
            // late R3 callback touches a half-disposed host (the store itself is stateless/owned by the caller).
            try { _signInRequiredSubscription?.Dispose(); } catch { /* ignore */ }
            _signInRequiredSubscription = null;
            try { _credentialCoordinator?.Dispose(); } catch { /* ignore */ }
            _credentialCoordinator = null;
            try { _credentialProvider?.Dispose(); } catch { /* ignore */ }
            // Tear down the transition CTS under _transitionLock so a concurrent RunConnectionTransition cannot
            // observe a half-disposed field (or have its captured predecessor disposed out from under it): take
            // and null the field inside the lock, then cancel/dispose outside (cancel runs continuations
            // synchronously — keep them off the lock). Cancel-after-dispose / double-dispose are guarded.
            CancellationTokenSource? transitionCts;
            lock (_transitionLock)
            {
                transitionCts = _transitionCts;
                _transitionCts = null;
            }
            try { transitionCts?.Cancel(); } catch (ObjectDisposedException) { /* a racing submission disposed it */ }
            transitionCts?.Dispose();
            _ownedHttpClient?.Dispose(); // released here — the authenticator does not own it (default path)
            try { _plugin?.Dispose(); } catch { /* ignore */ }
            _plugin = null;
        }
    }
}
