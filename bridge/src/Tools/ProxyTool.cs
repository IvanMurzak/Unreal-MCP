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
    /// <b>TODO (upstream swap):</b> this is a deliberate bridge-local mirror of the
    /// <c>com.IvanMurzak.McpPlugin.ProxyTool</c> design (docs/ARCHITECTURE.md §2.3). <see cref="IRunTool"/> +
    /// <c>IToolManager.AddTool</c> are already public, so hosting it here needed no upstream API bump. The
    /// <c>com.IvanMurzak.McpPlugin</c> pin is currently 6.10.0 — read the live value from the bridge csproj;
    /// it drifts as upstream releases (it has gone 6.7.0 → 6.9.0 → 6.10.0) — and ships no equivalent
    /// <c>ProxyTool</c> yet, so the chars/4 token-count formula stays inlined below to keep this mirror
    /// self-contained rather than depending on an upstream <c>ToolTokenCount.Calculate</c> helper. If a
    /// future upstream ships an equivalent ProxyTool, the swap is release-pipeline owned: delete this file
    /// and use the upstream type — the public surface is identical. The behaviour is byte-identical either way.
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
        /// The served SURFACE (docs/ARCHITECTURE.md §2.4). <see cref="McpToolType.Standard"/> keeps the proxy on
        /// the MCP tool manager (<c>tools/list</c> + <c>/api/tools/&lt;name&gt;</c>);
        /// <see cref="McpToolType.System"/> moves it to the system-tool manager
        /// (<c>/api/system-tools/&lt;name&gt;</c> only). Two consumers read it: <c>McpPluginBuilder.Build</c>
        /// partitions BUILD-TIME runners by this value, and <c>SurfaceRoutingToolSink</c> routes each
        /// manifest-driven proxy to the matching manager at runtime.
        /// </summary>
        public McpToolType ToolType { get; }

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
            Func<string, IReadOnlyDictionary<string, JsonElement>?, CancellationToken, Task<ResponseCallTool>> handler,
            McpToolType toolType = McpToolType.Standard)
        {
            ToolType = toolType;
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
