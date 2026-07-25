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
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The <see cref="IProxyToolSink"/> the <see cref="ManifestRegistrar"/> writes through, which fans each
    /// proxied tool onto the surface the plugin declared for it (docs/ARCHITECTURE.md §2.4): a
    /// <see cref="McpToolType.Standard"/> proxy goes to the MCP tool manager (<c>tools/list</c> +
    /// <c>/api/tools/&lt;name&gt;</c>), a <see cref="McpToolType.System"/> proxy to the system-tool manager
    /// (<c>/api/system-tools/&lt;name&gt;</c> only).
    ///
    /// <para>
    /// Keeping the routing in a SINK rather than in the registrar is what leaves the §2.2 diff machinery
    /// (revision guard, reconnect reset, remove → changed → add → toggle) completely untouched: the registrar
    /// still sees one flat tool set keyed by name, exactly as before. The only thing this class must remember
    /// is WHICH surface each registered name landed on, because <see cref="RemoveTool"/> and
    /// <see cref="SetToolEnabled"/> are given a bare name with no descriptor. A tool that CHANGES surface
    /// arrives as a §2.2 "changed" entry (ToolType is part of the descriptor's structural signature), i.e.
    /// remove-then-add — and the remove consults the recorded surface, so the old proxy is dropped from the
    /// manager it actually lived on.
    /// </para>
    /// <para>
    /// A name that was never routed (a defensive path — the registrar never removes what it did not add) falls
    /// back to trying BOTH surfaces, so a bookkeeping gap can only ever leak a stale proxy, never leave one
    /// stranded on the wrong manager.
    /// </para>
    /// <para><b>Threading:</b> not thread-safe — the host serializes tool-set mutations, as for every sink here.</para>
    /// </summary>
    public sealed class SurfaceRoutingToolSink : IProxyToolSink
    {
        private readonly IProxyToolSink _standard;
        private readonly IProxyToolSink _system;
        private readonly ILogger? _logger;

        /// <summary>Which surface each currently-registered tool name was routed to.</summary>
        private readonly Dictionary<string, McpToolType> _routed = new();

        public SurfaceRoutingToolSink(IProxyToolSink standard, IProxyToolSink system, ILogger? logger = null)
        {
            _standard = standard ?? throw new ArgumentNullException(nameof(standard));
            _system = system ?? throw new ArgumentNullException(nameof(system));
            _logger = logger;
        }

        /// <summary>The surface <paramref name="name"/> is currently registered on, or null when unregistered.</summary>
        public McpToolType? SurfaceOf(string name)
            => _routed.TryGetValue(name, out var surface) ? surface : null;

        public bool HasTool(string name) => _standard.HasTool(name) || _system.HasTool(name);

        public bool AddTool(string name, ProxyTool tool)
        {
            if (tool == null)
                return false;

            var surface = tool.ToolType;
            var added = surface == McpToolType.System
                ? _system.AddTool(name, tool)
                : _standard.AddTool(name, tool);

            if (added)
            {
                _routed[name] = surface;
                _logger?.LogDebug("Routed tool '{Name}' to the {Surface} surface.", name, surface);
            }
            return added;
        }

        public bool RemoveTool(string name)
        {
            if (_routed.TryGetValue(name, out var surface))
            {
                _routed.Remove(name);
                return surface == McpToolType.System ? _system.RemoveTool(name) : _standard.RemoveTool(name);
            }

            // Defensive: never routed by us (or bookkeeping lost). Drop it from both so nothing survives on the
            // wrong manager; `|` (not `||`) so the second removal is not short-circuited away.
            return _standard.RemoveTool(name) | _system.RemoveTool(name);
        }

        public bool SetToolEnabled(string name, bool enabled)
        {
            if (_routed.TryGetValue(name, out var surface))
            {
                return surface == McpToolType.System
                    ? _system.SetToolEnabled(name, enabled)
                    : _standard.SetToolEnabled(name, enabled);
            }
            return _standard.SetToolEnabled(name, enabled) | _system.SetToolEnabled(name, enabled);
        }
    }
}
