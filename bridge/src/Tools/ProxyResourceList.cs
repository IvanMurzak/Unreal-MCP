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
        private readonly string _uri;
        private readonly string _name;
        private readonly bool _enabled;
        private readonly string? _mimeType;
        private readonly string? _description;

        public ProxyResourceList(string uri, string name, bool enabled, string? mimeType, string? description)
        {
            _uri = uri ?? throw new ArgumentNullException(nameof(uri));
            _name = name ?? string.Empty;
            _enabled = enabled;
            _mimeType = mimeType;
            _description = description;
        }

        public Task<ResponseListResource[]> Run(params object?[] parameters) => ListAsync();
        public Task<ResponseListResource[]> Run(IDictionary<string, object?>? namedParameters) => ListAsync();

        private Task<ResponseListResource[]> ListAsync()
        {
            // Static MVP: one entry, the resource's own descriptor. No IPC — the URI is fixed and known.
            var entry = new ResponseListResource(
                uri: _uri,
                name: _name,
                enabled: _enabled,
                mimeType: _mimeType,
                description: _description);
            return Task.FromResult(new[] { entry });
        }
    }
}
