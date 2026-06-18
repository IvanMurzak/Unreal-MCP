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
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// Builds a <see cref="ProxyPrompt"/> from a manifest <see cref="PromptDescriptor"/> (docs/ARCHITECTURE.md
    /// §A.1) — the prompt sibling of <see cref="ProxyToolFactory"/>. The proxy's body serializes the get onto
    /// the supplied <see cref="IPromptCallChannel"/>, awaits the terminal <c>prompt-response</c>, and maps it
    /// to a <see cref="ResponseGetPrompt"/>. On an IPC disconnect the call fails fast with the structured
    /// "bridge disconnected" error (§2.2 step 4).
    /// </summary>
    public static class ProxyPromptFactory
    {
        public static ProxyPrompt Create(PromptDescriptor descriptor, IPromptCallChannel channel)
        {
            var promptName = descriptor.Name;
            var timeoutMs = IpcProtocol.DefaultToolTimeoutMs;
            var role = ParseRole(descriptor.Role) ?? Role.User;

            return new ProxyPrompt(
                name: promptName,
                title: descriptor.Title,
                description: descriptor.Description,
                role: role,
                inputSchema: descriptor.InputSchema,
                handler: async (requestId, namedParameters, cancellationToken) =>
                {
                    // Reuse ProxyToolFactory.ToArguments (public static) — the prompt args are the same JSON shape.
                    var args = ProxyToolFactory.ToArguments(namedParameters);
                    try
                    {
                        var response = await channel
                            .GetPromptAsync(promptName, args, timeoutMs, cancellationToken)
                            .ConfigureAwait(false);
                        return MapResponse(response, requestId, role);
                    }
                    catch (IpcDisconnectedException ex)
                    {
                        // Fail fast — never hang to timeoutMs (§2.2 step 4).
                        return ResponseGetPrompt.Error(ex.Message).SetRequestID(requestId);
                    }
                }) { Enabled = descriptor.Enabled };
        }

        /// <summary>
        /// Map an IPC <c>prompt-response</c> onto a <see cref="ResponseGetPrompt"/>: an <c>error</c> status
        /// becomes <see cref="ResponseGetPrompt.Error(string)"/>; otherwise each entry becomes a
        /// <see cref="ResponsePromptMessage"/> (its role parsed, falling back to the descriptor's default role).
        /// </summary>
        private static ResponseGetPrompt MapResponse(PromptResponseMessage response, string requestId, Role defaultRole)
        {
            if (string.Equals(response.Status, IpcProtocol.Status.Error, StringComparison.OrdinalIgnoreCase))
                return ResponseGetPrompt.Error(response.Error ?? "Prompt failed.").SetRequestID(requestId);

            var messages = response.Messages?
                .Select(m => new ResponsePromptMessage(m.Text ?? string.Empty, ParseRole(m.Role) ?? defaultRole))
                .ToList() ?? new List<ResponsePromptMessage>();

            return ResponseGetPrompt.Success(messages, response.Description).SetRequestID(requestId);
        }

        /// <summary>Parse a wire role string ("user"/"assistant", case-insensitive) → <see cref="Role"/>; null when absent/unknown.</summary>
        private static Role? ParseRole(string? role)
        {
            if (string.IsNullOrWhiteSpace(role))
                return null;
            if (string.Equals(role, "assistant", StringComparison.OrdinalIgnoreCase))
                return Role.Assistant;
            if (string.Equals(role, "user", StringComparison.OrdinalIgnoreCase))
                return Role.User;
            return null;
        }
    }
}
