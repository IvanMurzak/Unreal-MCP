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
using System.Text.Json.Serialization;
using com.IvanMurzak.McpPlugin;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// An <see cref="IRunResource"/> whose metadata is supplied externally at runtime (the plugin's resource
    /// manifest, §A.1) and whose content/list runners are IPC-backed proxies. The resource sibling of
    /// <see cref="ProxyTool"/> / <see cref="ProxyPrompt"/> — the building block for the sidecar's dynamic,
    /// manifest-driven resource set. MVP scope is STATIC fixed-URI resources (the <see cref="Route"/> is the
    /// concrete URI; templated URIs are deferred, §A.1).
    /// </summary>
    public sealed class ProxyResource : IRunResource
    {
        /// <summary>The resource's MCP URI — its route AND identity (the manager keys resources by route).</summary>
        public string Route { get; set; }
        public string Name { get; set; }
        public string? Description { get; set; }
        public string? MimeType { get; set; }

        /// <summary>
        /// Whether this resource is exposed to MCP clients. Settable so the manifest registrar can toggle a
        /// runtime resource on/off (mirrors <see cref="ProxyTool.Enabled"/> / <see cref="ProxyPrompt.Enabled"/>).
        /// </summary>
        public bool Enabled { get; set; } = true;

        [JsonIgnore]
        public IRunResourceContent RunGetContent { get; set; }

        [JsonIgnore]
        public IRunResourceList RunListContext { get; set; }

        public ProxyResource(
            string route,
            string name,
            string? description,
            string? mimeType,
            IRunResourceContent runGetContent,
            IRunResourceList runListContext)
        {
            Route = route ?? throw new ArgumentNullException(nameof(route));
            Name = name ?? string.Empty;
            Description = description;
            MimeType = mimeType;
            RunGetContent = runGetContent ?? throw new ArgumentNullException(nameof(runGetContent));
            RunListContext = runListContext ?? throw new ArgumentNullException(nameof(runListContext));
        }
    }
}
