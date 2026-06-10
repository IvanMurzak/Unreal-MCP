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
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
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
