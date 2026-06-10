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
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The channel a <see cref="ProxyTool"/> uses to round-trip a tool invocation over IPC: serialize a
    /// <c>tool-call</c>, await the matching <c>tool-response</c>, and forward cancellation as a
    /// <c>tool-cancel</c> (docs/ARCHITECTURE.md §2.2 step 2, §4). Implemented by the IPC client; an
    /// interface so the proxy/registration path is unit-testable with a fake channel.
    /// </summary>
    public interface IToolCallChannel
    {
        /// <summary>
        /// Send a <c>tool-call</c> and await its terminal <c>tool-response</c>.
        /// </summary>
        /// <exception cref="IpcDisconnectedException">
        /// The IPC link is down — the proxy must fail fast with the structured "bridge disconnected"
        /// error (§2.2 step 4), never hang to <paramref name="timeoutMs"/>.
        /// </exception>
        Task<ToolResponseMessage> CallToolAsync(
            string tool,
            JsonObject? arguments,
            int timeoutMs,
            CancellationToken cancellationToken);
    }

    /// <summary>
    /// Raised by <see cref="IToolCallChannel.CallToolAsync"/> when the IPC link is not connected, or
    /// when an in-flight call is failed because the link dropped mid-call (§1.5, §2.2 step 4).
    /// </summary>
    public sealed class IpcDisconnectedException : Exception
    {
        public const string DefaultMessage = "Unreal editor bridge disconnected.";
        public IpcDisconnectedException(string? message = null) : base(message ?? DefaultMessage) { }
    }
}
