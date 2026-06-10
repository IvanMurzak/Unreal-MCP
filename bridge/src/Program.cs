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

namespace com.IvanMurzak.Unreal.MCP.Bridge
{
    /// <summary>
    /// Scaffold stub of the unreal-mcp-bridge sidecar. Parses the launch contract
    /// (docs/ARCHITECTURE.md §1.4) and exits cleanly. The real implementation —
    /// IpcClient dialing 127.0.0.1:&lt;ipc-port&gt;, token handshake, manifest-driven
    /// ProxyTool registration into the McpPlugin host, SignalR relay — lands with
    /// the sidecar-bridge task.
    /// </summary>
    public static class Program
    {
        public static int Main(string[] args)
        {
            var arguments = BridgeArguments.Parse(args);
            if (!arguments.IsValid)
            {
                Console.Error.WriteLine($"[unreal-mcp-bridge] {arguments.Error}");
                Console.Error.WriteLine($"Usage: unreal-mcp-bridge {BridgeArguments.IpcPortArg}=<port> {BridgeArguments.ParentPidArg}=<editor pid>");
                return 2;
            }

            // §1.4: the one-shot IPC auth token arrives as exactly one line on stdin
            // (never argv). It is a secret — only its PRESENCE may ever be logged.
            var token = Console.IsInputRedirected ? Console.In.ReadLine() : null;
            var tokenState = string.IsNullOrWhiteSpace(token) ? "absent" : "received";

            Console.WriteLine($"[unreal-mcp-bridge] scaffold stub v0.1.0 — ipc-port={arguments.IpcPort}, parent-pid={arguments.ParentPid}, token={tokenState}");
            Console.WriteLine("[unreal-mcp-bridge] TODO(sidecar-bridge task): dial the plugin's IPC listener, perform the token handshake, host McpPlugin, relay tool calls. Exiting cleanly.");
            return 0;
        }
    }
}
