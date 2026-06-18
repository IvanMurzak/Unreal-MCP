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
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common.Model;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The <see cref="IRunResourceList"/> half of a <see cref="ProxyResource"/> (docs/ARCHITECTURE.md §A.1).
    /// MVP scope is STATIC fixed-URI resources, so the list is synthesized DIRECTLY from the manifest
    /// descriptor — a single-element <see cref="ResponseListResource"/>[] — with NO IPC round-trip (a static
    /// resource has exactly one concrete URI; only templated URIs, deferred per §A.1, would need a live
    /// enumeration). The resource sibling shape of <see cref="ProxyPrompt"/>'s no-IPC metadata.
    /// </summary>
    public sealed class ProxyResourceList : IRunResourceList
    {
        private readonly ResourceDescriptorSnapshot _snapshot;

        public ProxyResourceList(string uri, string name, bool enabled, string? mimeType, string? description)
            => _snapshot = new ResourceDescriptorSnapshot(uri, name, enabled, mimeType, description);

        public Task<ResponseListResource[]> Run(params object?[] parameters) => ListAsync();
        public Task<ResponseListResource[]> Run(IDictionary<string, object?>? namedParameters) => ListAsync();

        private Task<ResponseListResource[]> ListAsync()
        {
            // Static MVP: one entry, the resource's own descriptor. No IPC — the URI is fixed and known.
            var entry = new ResponseListResource(
                uri: _snapshot.Uri,
                name: _snapshot.Name,
                enabled: _snapshot.Enabled,
                mimeType: _snapshot.MimeType,
                description: _snapshot.Description);
            return Task.FromResult(new[] { entry });
        }

        private readonly struct ResourceDescriptorSnapshot
        {
            public readonly string Uri;
            public readonly string Name;
            public readonly bool Enabled;
            public readonly string? MimeType;
            public readonly string? Description;

            public ResourceDescriptorSnapshot(string uri, string name, bool enabled, string? mimeType, string? description)
            {
                Uri = uri ?? throw new ArgumentNullException(nameof(uri));
                Name = name ?? string.Empty;
                Enabled = enabled;
                MimeType = mimeType;
                Description = description;
            }
        }
    }
}
