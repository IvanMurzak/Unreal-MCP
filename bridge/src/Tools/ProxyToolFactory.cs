/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Nodes;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// Builds a <see cref="ProxyTool"/> from a manifest <see cref="ToolDescriptor"/> (docs/ARCHITECTURE.md
    /// §2.2 step 2). The proxy's body serializes the call onto the supplied <see cref="IToolCallChannel"/>,
    /// awaits the terminal <c>tool-response</c>, and maps it to a <see cref="ResponseCallTool"/> with zero
    /// re-shaping (§1.3 — the IPC <c>status/content/structured</c> mirror <c>ResponseCallTool</c>). On an
    /// IPC disconnect the call fails fast with the structured "bridge disconnected" error (§2.2 step 4).
    /// </summary>
    public static class ProxyToolFactory
    {
        public static ProxyTool Create(ToolDescriptor descriptor, IToolCallChannel channel)
        {
            var toolName = descriptor.Name;
            var timeoutMs = IpcProtocol.DefaultToolTimeoutMs;

            return new ProxyTool(
                name: toolName,
                title: descriptor.Title,
                description: descriptor.Description,
                skillDescription: descriptor.SkillDescription,
                skillBody: descriptor.SkillBody,
                inputSchema: descriptor.InputSchema,
                outputSchema: descriptor.OutputSchema,
                readOnlyHint: descriptor.ReadOnlyHint,
                destructiveHint: descriptor.DestructiveHint,
                idempotentHint: descriptor.IdempotentHint,
                openWorldHint: descriptor.OpenWorldHint,
                handler: async (requestId, namedParameters, cancellationToken) =>
                {
                    var arguments = ToArguments(namedParameters);
                    try
                    {
                        var response = await channel
                            .CallToolAsync(toolName, arguments, timeoutMs, cancellationToken)
                            .ConfigureAwait(false);
                        return ProxyResponseMapper.Map(response, requestId);
                    }
                    catch (IpcDisconnectedException ex)
                    {
                        // Fail fast — never hang to timeoutMs (§2.2 step 4).
                        return ResponseCallTool.Error(ex.Message).SetRequestID(requestId);
                    }
                },
                // §2.4: carry the plugin-declared surface onto the proxy so SurfaceRoutingToolSink can put it on
                // the system-tool manager instead of the MCP tool manager. A descriptor with no `toolType` (an
                // older plugin) reads as Standard, preserving the pre-§2.4 behaviour exactly.
                toolType: descriptor.IsSystem ? McpToolType.System : McpToolType.Standard)
            { Enabled = descriptor.Enabled };
        }

        /// <summary>
        /// Convert the framework's raw named arguments (<c>IReadOnlyDictionary&lt;string, JsonElement&gt;</c>)
        /// into the <see cref="JsonObject"/> carried in the <c>tool-call</c> message. A null/empty set
        /// becomes an empty object so the plugin always sees a well-formed <c>arguments</c> field.
        /// </summary>
        public static JsonObject ToArguments(IReadOnlyDictionary<string, JsonElement>? namedParameters)
        {
            var obj = new JsonObject();
            if (namedParameters == null)
                return obj;

            foreach (var kvp in namedParameters)
                obj[kvp.Key] = JsonNode.Parse(kvp.Value.GetRawText());

            return obj;
        }
    }
}
