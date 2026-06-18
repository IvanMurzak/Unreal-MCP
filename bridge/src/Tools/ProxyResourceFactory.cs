/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// Builds a <see cref="ProxyResource"/> from a manifest <see cref="ResourceDescriptor"/>
    /// (docs/ARCHITECTURE.md §A.1) — the resource sibling of <see cref="ProxyToolFactory"/> /
    /// <see cref="ProxyPromptFactory"/>. The resource's content runner round-trips a <c>resource-read</c> on the
    /// supplied <see cref="IResourceCallChannel"/>; its list runner is a static single-element synthesis from
    /// the descriptor (MVP fixed-URI scope, NO IPC — see <see cref="ProxyResourceList"/>).
    /// </summary>
    public static class ProxyResourceFactory
    {
        public static ProxyResource Create(ResourceDescriptor descriptor, IResourceCallChannel channel)
        {
            var uri = descriptor.Uri;
            // The McpResourceManager keys resources by IRunResource.Name (its dictionary key) AND looks content
            // up by Route (URI match). To make the URI the single identity — the dedup key the registrar diffs on
            // AND the route a resources/read resolves — set BOTH Name and Route to the uri. The descriptor's
            // human-friendly `name` is surfaced as the resources/list display name via ProxyResourceList instead.
            var displayName = string.IsNullOrEmpty(descriptor.Name) ? uri : descriptor.Name!;
            var timeoutMs = IpcProtocol.DefaultToolTimeoutMs;

            var content = new ProxyResourceContent(uri, channel, timeoutMs);
            var list = new ProxyResourceList(uri, displayName, descriptor.Enabled, descriptor.MimeType, descriptor.Description);

            return new ProxyResource(
                route: uri,
                name: uri, // manager key == route == uri (single identity)
                description: descriptor.Description,
                mimeType: descriptor.MimeType,
                runGetContent: content,
                runListContext: list)
            {
                Enabled = descriptor.Enabled,
            };
        }
    }
}
