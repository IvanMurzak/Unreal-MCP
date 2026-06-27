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
using System.Net.Sockets;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Sidecar;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Ipc
{
    /// <summary>
    /// The sidecar's IPC client (docs/ARCHITECTURE.md §1): dials the plugin's localhost TCP listener,
    /// performs the stdin-token handshake (§1.4), reads NDJSON messages (§1.2), routes them, sends
    /// heartbeats (§1.3), and reconnects with backoff (§1.5). Implements <see cref="IToolCallChannel"/>
    /// so <see cref="ProxyTool"/>s round-trip calls through it. The reader loop never executes tool
    /// bodies — it completes the pending-call task and the proxy/handler continuation runs off the
    /// reader thread (RunContinuationsAsynchronously). One mutex-guarded writer serializes every send so
    /// messages never interleave (§1.2).
    /// </summary>
    public sealed class IpcClient : IToolCallChannel, IPromptCallChannel, IResourceCallChannel, IAsyncDisposable
    {
        private readonly string _host;
        private readonly int _port;
        private readonly string _token;
        private readonly string _sidecarVersion;
        private readonly ILogger? _logger;

        private readonly PendingCallRegistry _pending = new();
        // v2 (§A.1) prompt-get / resource-read pending registries — reuse the same correlation machinery as
        // tool-calls (generic over the response type). Failed/drained alongside _pending on every disconnect.
        private readonly PendingCallRegistry<PromptResponseMessage> _pendingPrompts = new();
        private readonly PendingCallRegistry<ResourceResponseMessage> _pendingResources = new();
        private readonly ReconnectBackoff _backoff = new();
        private readonly SemaphoreSlim _writeLock = new(1, 1);

        private TcpClient? _tcp;
        private NetworkStream? _stream;
        private volatile bool _shutdownRequested;
        // True only between a received handshake-ack and the connection's teardown. CallToolAsync gates on
        // this (not merely an open socket) so a call cannot win the write race against the handshake.
        private volatile bool _ackAccepted;
        private long _lastActivityTicks;

        /// <summary>Routes applied tool manifests; set by the host before <see cref="RunAsync"/>.</summary>
        public ManifestRegistrar? Registrar { get; set; }

        /// <summary>
        /// Routes applied prompt manifests (IPC v2, §A.1); set by the host (P1) before <see cref="RunAsync"/>.
        /// Null in the P0 scaffold — a <c>prompt-manifest</c> that arrives with no registrar wired is logged
        /// and dropped (forward-compatible: the plugin never pushes one until both peers negotiate v2 and a
        /// prompt registry exists).
        /// </summary>
        public IManifestSink<PromptManifestMessage>? PromptRegistrar { get; set; }

        /// <summary>
        /// Routes applied resource manifests (IPC v2, §A.1); set by the host (P2) before <see cref="RunAsync"/>.
        /// Null in the P0 scaffold — see <see cref="PromptRegistrar"/>.
        /// </summary>
        public IManifestSink<ResourceManifestMessage>? ResourceRegistrar { get; set; }

        /// <summary>
        /// The IPC version negotiated on the most recent accepted handshake (<see cref="IpcProtocol.NegotiateVersion"/>):
        /// the minimum of this sidecar's <see cref="IpcProtocol.IpcVersion"/> and the version the plugin advertised in
        /// the handshake-ack. <c>0</c> until the first ack. When below <see cref="IpcProtocol.PromptsResourcesMinVersion"/>
        /// the link is tools-only and the prompt/resource families are never exchanged (an old peer keeps working).
        /// </summary>
        public int NegotiatedIpcVersion { get; private set; }

        /// <summary>True once a handshake negotiated an IPC version that exchanges the v2 prompt/resource families.</summary>
        public bool SupportsPromptsResources => IpcProtocol.SupportsPromptsResources(NegotiatedIpcVersion);

        /// <summary>Raised when a <c>handshake-ack</c> is received (the link is established, §1.5).</summary>
        public event Action<HandshakeAckMessage>? HandshakeAccepted;

        /// <summary>Raised when a connection attempt's handshake was rejected (socket closed pre-ack, §1.4).</summary>
        public event Action? HandshakeRejected;

        /// <summary>Raised when the plugin sends <c>shutdown</c> (editor quitting, §1.5).</summary>
        public event Action? ShutdownRequested;

        /// <summary>
        /// Raised when the plugin pushes a §1.3 <c>config</c> message (effective connection config changed,
        /// §8). The argument is the parsed message object (flat: <c>mode</c>/<c>host</c>/<c>cloudUrl</c>/
        /// <c>token</c>/<c>keepConnected</c> at the top level).
        /// </summary>
        public event Action<JsonObject>? ConfigReceived;

        /// <summary>
        /// Raised when the plugin pushes a §1.3 auth message (<c>auth-start</c> / <c>auth-cancel</c> /
        /// <c>auth-revoke</c>). The argument is the message <c>type</c>. The full device-code flow lands with
        /// the UI task; the sidecar handles these gracefully as stubs today (§8).
        /// </summary>
        public event Action<string>? AuthMessageReceived;

        /// <summary>
        /// Raised when the plugin sends a §7 AI-agent configurator request (<c>agents-list</c> /
        /// <c>agent-status</c> / <c>agent-configure</c> / <c>agent-remove</c> / <c>agent-skills-path</c>). The
        /// arguments are the request <c>type</c> and the raw parsed JSON object; the host deserializes the
        /// concrete request, serves it against the shared AgentConfig library, and sends back an
        /// <c>agent-config-result</c> via <see cref="SendToPluginAsync"/>. Invoked off the reader thread.
        /// </summary>
        public event Action<string, JsonObject>? AgentConfigRequestReceived;

        public IpcClient(string host, int port, string token, string sidecarVersion, ILogger? logger = null)
        {
            _host = host;
            _port = port;
            _token = token ?? throw new ArgumentNullException(nameof(token));
            _sidecarVersion = sidecarVersion;
            _logger = logger;
        }

        public bool IsConnected => _stream != null && _tcp is { Connected: true };

        /// <summary>
        /// Connect → serve → reconnect until <paramref name="ct"/> is cancelled or the plugin requests
        /// shutdown. Each iteration: dial, handshake, and (on accept) serve the read+heartbeat loop until
        /// the link drops; then back off and retry (§1.5).
        /// </summary>
        public async Task RunAsync(CancellationToken ct)
        {
            while (!ct.IsCancellationRequested && !_shutdownRequested)
            {
                var accepted = await TryConnectAndServeAsync(ct).ConfigureAwait(false);

                if (_shutdownRequested || ct.IsCancellationRequested)
                    break;

                // On a rejected handshake we do NOT reset the backoff; on an accepted-then-dropped link the
                // serve loop already reset it on accept, so the next reconnect starts from 1 s.
                var delay = _backoff.Next();
                _logger?.LogInformation("IPC reconnecting in {DelayMs} ms (accepted={Accepted}).",
                    (int)delay.TotalMilliseconds, accepted);
                try { await Task.Delay(delay, ct).ConfigureAwait(false); }
                catch (OperationCanceledException) { break; }
            }
            _logger?.LogInformation("IPC client run loop exited (shutdown={Shutdown}).", _shutdownRequested);
        }

        private async Task<bool> TryConnectAndServeAsync(CancellationToken ct)
        {
            using var connCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            var tcp = new TcpClient();
            var ackTcs = new TaskCompletionSource<HandshakeAckMessage>(TaskCreationOptions.RunContinuationsAsynchronously);

            try
            {
                _logger?.LogInformation("IPC dialing {Host}:{Port} ...", _host, _port);
                try
                {
                    await tcp.ConnectAsync(_host, _port, connCts.Token).ConfigureAwait(false);
                }
                catch (Exception ex) when (ex is not OperationCanceledException)
                {
                    // A failed DIAL is a liveness event (plugin not listening yet / port not up), NOT a
                    // handshake rejection (§6). It must NOT count toward MaxConsecutiveRejections — otherwise
                    // 3 transient connect failures would be fatal. Just reconnect; the 60 s no-success
                    // deadline + the parent-process monitor are what bound a truly dead link.
                    _logger?.LogDebug("IPC dial failed: {Message}", ex.Message);
                    return false;
                }
                tcp.NoDelay = true;

                _tcp = tcp;
                _stream = tcp.GetStream();
                Touch();

                // §1.4: send the handshake carrying the one-shot token as the FIRST message.
                await SendAsync(new HandshakeMessage
                {
                    IpcVersion = IpcProtocol.IpcVersion,
                    SidecarVersion = _sidecarVersion,
                    Token = _token,
                }, connCts.Token).ConfigureAwait(false);

                // Serve the read loop; it completes ackTcs on handshake-ack.
                var readTask = ReadLoopAsync(_stream, ackTcs, connCts.Token);

                // Wait for the ack (or the read loop ending first = socket closed pre-ack = rejection, §1.4).
                var acceptTimeout = Task.Delay(TimeSpan.FromSeconds(10), connCts.Token);
                var first = await Task.WhenAny(ackTcs.Task, readTask, acceptTimeout).ConfigureAwait(false);

                if (first == ackTcs.Task && ackTcs.Task.IsCompletedSuccessfully)
                {
                    _backoff.Reset();
                    // §A.1 version negotiation: the link runs at min(local, plugin) — the highest both understand.
                    // A v1 plugin negotiates down to 1 (tools-only); a v2 plugin enables the prompt/resource families.
                    // The tool path is identical at every version, so a mismatch never breaks tools (an old peer works).
                    NegotiatedIpcVersion = IpcProtocol.NegotiateVersion(IpcProtocol.IpcVersion, ackTcs.Task.Result.IpcVersion);
                    _logger?.LogInformation("IPC version negotiated: {Negotiated} (local {Local}, plugin {Remote}); prompts/resources {State}.",
                        NegotiatedIpcVersion, IpcProtocol.IpcVersion, ackTcs.Task.Result.IpcVersion,
                        SupportsPromptsResources ? "ENABLED" : "tools-only");
                    _ackAccepted = true; // open the gate: tool-calls may now hit the wire (§1.4)
                    HandshakeAccepted?.Invoke(ackTcs.Task.Result);
                    using var heartbeat = StartHeartbeat(connCts);
                    await readTask.ConfigureAwait(false); // serve until the link drops
                    return true;
                }

                // No ack: timeout or socket closed first → treat as a rejected/failed handshake (§1.4).
                _logger?.LogWarning("IPC handshake not acknowledged (timeout or socket closed pre-ack).");
                HandshakeRejected?.Invoke();
                connCts.Cancel();
                await SafeAwait(readTask).ConfigureAwait(false);
                return false;
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested || _shutdownRequested)
            {
                return false;
            }
            catch (Exception ex)
            {
                // Post-dial I/O error (e.g. the peer reset the socket mid-handshake). Treat it as liveness
                // and reconnect WITHOUT counting a rejection — only a socket the plugin closed pre-ack after
                // a completed dial (the no-ack branch above) is a genuine §1.4 handshake rejection.
                _logger?.LogWarning("IPC connection attempt failed: {Message}", ex.Message);
                return false;
            }
            finally
            {
                CloseConnection(tcp);
                _pending.FailAll(); // every in-flight proxy call fails fast (§2.2 step 4)
                _pendingPrompts.FailAll();   // v2: in-flight prompt-get/resource-read fail fast too (§A.1)
                _pendingResources.FailAll();
                Registrar?.ResetForReconnect(); // §1.5: next handshake-ack re-applies the manifest
                PromptRegistrar?.ResetForReconnect();
                ResourceRegistrar?.ResetForReconnect();
            }
        }

        private async Task ReadLoopAsync(NetworkStream stream, TaskCompletionSource<HandshakeAckMessage> ackTcs, CancellationToken ct)
        {
            var framer = new NdjsonFramer();
            var buffer = new byte[64 * 1024];
            try
            {
                while (!ct.IsCancellationRequested)
                {
                    var read = await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), ct).ConfigureAwait(false);
                    if (read == 0)
                        break; // peer closed

                    Touch();
                    foreach (var line in framer.Push(buffer.AsSpan(0, read)))
                    {
                        if (!string.IsNullOrWhiteSpace(line))
                            Dispatch(line, ackTcs, ct);
                    }
                }
            }
            catch (OperationCanceledException) { /* expected on teardown */ }
            catch (Exception ex)
            {
                _logger?.LogWarning("IPC read loop ended: {Message}", ex.Message);
            }
        }

        private void Dispatch(string line, TaskCompletionSource<HandshakeAckMessage> ackTcs, CancellationToken ct)
        {
            JsonNode? node;
            try { node = JsonNode.Parse(line); }
            catch (Exception ex) { _logger?.LogWarning("IPC dropped malformed line: {Message}", ex.Message); return; }

            var type = node?["type"]?.GetValue<string>();
            switch (type)
            {
                case IpcProtocol.Type.HandshakeAck:
                {
                    var ack = node!.Deserialize<HandshakeAckMessage>(IpcProtocol.JsonOptions) ?? new HandshakeAckMessage();
                    ackTcs.TrySetResult(ack);
                    break;
                }
                case IpcProtocol.Type.ToolManifest:
                {
                    var manifest = node!.Deserialize<ToolManifestMessage>(IpcProtocol.JsonOptions);
                    if (manifest != null) Registrar?.Apply(manifest);
                    break;
                }
                case IpcProtocol.Type.ToolResponse:
                {
                    var response = node!.Deserialize<ToolResponseMessage>(IpcProtocol.JsonOptions);
                    if (response != null && !_pending.TryComplete(response.RequestId, response))
                        _logger?.LogDebug("Dropped tool-response for unknown/completed requestId {Id}.", response.RequestId);
                    break;
                }
                case IpcProtocol.Type.PromptManifest:
                {
                    // v2 (§A.1): apply the prompt-set snapshot. Dropped (logged) when no registrar is wired — the
                    // P0 scaffold has none, and a v1-negotiated link never receives this anyway.
                    var manifest = node!.Deserialize<PromptManifestMessage>(IpcProtocol.JsonOptions);
                    if (manifest != null && PromptRegistrar != null)
                        PromptRegistrar.ApplyManifest(manifest);
                    else if (manifest != null)
                        _logger?.LogDebug("Received prompt-manifest but no PromptRegistrar is wired (scaffold); dropping.");
                    break;
                }
                case IpcProtocol.Type.ResourceManifest:
                {
                    var manifest = node!.Deserialize<ResourceManifestMessage>(IpcProtocol.JsonOptions);
                    if (manifest != null && ResourceRegistrar != null)
                        ResourceRegistrar.ApplyManifest(manifest);
                    else if (manifest != null)
                        _logger?.LogDebug("Received resource-manifest but no ResourceRegistrar is wired (scaffold); dropping.");
                    break;
                }
                case IpcProtocol.Type.PromptResponse:
                {
                    var response = node!.Deserialize<PromptResponseMessage>(IpcProtocol.JsonOptions);
                    if (response != null && !_pendingPrompts.TryComplete(response.RequestId, response))
                        _logger?.LogDebug("Dropped prompt-response for unknown/completed requestId {Id}.", response.RequestId);
                    break;
                }
                case IpcProtocol.Type.ResourceResponse:
                {
                    var response = node!.Deserialize<ResourceResponseMessage>(IpcProtocol.JsonOptions);
                    if (response != null && !_pendingResources.TryComplete(response.RequestId, response))
                        _logger?.LogDebug("Dropped resource-response for unknown/completed requestId {Id}.", response.RequestId);
                    break;
                }
                case IpcProtocol.Type.Ping:
                    // Fire-and-forget pong, but observe the task so a send fault on a dropped link does not
                    // surface as an unobserved TaskException on the finalizer thread.
                    _ = SafeAwait(SendAsync(new HeartbeatMessage { Type = IpcProtocol.Type.Pong }, ct));
                    break;
                case IpcProtocol.Type.Pong:
                    break; // liveness already refreshed by Touch()
                case IpcProtocol.Type.Shutdown:
                    _logger?.LogInformation("IPC received shutdown from plugin; exiting.");
                    _shutdownRequested = true;
                    ShutdownRequested?.Invoke();
                    break;
                case IpcProtocol.Type.Config:
                {
                    // §8: the plugin pushed the effective connection config. Hand the flat object to the host,
                    // which applies mode-aware host/token/keepConnected. Never log the token (§8).
                    if (node is JsonObject configObj)
                        ConfigReceived?.Invoke(configObj);
                    break;
                }
                case IpcProtocol.Type.AuthStart:
                case IpcProtocol.Type.AuthCancel:
                case IpcProtocol.Type.AuthRevoke:
                    // §8 auth plumbing — handled gracefully as stubs pending the full device-code UI flow.
                    AuthMessageReceived?.Invoke(type);
                    break;
                case IpcProtocol.Type.AgentsList:
                case IpcProtocol.Type.AgentStatus:
                case IpcProtocol.Type.AgentConfigure:
                case IpcProtocol.Type.AgentRemove:
                case IpcProtocol.Type.AgentSkillsPath:
                case IpcProtocol.Type.AgentGenerateSkills:
                    // §7 AI-agent configurator requests. The host serves them against the shared AgentConfig
                    // library and answers with an `agent-config-result` (off the reader thread).
                    if (node is JsonObject agentObj)
                        AgentConfigRequestReceived?.Invoke(type, agentObj);
                    break;
                case IpcProtocol.Type.Status:
                case IpcProtocol.Type.Log:
                    // Wired in the later UI task; logged for now.
                    _logger?.LogDebug("IPC received '{Type}' (not handled in the sidecar-bridge MVP).", type);
                    break;
                default:
                    _logger?.LogDebug("IPC received unknown message type '{Type}'.", type);
                    break;
            }
        }

        private IDisposable StartHeartbeat(CancellationTokenSource connCts)
        {
            var cts = CancellationTokenSource.CreateLinkedTokenSource(connCts.Token);
            _ = Task.Run(async () =>
            {
                try
                {
                    while (!cts.IsCancellationRequested)
                    {
                        await Task.Delay(IpcProtocol.HeartbeatIntervalMs, cts.Token).ConfigureAwait(false);

                        var silentMs = (DateTime.UtcNow - new DateTime(Interlocked.Read(ref _lastActivityTicks), DateTimeKind.Utc)).TotalMilliseconds;
                        if (silentMs > IpcProtocol.HeartbeatTimeoutMs)
                        {
                            _logger?.LogWarning("IPC peer silent for {SilentMs} ms (> {Timeout} ms); treating as dead.",
                                (int)silentMs, IpcProtocol.HeartbeatTimeoutMs);
                            connCts.Cancel(); // drop the link → reconnect
                            break;
                        }
                        await SendAsync(new HeartbeatMessage { Type = IpcProtocol.Type.Ping }, cts.Token).ConfigureAwait(false);
                    }
                }
                catch (OperationCanceledException) { /* teardown */ }
                catch (Exception ex) { _logger?.LogDebug("IPC heartbeat ended: {Message}", ex.Message); }
            }, cts.Token);

            // Disposing the heartbeat MUST cancel it, not just dispose the CTS: a plain Dispose() of a linked
            // CTS does not cancel its token, so the heartbeat loop would survive into the next reconnect epoch
            // (writing a stray ping into the new connection's stream) until it happened to throw
            // ObjectDisposedException. Cancel first, then dispose.
            return new CancelOnDispose(cts);
        }

        /// <summary>Cancels the wrapped <see cref="CancellationTokenSource"/> on dispose, then disposes it.</summary>
        private sealed class CancelOnDispose : IDisposable
        {
            private readonly CancellationTokenSource _cts;
            public CancelOnDispose(CancellationTokenSource cts) => _cts = cts;
            public void Dispose()
            {
                try { _cts.Cancel(); } catch (ObjectDisposedException) { /* already torn down */ }
                _cts.Dispose();
            }
        }

        // --- IToolCallChannel -------------------------------------------------------------------------

        /// <summary>
        /// Send a <c>tool-call</c> and await its terminal <c>tool-response</c> (§2.2). Delegates to the shared
        /// <see cref="RoundTripAsync"/> round-trip — the same handshake gate, requestId correlation,
        /// cancellation-as-<c>tool-cancel</c>, and local timeout backstop that <see cref="GetPromptAsync"/> and
        /// <see cref="ReadResourceAsync"/> use. Throws <see cref="IpcDisconnectedException"/> when the link is down.
        /// </summary>
        public Task<ToolResponseMessage> CallToolAsync(string tool, JsonObject? arguments, int timeoutMs, CancellationToken cancellationToken)
            => RoundTripAsync(_pending,
                requestId => new ToolCallMessage { RequestId = requestId, Tool = tool, Arguments = arguments, TimeoutMs = timeoutMs },
                $"Tool '{tool}'", timeoutMs, cancellationToken);

        // --- Prompt/Resource outbound channels (IPC v2, §A.1) ----------------------------------------

        /// <summary>
        /// Send a <c>prompt-get</c> and await its terminal <c>prompt-response</c> (IPC v2, §A.1). Mirrors
        /// <see cref="CallToolAsync"/> exactly — gates on a completed handshake, correlates by requestId via
        /// the prompt pending registry, forwards cancellation as the generic <c>tool-cancel</c>, and arms the
        /// same local timeout backstop. Throws <see cref="IpcDisconnectedException"/> when the link is down so
        /// the proxy (P1) fails fast. The plugin only serves these on a v2-negotiated link.
        /// </summary>
        public Task<PromptResponseMessage> GetPromptAsync(string prompt, JsonObject? arguments, int timeoutMs, CancellationToken cancellationToken)
            => RoundTripAsync(_pendingPrompts,
                requestId => new PromptGetMessage { RequestId = requestId, Prompt = prompt, Arguments = arguments, TimeoutMs = timeoutMs },
                $"Prompt '{prompt}'", timeoutMs, cancellationToken);

        /// <summary>
        /// Send a <c>resource-read</c> and await its terminal <c>resource-response</c> (IPC v2, §A.1). The
        /// resource analog of <see cref="GetPromptAsync"/>; same handshake gate, requestId correlation,
        /// <c>tool-cancel</c> forwarding, and timeout backstop.
        /// </summary>
        public Task<ResourceResponseMessage> ReadResourceAsync(string uri, int timeoutMs, CancellationToken cancellationToken)
            => RoundTripAsync(_pendingResources,
                requestId => new ResourceReadMessage { RequestId = requestId, Uri = uri, TimeoutMs = timeoutMs },
                $"Resource '{uri}'", timeoutMs, cancellationToken);

        /// <summary>
        /// The shared request→response round-trip for every correlated IPC channel — tool-call (via
        /// <see cref="CallToolAsync"/>), prompt-get (<see cref="GetPromptAsync"/>), and resource-read
        /// (<see cref="ReadResourceAsync"/>) — parameterized over the pending registry + request factory so the
        /// handshake gate, requestId correlation, cancellation-as-<c>tool-cancel</c>, and local timeout backstop
        /// are written once. <typeparamref name="TResponse"/> is the matching terminal response type.
        /// </summary>
        private async Task<TResponse> RoundTripAsync<TResponse>(
            PendingCallRegistry<TResponse> registry,
            Func<string, object> buildRequest,
            string what,
            int timeoutMs,
            CancellationToken cancellationToken)
        {
            // Gate on a COMPLETED handshake, not merely an open socket: _stream/_tcp are published right after
            // the dial — BEFORE the handshake-ack — and on a reconnect epoch the proxies from the prior epoch
            // are still registered. A SignalR-driven call landing in that pre-ack window would win the write
            // race against the handshake, hit the wire first, and be silently dropped by the plugin's
            // pre-handshake auth gate, with no response ever returning. Fail fast pre-ack instead (§1.4).
            var stream = _stream;
            if (stream == null || !_ackAccepted || _tcp is not { Connected: true })
                throw new IpcDisconnectedException();

            var requestId = Guid.NewGuid().ToString("N");
            var task = registry.Register(requestId);

            // Cancellation: forward the caller's token as the GENERIC tool-cancel (by requestId, reused for all
            // kinds, §A.1) and fail the pending call.
            using var registration = cancellationToken.Register(() =>
            {
                _ = SafeAwait(SendAsync(new ToolCancelMessage { RequestId = requestId }, CancellationToken.None));
                registry.TryFail(requestId, new OperationCanceledException(cancellationToken));
            });

            try
            {
                await SendAsync(buildRequest(requestId), cancellationToken).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                registry.TryFail(requestId, new IpcDisconnectedException());
                throw new IpcDisconnectedException();
            }

            if (timeoutMs > 0)
            {
                using var backstopCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                var completed = await Task.WhenAny(task, Task.Delay(timeoutMs + IpcProtocol.CallTimeoutGraceMs, backstopCts.Token)).ConfigureAwait(false);
                backstopCts.Cancel();
                if (completed != task && !cancellationToken.IsCancellationRequested)
                {
                    var timeout = new TimeoutException($"{what} call timed out after {timeoutMs + IpcProtocol.CallTimeoutGraceMs} ms with no IPC response.");
                    _ = SafeAwait(SendAsync(new ToolCancelMessage { RequestId = requestId }, CancellationToken.None));
                    registry.TryFail(requestId, timeout);
                    throw timeout;
                }
            }

            return await task.ConfigureAwait(false);
        }

        /// <summary>
        /// Send a sidecar→plugin message (the §7 <c>device-auth</c> / <c>status</c> feed) over the live IPC
        /// link. No-op (returns false) when no connection is up — the plugin re-reads state on the next
        /// handshake anyway. Never throws on a dropped link; the single mutex-guarded writer keeps the frame
        /// from interleaving with tool traffic (§1.2). The host calls this off the reader thread.
        /// </summary>
        public async Task<bool> SendToPluginAsync<T>(T message, CancellationToken ct = default)
        {
            if (_stream == null || _tcp is not { Connected: true })
                return false;
            try
            {
                await SendAsync(message, ct).ConfigureAwait(false);
                return true;
            }
            catch (Exception ex)
            {
                _logger?.LogDebug("SendToPluginAsync failed (link down?): {Message}", ex.Message);
                return false;
            }
        }

        // --- writer (single mutex-guarded path, §1.2) -------------------------------------------------

        private async Task SendAsync<T>(T message, CancellationToken ct)
        {
            var stream = _stream;
            if (stream == null)
                throw new IpcDisconnectedException();

            var json = JsonSerializer.Serialize(message, IpcProtocol.JsonOptions);
            var framed = NdjsonFramer.Encode(json);

            await _writeLock.WaitAsync(ct).ConfigureAwait(false);
            try
            {
                await stream.WriteAsync(framed.AsMemory(), ct).ConfigureAwait(false);
                await stream.FlushAsync(ct).ConfigureAwait(false);
            }
            finally
            {
                _writeLock.Release();
            }
        }

        private void Touch() => Interlocked.Exchange(ref _lastActivityTicks, DateTime.UtcNow.Ticks);

        private void CloseConnection(TcpClient tcp)
        {
            _ackAccepted = false; // close the gate: the next epoch must re-handshake before any tool-call
            try { _stream?.Dispose(); } catch { /* ignore */ }
            try { tcp.Close(); } catch { /* ignore */ }
            _stream = null;
            if (ReferenceEquals(_tcp, tcp)) _tcp = null;
        }

        private static async Task SafeAwait(Task task)
        {
            try { await task.ConfigureAwait(false); } catch { /* swallow on teardown */ }
        }

        public ValueTask DisposeAsync()
        {
            _shutdownRequested = true;
            _ackAccepted = false;
            try { _stream?.Dispose(); } catch { /* ignore */ }
            try { _tcp?.Close(); } catch { /* ignore */ }
            _writeLock.Dispose();
            _pending.FailAll();
            _pendingPrompts.FailAll();
            _pendingResources.FailAll();
            return ValueTask.CompletedTask;
        }
    }
}
