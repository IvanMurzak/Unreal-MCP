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
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common.Model;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// An <see cref="IRunPrompt"/> whose schema is supplied externally at runtime (the plugin's prompt
    /// manifest, §A.1) and whose body is an async delegate that round-trips a <c>prompt-get</c> over IPC and
    /// awaits the matching <c>prompt-response</c>. The prompt sibling of <see cref="ProxyTool"/> — the
    /// building block for the sidecar's dynamic, manifest-driven prompt set.
    /// </summary>
    public sealed class ProxyPrompt : IRunPrompt
    {
        private readonly Func<string, IReadOnlyDictionary<string, JsonElement>?, CancellationToken, Task<ResponseGetPrompt>> _handler;

        public string Name { get; }
        public string? Title { get; }
        public string? Description { get; }
        public Role Role { get; }
        public JsonNode? InputSchema { get; }

        /// <summary>
        /// Whether this prompt is exposed to MCP clients. Settable so the manifest registrar can toggle a
        /// runtime prompt on/off (mirrors <see cref="ProxyTool.Enabled"/>; <see cref="IRunPrompt"/> itself
        /// carries no enabled flag, so the toggle rides on the proxy + <c>IPromptManager.SetPromptEnabled</c>).
        /// </summary>
        public bool Enabled { get; set; } = true;

        public ProxyPrompt(
            string name,
            string? title,
            string? description,
            Role role,
            JsonNode? inputSchema,
            Func<string, IReadOnlyDictionary<string, JsonElement>?, CancellationToken, Task<ResponseGetPrompt>> handler)
        {
            Name = name ?? throw new ArgumentNullException(nameof(name));
            Title = title;
            Description = description;
            Role = role;
            // Detach the externally supplied schema via a round-trip clone so the proxy owns an immutable,
            // self-consistent copy (a JsonNode may only have one parent; mirrors ProxyTool).
            InputSchema = inputSchema is null ? null : JsonNode.Parse(inputSchema.ToJsonString());
            _handler = handler ?? throw new ArgumentNullException(nameof(handler));
        }

        public Task<ResponseGetPrompt> Run(string requestId, IReadOnlyDictionary<string, JsonElement>? namedParameters, CancellationToken cancellationToken = default)
            => _handler(requestId, namedParameters, cancellationToken);
    }
}
