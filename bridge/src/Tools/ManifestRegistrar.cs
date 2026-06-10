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
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The minimal tool-set mutation surface the registrar needs. Abstracts <see cref="IToolManager"/>
    /// (adapted by <see cref="ToolManagerSink"/>) so the manifest-application logic is unit-tested with
    /// a fake sink — no built McpPlugin required.
    /// </summary>
    public interface IProxyToolSink
    {
        bool HasTool(string name);
        bool AddTool(string name, ProxyTool tool);
        bool RemoveTool(string name);
        bool SetToolEnabled(string name, bool enabled);
    }

    /// <summary>Adapts the reused <see cref="IToolManager"/> to <see cref="IProxyToolSink"/>.</summary>
    public sealed class ToolManagerSink : IProxyToolSink
    {
        private readonly IToolManager _toolManager;
        public ToolManagerSink(IToolManager toolManager) => _toolManager = toolManager;

        public bool HasTool(string name) => _toolManager.HasTool(name);
        public bool AddTool(string name, ProxyTool tool) => _toolManager.AddTool(name, tool);
        public bool RemoveTool(string name) => _toolManager.RemoveTool(name);
        public bool SetToolEnabled(string name, bool enabled) => _toolManager.SetToolEnabled(name, enabled);
    }

    /// <summary>
    /// Applies tool manifests (docs/ARCHITECTURE.md §2.2) to an <see cref="IProxyToolSink"/>: diff against
    /// the previously applied set, then remove / add (as <see cref="ProxyTool"/>s) / re-add changed /
    /// toggle enabled. Honours the out-of-order revision guard (§2.2 step 3) — but
    /// <see cref="ResetForReconnect"/> (called on every <c>handshake-ack</c>, §1.5) resets the
    /// last-applied revision to −1 so a post-reconnect re-push with an unchanged revision is always
    /// applied. NOT thread-safe by itself: ProxyTool's docs require the host to serialize tool-set
    /// mutations; the IPC client invokes this from its single reader loop.
    /// </summary>
    public sealed class ManifestRegistrar
    {
        private readonly IProxyToolSink _sink;
        private readonly IToolCallChannel _channel;
        private readonly ILogger? _logger;

        private readonly Dictionary<string, ToolDescriptor> _applied = new();
        private int _lastAppliedRevision = -1;

        public ManifestRegistrar(IProxyToolSink sink, IToolCallChannel channel, ILogger? logger = null)
        {
            _sink = sink;
            _channel = channel;
            _logger = logger;
        }

        /// <summary>Names of the tools currently registered by this registrar (test/inspection aid).</summary>
        public IReadOnlyCollection<string> AppliedToolNames => _applied.Keys;

        public int LastAppliedRevision => _lastAppliedRevision;

        /// <summary>
        /// Reset the revision guard after a (re)connection handshake (§1.5). The applied snapshot is
        /// retained — the long-lived McpPlugin still has the proxies registered, so a re-pushed manifest
        /// that is byte-identical diffs to a no-op (correct), while a genuine change while the link was
        /// down is still caught by the diff.
        /// </summary>
        public void ResetForReconnect() => _lastAppliedRevision = -1;

        /// <summary>Apply a manifest. Returns the diff that was applied (empty when ignored/no-op).</summary>
        public ManifestDiff Apply(ToolManifestMessage manifest)
        {
            if (manifest.Revision <= _lastAppliedRevision)
            {
                _logger?.LogDebug(
                    "Ignoring tool-manifest revision {Revision} (<= last applied {LastApplied}).",
                    manifest.Revision, _lastAppliedRevision);
                return new ManifestDiff();
            }

            var diff = ManifestDiffer.Compute(_applied, manifest.Tools);

            foreach (var name in diff.Removed)
            {
                _sink.RemoveTool(name);
                _applied.Remove(name);
            }

            foreach (var desc in diff.Changed)
            {
                _sink.RemoveTool(desc.Name);
                _sink.AddTool(desc.Name, ProxyToolFactory.Create(desc, _channel));
                _applied[desc.Name] = desc;
            }

            foreach (var desc in diff.Added)
            {
                _sink.AddTool(desc.Name, ProxyToolFactory.Create(desc, _channel));
                _applied[desc.Name] = desc;
            }

            foreach (var (name, enabled) in diff.EnabledChanged)
            {
                _sink.SetToolEnabled(name, enabled);
                if (_applied.TryGetValue(name, out var existing))
                    existing.Enabled = enabled;
            }

            _lastAppliedRevision = manifest.Revision;

            if (!diff.IsEmpty)
            {
                _logger?.LogInformation(
                    "Applied tool-manifest revision {Revision}: +{Added} ~{Changed} -{Removed} ⏼{Enabled} (total {Total}).",
                    manifest.Revision, diff.Added.Count, diff.Changed.Count, diff.Removed.Count,
                    diff.EnabledChanged.Count, _applied.Count);
            }

            return diff;
        }
    }
}
