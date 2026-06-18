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
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The <see cref="IRunResourceContent"/> half of a <see cref="ProxyResource"/> (docs/ARCHITECTURE.md §A.1)
    /// — its body round-trips a <c>resource-read</c> over IPC and maps the terminal <c>resource-response</c> to
    /// <see cref="ResponseResourceContent"/>[] (text XOR base64 blob). The resource sibling of the
    /// <see cref="ProxyPrompt"/> get-handler. On an IPC disconnect the read throws so the manager surfaces a
    /// fail-fast error rather than hanging to the timeout (§2.2 step 4).
    /// </summary>
    public sealed class ProxyResourceContent : IRunResourceContent
    {
        private readonly string _uri;
        private readonly IResourceCallChannel _channel;
        private readonly int _timeoutMs;

        public ProxyResourceContent(string uri, IResourceCallChannel channel, int timeoutMs)
        {
            _uri = uri ?? throw new ArgumentNullException(nameof(uri));
            _channel = channel ?? throw new ArgumentNullException(nameof(channel));
            _timeoutMs = timeoutMs;
        }

        // IRunResourceContent is schema-blind here: a static-URI MVP resource takes no parameters, so both Run
        // overloads ignore their args and read the fixed uri (templated URIs are deferred, §A.1).
        public Task<ResponseResourceContent[]> Run(params object?[] parameters) => ReadAsync(CancellationToken.None);
        public Task<ResponseResourceContent[]> Run(IDictionary<string, object?>? namedParameters) => ReadAsync(CancellationToken.None);

        private async Task<ResponseResourceContent[]> ReadAsync(CancellationToken cancellationToken)
        {
            // Round-trip the read; an IpcDisconnectedException propagates so the manager fails fast (never hangs).
            var response = await _channel.ReadResourceAsync(_uri, _timeoutMs, cancellationToken).ConfigureAwait(false);

            if (string.Equals(response.Status, IpcProtocol.Status.Error, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(response.Error ?? $"Resource '{_uri}' read failed.");

            return MapContents(response, _uri);
        }

        /// <summary>
        /// Map an IPC <c>resource-response</c>'s contents[] onto <see cref="ResponseResourceContent"/>[]: each
        /// entry becomes a text block (<see cref="ResponseResourceContent.CreateText"/>) or, when a base64
        /// <c>blob</c> is present, a blob block (<see cref="ResponseResourceContent.CreateBlob"/>). The blob
        /// branch is preferred when both are set (a binary resource), mirroring the plugin's Text-XOR-Blob shape.
        /// </summary>
        internal static ResponseResourceContent[] MapContents(ResourceResponseMessage response, string fallbackUri)
        {
            if (response.Contents == null || response.Contents.Count == 0)
                return Array.Empty<ResponseResourceContent>();

            return response.Contents.Select(c =>
            {
                var uri = string.IsNullOrEmpty(c.Uri) ? fallbackUri : c.Uri!;
                if (!string.IsNullOrEmpty(c.Blob))
                    return ResponseResourceContent.CreateBlob(uri, c.Blob!, c.MimeType);
                return ResponseResourceContent.CreateText(uri, c.Text ?? string.Empty, c.MimeType);
            }).ToArray();
        }
    }
}
