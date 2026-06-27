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
    /// <see cref="ManifestRegistrarBase{TDescriptor}.ResetForReconnect"/> (called on every
    /// <c>handshake-ack</c>, §1.5) resets the last-applied revision to −1 so a post-reconnect re-push with
    /// an unchanged revision is always applied. NOT thread-safe by itself: ProxyTool's docs require the host
    /// to serialize tool-set mutations; the IPC client invokes this from its single reader loop.
    ///
    /// <para>
    /// The revision-guard / reconnect-reset / diff-application machinery lives in the shared, kind-agnostic
    /// <see cref="ManifestRegistrarBase{TDescriptor}"/> so the prompt (P1) and resource (P2) registrars
    /// reuse it. This tool registrar is the original behaviour expressed through that base (gated by the
    /// bridge xUnit suite); the tool-specific bits are the <see cref="ProxyTool"/> sink + the
    /// <see cref="ManifestDiffer"/> name/schema-hash diff.
    /// </para>
    /// </summary>
    public sealed class ManifestRegistrar : ManifestRegistrarBase<ToolDescriptor>
    {
        private readonly IProxyToolSink _sink;
        private readonly IToolCallChannel _channel;

        public ManifestRegistrar(IProxyToolSink sink, IToolCallChannel channel, ILogger? logger = null)
            : base(logger)
        {
            _sink = sink;
            _channel = channel;
        }

        /// <summary>Names of the tools currently registered by this registrar (test/inspection aid).</summary>
        public IReadOnlyCollection<string> AppliedToolNames => Applied.Keys;

        /// <summary>
        /// A snapshot of the currently-applied tool descriptors (§7 skill-file generation reads this). The
        /// descriptors carry the full per-tool metadata the C++ plugin pushed over the manifest (Name / Title /
        /// Description / hints / input+output schema) — exactly the source the SKILL.md generator needs, so the
        /// sidecar can author the docs without any extra C++→sidecar payload. A copied list (caller mutation-safe).
        /// </summary>
        public IReadOnlyList<ToolDescriptor> AppliedDescriptors => new List<ToolDescriptor>(Applied.Values);

        /// <summary>Apply a tool manifest. Returns the diff that was applied (empty when ignored/no-op).</summary>
        public ManifestDiff<ToolDescriptor> Apply(ToolManifestMessage manifest)
            => ApplyEntries(manifest.Revision, manifest.Tools);

        protected override string KindLabel => "tool";

        protected override ManifestDiff<ToolDescriptor> ComputeDiff(IReadOnlyList<ToolDescriptor> next)
            => ManifestDiffer.Compute(Applied, next);

        protected override void SinkRemove(string key) => _sink.RemoveTool(key);

        protected override void SinkAdd(ToolDescriptor descriptor) =>
            _sink.AddTool(descriptor.Name, ProxyToolFactory.Create(descriptor, _channel));

        protected override void SinkSetEnabled(string key, bool enabled) => _sink.SetToolEnabled(key, enabled);
    }
}
