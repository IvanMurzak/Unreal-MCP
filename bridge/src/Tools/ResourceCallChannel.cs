/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The channel a <see cref="ProxyResourceContent"/> uses to round-trip a resource-read over IPC: serialize
    /// a <c>resource-read</c>, await the matching <c>resource-response</c>, and forward cancellation as the
    /// generic <c>tool-cancel</c> (docs/ARCHITECTURE.md §A.1). The resource sibling of
    /// <see cref="IPromptCallChannel"/>; implemented by the IPC client (which already exposes
    /// <c>ReadResourceAsync</c> with this exact signature), kept as an interface so the proxy/registration path
    /// is unit-testable with a fake channel.
    /// </summary>
    public interface IResourceCallChannel
    {
        /// <summary>
        /// Send a <c>resource-read</c> and await its terminal <c>resource-response</c>.
        /// </summary>
        /// <exception cref="IpcDisconnectedException">
        /// The IPC link is down — the proxy must fail fast with the structured "bridge disconnected"
        /// error (§A.1 / §2.2 step 4), never hang to <paramref name="timeoutMs"/>.
        /// </exception>
        Task<ResourceResponseMessage> ReadResourceAsync(
            string uri,
            int timeoutMs,
            CancellationToken ct);
    }
}
