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
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common.Hub.Server;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.ReflectorNet;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Microsoft.AspNetCore.SignalR.Client;
using Microsoft.Extensions.Logging;
using R3;
using McpVersion = com.IvanMurzak.McpPlugin.Common.Version;
// MCP-Plugin 6.8.0 introduced an upstream com.IvanMurzak.McpPlugin.ProxyTool, which collides with the
// bridge's local Tools.ProxyTool under this file's two usings. The FakeToolSink implements the LOCAL
// IProxyToolSink, so pin the bare name to the local type.
using ProxyTool = com.IvanMurzak.Unreal.MCP.Bridge.Tools.ProxyTool;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// A controllable <see cref="IMcpPlugin"/> for driving <see cref="SidecarHost"/>'s connection-transition
    /// orchestration (re-dial / supersede / status) without a live SignalR endpoint. Only <see cref="Connect"/>
    /// / <see cref="Disconnect"/> are meaningful — the rest of the (large) interface surface is never touched by
    /// the transition machinery and throws if accessed, surfacing any accidental new dependency loudly.
    /// </summary>
    internal sealed class FakeMcpPlugin : IMcpPlugin
    {
        private readonly Func<CancellationToken, Task<bool>> _onConnect;
        private readonly Func<CancellationToken, Task>? _onDisconnect;
        public int ConnectCalls;
        public int DisconnectCalls;

        // Roster surface (issue #109). Supplied only by tests that exercise the AI-agent roster path; left null by
        // the connection-transition tests, which never touch McpManager/McpManagerHub.
        private readonly IMcpManager? _mcpManager;
        private readonly IMcpManagerHub? _mcpManagerHub;

        // Bug #116: a controllable transport connection-state property so the connection-drop subscription path is
        // drivable without a live SignalR client. Tests push transitions via PushConnectionState; the property is
        // seeded Disconnected. The transition tests never read it (R3 lets it lie dormant), so they are unaffected.
        private readonly ReactiveProperty<HubConnectionState> _connectionState = new(HubConnectionState.Disconnected);

        public FakeMcpPlugin(
            Func<CancellationToken, Task<bool>> onConnect,
            Func<CancellationToken, Task>? onDisconnect = null,
            IMcpManager? mcpManager = null,
            IMcpManagerHub? mcpManagerHub = null)
        {
            _onConnect = onConnect;
            _onDisconnect = onDisconnect;
            _mcpManager = mcpManager;
            _mcpManagerHub = mcpManagerHub;
        }

        /// <summary>Bug #116: drive a transport connection-state transition through <see cref="ConnectionState"/>.</summary>
        public void PushConnectionState(HubConnectionState state) => _connectionState.Value = state;

        public Task<bool> Connect(CancellationToken cancellationToken = default)
        {
            Interlocked.Increment(ref ConnectCalls);
            return _onConnect(cancellationToken);
        }

        public Task Disconnect(CancellationToken cancellationToken = default)
        {
            Interlocked.Increment(ref DisconnectCalls);
            return _onDisconnect?.Invoke(cancellationToken) ?? Task.CompletedTask;
        }

        public void DisconnectImmediate() { }
        // McpPlugin 6.10.0 added IConnection.WaitForImmediateTeardown(TimeSpan) — the immediate-teardown
        // path is not exercised by the transition orchestration under test, so report "torn down".
        public bool WaitForImmediateTeardown(TimeSpan timeout) => true;
        public void Dispose() { }

        // Unused surface — the transition orchestration under test never reads these. Throwing keeps the fake
        // honest: if a future change starts depending on one, the test fails loudly instead of silently.
        public ReadOnlyReactiveProperty<bool> KeepConnected => throw new NotSupportedException();
        // Bug #116: a real (controllable) ConnectionState so the connection-drop subscription is testable; the
        // transition tests never subscribe, so seeding it does not perturb them.
        public ReadOnlyReactiveProperty<HubConnectionState> ConnectionState => _connectionState;
        public Observable<Unit> OnAuthorizationRejected => throw new NotSupportedException();
        public ILogger Logger => throw new NotSupportedException();
        // Roster path (issue #109): return the injected fakes when supplied; otherwise throw to keep the
        // connection-transition tests honest (they must never depend on the roster surface).
        public IMcpManager McpManager => _mcpManager ?? throw new NotSupportedException();
        public IMcpManagerHub? McpManagerHub => _mcpManagerHub;
        public McpVersion Version => throw new NotSupportedException();
        public VersionHandshakeResponse? VersionHandshakeStatus => throw new NotSupportedException();
        public bool GenerateSkillFiles(string? path = null) => throw new NotSupportedException();
        public bool GenerateSkillFilesIfNeeded(string? path = null) => throw new NotSupportedException();
        public bool DeleteSkillFiles(string? path = null) => throw new NotSupportedException();
    }

    /// <summary>A scripted <see cref="IToolCallChannel"/> for testing the proxy/registration path.</summary>
    public sealed class FakeToolCallChannel : IToolCallChannel
    {
        private readonly Func<string, JsonObject?, ToolResponseMessage>? _responder;
        public bool Connected { get; set; } = true;

        public string? LastTool { get; private set; }
        public JsonObject? LastArguments { get; private set; }
        public int Calls { get; private set; }

        public FakeToolCallChannel(Func<string, JsonObject?, ToolResponseMessage>? responder = null)
            => _responder = responder;

        public Task<ToolResponseMessage> CallToolAsync(string tool, JsonObject? arguments, int timeoutMs, CancellationToken cancellationToken)
        {
            Calls++;
            LastTool = tool;
            LastArguments = arguments;
            if (!Connected)
                throw new IpcDisconnectedException();

            var response = _responder?.Invoke(tool, arguments) ?? new ToolResponseMessage
            {
                RequestId = "fake",
                Status = IpcProtocol.Status.Success,
            };
            return Task.FromResult(response);
        }
    }

    /// <summary>An in-memory <see cref="IProxyToolSink"/> recording the registrar's mutations.</summary>
    public sealed class FakeToolSink : IProxyToolSink
    {
        public readonly Dictionary<string, ProxyTool> Tools = new();
        public readonly Dictionary<string, bool> Enabled = new();

        public bool HasTool(string name) => Tools.ContainsKey(name);

        public bool AddTool(string name, ProxyTool tool)
        {
            if (Tools.ContainsKey(name))
                return false;
            Tools[name] = tool;
            Enabled[name] = tool.Enabled;
            return true;
        }

        public bool RemoveTool(string name)
        {
            Enabled.Remove(name);
            return Tools.Remove(name);
        }

        public bool SetToolEnabled(string name, bool enabled)
        {
            if (!Tools.ContainsKey(name))
                return false;
            Enabled[name] = enabled;
            return true;
        }
    }

    /// <summary>
    /// A controllable <see cref="IMcpManager"/> for driving the connected-AI-agent roster path (issue #109): the
    /// test pushes new rosters through <see cref="PushClients"/> (an R3 <see cref="Subject{T}"/> backing
    /// <see cref="OnClientsChanged"/>), and the host's subscription caches + re-emits status. Only the roster
    /// surface is meaningful — the rest of the interface throws so an accidental new dependency surfaces loudly.
    /// </summary>
    internal sealed class FakeMcpManager : IMcpManager
    {
        private readonly Subject<IReadOnlyList<McpClientData>> _onClientsChanged = new();

        /// <summary>Push a roster snapshot through <see cref="OnClientsChanged"/> (an agent join/leave event).</summary>
        public void PushClients(IReadOnlyList<McpClientData> clients) => _onClientsChanged.OnNext(clients);

        public Observable<IReadOnlyList<McpClientData>> OnClientsChanged => _onClientsChanged;

        public void Dispose() => _onClientsChanged.Dispose();

        // Unused surface — the roster wiring under test never reads these.
        public Reflector Reflector => throw new NotSupportedException();
        public IReadOnlyList<McpClientData> ActiveClients => throw new NotSupportedException();
        public Observable<Unit> OnForceDisconnect => throw new NotSupportedException();
        public Observable<McpClientData> OnClientConnected => throw new NotSupportedException();
        public Observable<McpClientData> OnClientDisconnected => throw new NotSupportedException();
        public IToolManager? ToolManager => throw new NotSupportedException();
        public IPromptManager? PromptManager => throw new NotSupportedException();
        public IResourceManager? ResourceManager => throw new NotSupportedException();
        public ISystemToolManager? SystemToolManager => throw new NotSupportedException();
    }

    /// <summary>
    /// A controllable <see cref="IMcpManagerHub"/> for the roster SEED path (issue #109): each
    /// <see cref="GetMcpClientData"/> call returns the next scripted snapshot (or the last one once the script is
    /// exhausted), letting a test model "empty, then an agent appears on retry". Only that one method is
    /// meaningful; the (large) inherited hub surface throws.
    /// </summary>
    internal sealed class FakeMcpManagerHub : IMcpManagerHub
    {
        private readonly Queue<McpClientData[]> _scripted;
        private McpClientData[] _last;
        public int GetMcpClientDataCalls { get; private set; }

        public FakeMcpManagerHub(params McpClientData[][] scriptedSnapshots)
        {
            _scripted = new Queue<McpClientData[]>(scriptedSnapshots);
            _last = scriptedSnapshots.Length > 0 ? scriptedSnapshots[^1] : Array.Empty<McpClientData>();
        }

        public Task<McpClientData[]> GetMcpClientData()
        {
            GetMcpClientDataCalls++;
            var snapshot = _scripted.Count > 0 ? _scripted.Dequeue() : _last;
            return Task.FromResult(snapshot);
        }

        public void Dispose() { }

        // Unused inherited surface (IConnectServerHub / IServerMcpManager / IServer*Hub).
        public VersionHandshakeResponse VersionHandshakeStatus => throw new NotSupportedException();
        public Task<bool> Connect(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task Disconnect(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public void DisconnectImmediate() => throw new NotSupportedException();
        public bool WaitForImmediateTeardown(TimeSpan timeout) => throw new NotSupportedException();
        public ReadOnlyReactiveProperty<bool> KeepConnected => throw new NotSupportedException();
        public ReadOnlyReactiveProperty<HubConnectionState> ConnectionState => throw new NotSupportedException();
        public Observable<Unit> OnAuthorizationRejected => throw new NotSupportedException();
        public Task<VersionHandshakeResponse> PerformVersionHandshake(RequestVersionHandshake request) => throw new NotSupportedException();
        public Task<McpServerData> GetMcpServerData() => throw new NotSupportedException();
        public Task<ResponseData> NotifyAboutUpdatedTools(RequestToolsUpdated request) => throw new NotSupportedException();
        public Task<ResponseData> NotifyToolRequestCompleted(RequestToolCompletedData request) => throw new NotSupportedException();
        public Task<ResponseData> NotifyAboutUpdatedPrompts(RequestPromptsUpdated request) => throw new NotSupportedException();
        public Task<ResponseData> NotifyAboutUpdatedResources(RequestResourcesUpdated request) => throw new NotSupportedException();
    }
}
