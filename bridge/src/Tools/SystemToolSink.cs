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
using com.IvanMurzak.McpPlugin;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The <see cref="IProxyToolSink"/> for the SYSTEM surface (docs/ARCHITECTURE.md §2.4) — the sibling of
    /// <see cref="ToolManagerSink"/>, which serves the standard surface via <see cref="IToolManager"/>.
    ///
    /// <para>
    /// <b>Why it writes a dictionary instead of calling a manager API.</b> McpPlugin's
    /// <c>ISystemToolManager</c> is deliberately READ-ONLY (<c>TotalToolsCount</c> / <c>GetAllTools</c> /
    /// <c>HasTool</c> + the two run methods) because for the C# engines the system tool set is fixed at build
    /// time: <c>McpPluginBuilder.Build</c> partitions the declared runners by <c>IRunTool.ToolType</c> into a
    /// <c>SystemToolRunnerCollection</c>, and that is that. The Unreal sidecar's tool set is the opposite — it
    /// is 100% dynamic, arriving over IPC in the plugin's manifest AFTER the plugin is built (§2.2) — so there
    /// is no build-time moment at which a proxied system tool could be declared.
    /// </para>
    /// <para>
    /// <c>SystemToolRunnerCollection</c> IS a <c>Dictionary&lt;string, IRunTool&gt;</c>, registered as a DI
    /// singleton, and <c>McpSystemToolManager</c> holds that same instance and reads it live on every
    /// <c>RunSystemTool</c> / <c>RunListSystemTool</c> call. Mutating it after Build is therefore the exact
    /// dynamic-registration path the manager already supports — this sink just names it. Taking
    /// <c>IDictionary&lt;string, IRunTool&gt;</c> (not the concrete collection) keeps the xUnit suite able to
    /// drive it with a plain dictionary.
    /// </para>
    /// <para>
    /// <b>Threading:</b> not thread-safe on its own — same contract as <see cref="ToolManagerSink"/>. The host
    /// serializes tool-set mutations (the IPC client applies manifests from its single reader loop).
    /// </para>
    /// </summary>
    public sealed class SystemToolSink : IProxyToolSink
    {
        private readonly IDictionary<string, IRunTool> _tools;

        public SystemToolSink(IDictionary<string, IRunTool> tools)
            => _tools = tools ?? throw new ArgumentNullException(nameof(tools));

        public bool HasTool(string name) => !string.IsNullOrEmpty(name) && _tools.ContainsKey(name);

        public bool AddTool(string name, ProxyTool tool)
        {
            if (string.IsNullOrEmpty(name) || tool == null)
                return false;

            // Last-write-wins, matching SystemToolRunnerCollection.Add's own indexer semantics (and the §2.2
            // remove-then-add path the registrar drives for a changed tool).
            _tools[name] = tool;
            return true;
        }

        public bool RemoveTool(string name)
            => !string.IsNullOrEmpty(name) && _tools.Remove(name);

        public bool SetToolEnabled(string name, bool enabled)
        {
            if (string.IsNullOrEmpty(name) || !_tools.TryGetValue(name, out var tool))
                return false;

            // IRunTool : IEnabled — the flag is settable on the concrete proxy. A system tool's Enabled flag is
            // reported by RunListSystemTool but is NOT a gate in McpSystemToolManager.RunSystemTool, so the
            // AUTHORITATIVE gate stays where it already is: the plugin excludes a disabled tool from the served
            // manifest entirely (§2.2), which removes the proxy here rather than merely flagging it.
            if (tool is ProxyTool proxy)
            {
                proxy.Enabled = enabled;
                return true;
            }
            return false;
        }
    }
}
