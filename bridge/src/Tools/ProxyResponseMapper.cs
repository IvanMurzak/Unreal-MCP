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
using com.IvanMurzak.McpPlugin.Common;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// Maps an IPC <c>tool-response</c> (docs/ARCHITECTURE.md §1.3) onto a <see cref="ResponseCallTool"/>
    /// with no re-shaping: <c>status</c>, <c>content</c> (the MCP content-block array), and
    /// <c>structured</c> carry over 1:1 because the plugin shapes them to mirror <c>ResponseCallTool</c>
    /// (§1.3). This is the "ProxyTool mapping" xUnit target (§9.3).
    /// </summary>
    public static class ProxyResponseMapper
    {
        public static ResponseCallTool Map(ToolResponseMessage message, string requestId)
        {
            var status = string.Equals(message.Status, IpcProtocol.Status.Error, System.StringComparison.OrdinalIgnoreCase)
                ? ResponseStatus.Error
                : ResponseStatus.Success;

            var content = new List<ContentBlock>();
            if (message.Content != null)
            {
                foreach (var node in message.Content)
                {
                    if (node is not System.Text.Json.Nodes.JsonObject block)
                        continue;

                    content.Add(new ContentBlock
                    {
                        Type = block["type"]?.GetValue<string>() ?? Consts.ContentType.Text,
                        Text = block["text"]?.GetValue<string>(),
                        Data = block["data"]?.GetValue<string>(),
                        MimeType = block["mimeType"]?.GetValue<string>(),
                    });
                }
            }

            return new ResponseCallTool
            {
                RequestID = requestId,
                Status = status,
                Content = content,
                StructuredContent = message.Structured,
            };
        }
    }
}
