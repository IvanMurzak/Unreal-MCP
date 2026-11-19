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
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.McpPlugin.Common.Hub.Client;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.ReflectorNet;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;
using McpVersion = com.IvanMurzak.McpPlugin.Common.Version;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// The server's <c>CancelCall</c> (the App aborted the HTTP call) reaches the editor: McpPlugin fires the call's
    /// <see cref="ToolCallCancellation.Token"/>, the manifest proxy hands it to the IPC round trip, and the plugin gets one
    /// <c>tool-cancel</c> for the <c>tool-call</c> it is running, so the game-thread dispatcher skips a call still queued
    /// (§4). Real McpPlugin hub, real <see cref="IpcClient"/>, a fake plugin on a loopback socket.
    /// </summary>
    public class ToolCallCancelTests
    {
        static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

        [Fact]
        public async Task CancelCall_SendsTheEditorOneToolCancel_ForTheRunningToolCall()
        {
            await using var link = await FakePluginLink.StartAsync();
            var hub = link.ToolHub;

            var call = hub.RunCallTool(new RequestCallTool("request-1", "slow", new Dictionary<string, JsonElement>()));
            var toolCall = await link.NextFrameAsync();
            Assert.Equal(IpcProtocol.Type.ToolCall, toolCall["type"]!.GetValue<string>());
            var ipcRequestId = toolCall["requestId"]!.GetValue<string>();

            ((IClientCallCancellation)hub).CancelCall(new RequestCancelCall("request-1"));

            var cancel = await link.NextFrameAsync();
            Assert.Equal(IpcProtocol.Type.ToolCancel, cancel["type"]!.GetValue<string>());
            Assert.Equal(ipcRequestId, cancel["requestId"]!.GetValue<string>());

            var answer = await call.WaitAsync(Wait);
            Assert.Equal(ResponseStatus.Error, answer.Status);

            // A repeated CancelCall for the finished call sends nothing more.
            ((IClientCallCancellation)hub).CancelCall(new RequestCancelCall("request-1"));
            Assert.Empty(await link.DrainFramesAsync());
        }

        [Fact]
        public async Task AnsweredCall_SendsNoToolCancel()
        {
            await using var link = await FakePluginLink.StartAsync();
            var hub = link.ToolHub;

            var call = hub.RunCallTool(new RequestCallTool("request-2", "slow", new Dictionary<string, JsonElement>()));
            var toolCall = await link.NextFrameAsync();
            await link.SendAsync(new JsonObject
            {
                ["type"] = IpcProtocol.Type.ToolResponse,
                ["requestId"] = toolCall["requestId"]!.GetValue<string>(),
                ["status"] = IpcProtocol.Status.Success,
                ["content"] = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = "done" }),
            });

            var answer = await call.WaitAsync(Wait);
            Assert.Equal(ResponseStatus.Success, answer.Status);

            ((IClientCallCancellation)hub).CancelCall(new RequestCancelCall("request-2"));
            Assert.Empty(await link.DrainFramesAsync());
        }

        /// <summary>
        /// A bridge <see cref="IpcClient"/> dialed into a fake editor plugin that acknowledged the handshake, plus a real
        /// McpPlugin whose tool manager holds one manifest proxy ("slow") routed over that link.
        /// </summary>
        sealed class FakePluginLink : IAsyncDisposable
        {
            readonly TcpListener _listener;
            readonly IpcClient _ipc;
            readonly CancellationTokenSource _cts = new();
            readonly Task _run;
            readonly Channel<JsonObject> _frames = Channel.CreateUnbounded<JsonObject>();
            TcpClient? _peer;
            StreamWriter? _writer;
            Task? _read;

            public IClientToolHub ToolHub { get; }

            FakePluginLink(TcpListener listener, IpcClient ipc, IClientToolHub toolHub)
            {
                _listener = listener;
                _ipc = ipc;
                ToolHub = toolHub;
                _run = ipc.RunAsync(_cts.Token);
            }

            public static async Task<FakePluginLink> StartAsync()
            {
                var listener = new TcpListener(IPAddress.Loopback, 0);
                listener.Start();
                var port = ((IPEndPoint)listener.LocalEndpoint).Port;
                var ipc = new IpcClient("127.0.0.1", port, token: "ipc-token", sidecarVersion: "0.1.0");
                var accepted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                ipc.HandshakeAccepted += _ => accepted.TrySetResult();

                var version = new McpVersion { Api = Consts.ApiVersion, Plugin = "0.1.0", Environment = "test" };
                var plugin = new McpPluginBuilder(version)
                    .SetConfig(new ConnectionConfig { GenerateSkillFiles = false, KeepConnected = false })
                    .Build(new Reflector());
                var manager = (McpManager)plugin.McpManager;
                new ManifestRegistrar(new ToolManagerSink(manager.ToolManager!), ipc)
                    .Apply(new ToolManifestMessage { Revision = 1, Tools = { new ToolDescriptor { Name = "slow" } } });

                var link = new FakePluginLink(listener, ipc, manager.ToolHub!);
                await link.AcceptAsync();
                await accepted.Task.WaitAsync(Wait);
                return link;
            }

            async Task AcceptAsync()
            {
                _peer = await _listener.AcceptTcpClientAsync().WaitAsync(Wait);
                var stream = _peer.GetStream();
                var reader = new StreamReader(stream, new UTF8Encoding(false));
                _writer = new StreamWriter(stream, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };

                var handshake = JsonNode.Parse((await reader.ReadLineAsync().WaitAsync(Wait))!)!;
                Assert.Equal(IpcProtocol.Type.Handshake, handshake["type"]!.GetValue<string>());
                await SendAsync(new JsonObject { ["type"] = IpcProtocol.Type.HandshakeAck, ["ipcVersion"] = IpcProtocol.IpcVersion });

                _read = Task.Run(async () =>
                {
                    try
                    {
                        while (await reader.ReadLineAsync(_cts.Token) is { } line)
                        {
                            if (JsonNode.Parse(line) is JsonObject frame && frame["type"]?.GetValue<string>() != IpcProtocol.Type.Ping)
                                _frames.Writer.TryWrite(frame);
                        }
                    }
                    catch (Exception ex) when (ex is OperationCanceledException or IOException or ObjectDisposedException)
                    {
                        // The link closed.
                    }
                });
            }

            public Task SendAsync(JsonObject frame) => _writer!.WriteLineAsync(frame.ToJsonString());

            /// <summary>The next frame the bridge sent (pings left out).</summary>
            public async Task<JsonObject> NextFrameAsync() => await _frames.Reader.ReadAsync().AsTask().WaitAsync(Wait);

            /// <summary>The frames the bridge sends within a short window.</summary>
            public async Task<List<JsonObject>> DrainFramesAsync()
            {
                await Task.Delay(500);
                var frames = new List<JsonObject>();
                while (_frames.Reader.TryRead(out var frame))
                    frames.Add(frame);
                return frames;
            }

            public async ValueTask DisposeAsync()
            {
                _cts.Cancel();
                _peer?.Dispose();
                _listener.Stop();
                await _ipc.DisposeAsync();
                await Task.WhenAny(_run, Task.Delay(Wait));
                if (_read != null)
                    await Task.WhenAny(_read, Task.Delay(Wait));
                _cts.Dispose();
            }
        }
    }
}
