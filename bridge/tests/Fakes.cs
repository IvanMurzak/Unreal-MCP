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
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Microsoft.AspNetCore.SignalR.Client;
using Microsoft.Extensions.Logging;
using R3;
using McpVersion = com.IvanMurzak.McpPlugin.Common.Version;

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

        public FakeMcpPlugin(Func<CancellationToken, Task<bool>> onConnect, Func<CancellationToken, Task>? onDisconnect = null)
        {
            _onConnect = onConnect;
            _onDisconnect = onDisconnect;
        }

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
        public void Dispose() { }

        // Unused surface — the transition orchestration under test never reads these. Throwing keeps the fake
        // honest: if a future change starts depending on one, the test fails loudly instead of silently.
        public ReadOnlyReactiveProperty<bool> KeepConnected => throw new NotSupportedException();
        public ReadOnlyReactiveProperty<HubConnectionState> ConnectionState => throw new NotSupportedException();
        public Observable<Unit> OnAuthorizationRejected => throw new NotSupportedException();
        public ILogger Logger => throw new NotSupportedException();
        public IMcpManager McpManager => throw new NotSupportedException();
        public IMcpManagerHub? McpManagerHub => throw new NotSupportedException();
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
}
