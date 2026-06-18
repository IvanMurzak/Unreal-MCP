/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The channel a <see cref="ProxyPrompt"/> uses to round-trip a prompt-get over IPC: serialize a
    /// <c>prompt-get</c>, await the matching <c>prompt-response</c>, and forward cancellation as the generic
    /// <c>tool-cancel</c> (docs/ARCHITECTURE.md §A.1). The prompt sibling of <see cref="IToolCallChannel"/>;
    /// implemented by the IPC client (which already exposes <c>GetPromptAsync</c> with this exact signature),
    /// kept as an interface so the proxy/registration path is unit-testable with a fake channel.
    /// </summary>
    public interface IPromptCallChannel
    {
        /// <summary>
        /// Send a <c>prompt-get</c> and await its terminal <c>prompt-response</c>.
        /// </summary>
        /// <exception cref="IpcDisconnectedException">
        /// The IPC link is down — the proxy must fail fast with the structured "bridge disconnected"
        /// error (§A.1 / §2.2 step 4), never hang to <paramref name="timeoutMs"/>.
        /// </exception>
        Task<PromptResponseMessage> GetPromptAsync(
            string prompt,
            JsonObject? arguments,
            int timeoutMs,
            CancellationToken ct);
    }
}
