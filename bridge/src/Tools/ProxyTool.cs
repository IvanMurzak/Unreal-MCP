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
    /// An <see cref="IRunTool"/> whose JSON schemas are supplied externally at runtime (the plugin's
    /// tool manifest, §2.2) and whose body is an async delegate that round-trips a <c>tool-call</c>
    /// over IPC and awaits the matching <c>tool-response</c>. The building block for the sidecar's
    /// dynamic, manifest-driven tool set.
    ///
    /// <para>
    /// <b>TODO (upstream swap):</b> this is a local mirror of <c>MCP-Plugin-dotnet</c> PR #122's
    /// <c>com.IvanMurzak.McpPlugin.ProxyTool</c> (docs/ARCHITECTURE.md §2.3). The §2.3 additive API
    /// lands as <c>com.IvanMurzak.McpPlugin</c> &gt;= 6.8.0; when the pin is bumped (release-pipeline
    /// owned), delete this file and use the upstream type — the public surface is identical. The pin
    /// is FROZEN at 6.7.0 here, so the chars/4 token-count formula is inlined rather than reusing the
    /// 6.8.0-only <c>ToolTokenCount.Calculate</c> helper. The behaviour is byte-identical.
    /// </para>
    /// </summary>
    public sealed class ProxyTool : IRunTool
    {
        private readonly Func<string, IReadOnlyDictionary<string, JsonElement>?, CancellationToken, Task<ResponseCallTool>> _handler;
        private int? _cachedTokenCount;

        public string Name { get; }
        public string? Title { get; }
        public string? Description { get; }
        public string? SkillDescription { get; }
        public string? SkillBody { get; }
        public JsonNode? InputSchema { get; }
        public JsonNode? OutputSchema { get; }
        public bool? ReadOnlyHint { get; }
        public bool? DestructiveHint { get; }
        public bool? IdempotentHint { get; }
        public bool? OpenWorldHint { get; }

        /// <summary>
        /// Whether this tool is exposed to MCP clients. Settable so the manifest registrar can toggle a
        /// runtime tool on/off. For post-registration toggling prefer
        /// <c>IToolManager.SetToolEnabled(name, enabled)</c> (it also fires the list-changed notification).
        /// </summary>
        public bool Enabled { get; set; } = true;

        public int TokenCount
        {
            get
            {
                if (_cachedTokenCount.HasValue)
                    return _cachedTokenCount.Value;

                try
                {
                    _cachedTokenCount = CalculateTokenCount(Name, Title, Description, InputSchema, OutputSchema);
                }
                catch
                {
                    // Mirror RunTool.CalculateTokenCount's resilience: a tool whose externally supplied
                    // schema fails to serialize must not poison IToolManager.EnabledToolsTokenCount.
                    _cachedTokenCount = 0;
                }
                return _cachedTokenCount.Value;
            }
        }

        public ProxyTool(
            string name,
            string? title,
            string? description,
            string? skillDescription,
            string? skillBody,
            JsonNode? inputSchema,
            JsonNode? outputSchema,
            bool? readOnlyHint,
            bool? destructiveHint,
            bool? idempotentHint,
            bool? openWorldHint,
            Func<string, IReadOnlyDictionary<string, JsonElement>?, CancellationToken, Task<ResponseCallTool>> handler)
        {
            Name = name ?? throw new ArgumentNullException(nameof(name));
            Title = title;
            Description = description;
            SkillDescription = skillDescription;
            SkillBody = skillBody;
            // Detach the externally supplied schemas via a round-trip clone so the proxy owns immutable,
            // self-consistent copies (a JsonNode may only have one parent; this also lets one node back
            // more than one tool).
            InputSchema = inputSchema is null ? null : JsonNode.Parse(inputSchema.ToJsonString());
            OutputSchema = outputSchema is null ? null : JsonNode.Parse(outputSchema.ToJsonString());
            ReadOnlyHint = readOnlyHint;
            DestructiveHint = destructiveHint;
            IdempotentHint = idempotentHint;
            OpenWorldHint = openWorldHint;
            _handler = handler ?? throw new ArgumentNullException(nameof(handler));
        }

        public Task<ResponseCallTool> Run(string requestId, IReadOnlyDictionary<string, JsonElement>? namedParameters, CancellationToken cancellationToken = default)
            => _handler(requestId, namedParameters, cancellationToken);

        /// <summary>
        /// The chars/4 token approximation, inlined from <c>RunTool.CalculateTokenCount</c> / the 6.8.0
        /// <c>ToolTokenCount.Calculate</c> helper (identical formula and JSON shape — see §2.1, the
        /// <c>RunTool.TokenCount.cs</c> reference). Builds a JSON object of the non-empty fields +
        /// detached schema copies, serializes, and returns <c>ceil(len / 4)</c>.
        /// </summary>
        internal static int CalculateTokenCount(string? name, string? title, string? description, JsonNode? inputSchema, JsonNode? outputSchema)
        {
            var jsonObject = new JsonObject();
            if (!string.IsNullOrEmpty(name)) jsonObject["name"] = name;
            if (!string.IsNullOrEmpty(title)) jsonObject["title"] = title;
            if (!string.IsNullOrEmpty(description)) jsonObject["description"] = description;
            if (inputSchema != null) jsonObject["inputSchema"] = JsonNode.Parse(inputSchema.ToJsonString());
            if (outputSchema != null) jsonObject["outputSchema"] = JsonNode.Parse(outputSchema.ToJsonString());

            var jsonString = jsonObject.ToJsonString();
            return (int)Math.Ceiling(jsonString.Length / 4.0);
        }
    }
}
