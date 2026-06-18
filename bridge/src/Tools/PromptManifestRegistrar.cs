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
    /// The minimal prompt-set mutation surface the registrar needs (the prompt sibling of
    /// <see cref="IProxyToolSink"/>). Abstracts <see cref="IPromptManager"/> (adapted by
    /// <see cref="PromptManagerSink"/>) so the manifest-application logic is unit-tested with a fake sink.
    /// </summary>
    public interface IProxyPromptSink
    {
        bool HasPrompt(string name);
        bool AddPrompt(string name, ProxyPrompt prompt);
        bool RemovePrompt(string name);
        bool SetPromptEnabled(string name, bool enabled);
    }

    /// <summary>Adapts the reused <see cref="IPromptManager"/> to <see cref="IProxyPromptSink"/>.</summary>
    public sealed class PromptManagerSink : IProxyPromptSink
    {
        private readonly IPromptManager _promptManager;
        public PromptManagerSink(IPromptManager promptManager) => _promptManager = promptManager;

        public bool HasPrompt(string name) => _promptManager.HasPrompt(name);

        // IPromptManager.AddPrompt takes ONLY the runner (its name == prompt.Name); the name param is kept on
        // the sink interface for symmetry/testability with IProxyToolSink and is identical to prompt.Name.
        public bool AddPrompt(string name, ProxyPrompt prompt) => _promptManager.AddPrompt(prompt);

        public bool RemovePrompt(string name) => _promptManager.RemovePrompt(name);
        public bool SetPromptEnabled(string name, bool enabled) => _promptManager.SetPromptEnabled(name, enabled);
    }

    /// <summary>
    /// The prompt-specific differ (docs/ARCHITECTURE.md §A.1) — the prompt sibling of
    /// <see cref="ManifestDiffer"/>: compare by <c>name</c> + <c>schemaHash</c> (the hash excludes the enabled
    /// flag), with a structural-signature fallback when a hash is absent on either side.
    /// </summary>
    internal static class PromptManifestDiffer
    {
        public static ManifestDiff<PromptDescriptor> Compute(
            IReadOnlyDictionary<string, PromptDescriptor> previous,
            IReadOnlyList<PromptDescriptor> next)
        {
            var diff = new ManifestDiff<PromptDescriptor>();
            var nextByName = new Dictionary<string, PromptDescriptor>();

            foreach (var desc in next)
            {
                if (string.IsNullOrEmpty(desc.Name))
                    continue;
                nextByName[desc.Name] = desc;

                if (!previous.TryGetValue(desc.Name, out var prev))
                {
                    diff.Added.Add(desc);
                    continue;
                }

                if (!SchemaEquals(prev, desc))
                    diff.Changed.Add(desc);
                else if (prev.Enabled != desc.Enabled)
                    diff.EnabledChanged.Add((desc.Name, desc.Enabled));
                // else: identical → no-op.
            }

            foreach (var name in previous.Keys)
            {
                if (!nextByName.ContainsKey(name))
                    diff.Removed.Add(name);
            }

            return diff;
        }

        private static bool SchemaEquals(PromptDescriptor a, PromptDescriptor b)
        {
            if (!string.IsNullOrEmpty(a.SchemaHash) || !string.IsNullOrEmpty(b.SchemaHash))
                return a.SchemaHash == b.SchemaHash;

            return Signature(a) == Signature(b);
        }

        private static string Signature(PromptDescriptor d)
        {
            // Structural fallback: everything that defines the prompt's surface EXCEPT the enabled flag.
            var input = d.InputSchema?.ToJsonString() ?? "null";
            return string.Join("", d.Name, d.Title, d.Description, d.Role, input, d.ExtensionId);
        }
    }

    /// <summary>
    /// Applies prompt manifests (docs/ARCHITECTURE.md §A.1) to an <see cref="IProxyPromptSink"/> — the prompt
    /// sibling of <see cref="ManifestRegistrar"/>. Derives from the shared, kind-agnostic
    /// <see cref="ManifestRegistrarBase{TDescriptor}"/> (the revision guard / reconnect reset / diff-application
    /// loop) and supplies the prompt-specific bits: the <see cref="ProxyPrompt"/> sink + the
    /// <see cref="PromptManifestDiffer"/>. Implements <see cref="IManifestSink{PromptManifestMessage}"/> so the
    /// IPC client routes a <c>prompt-manifest</c> to it without knowing the descriptor type. NOT thread-safe by
    /// itself: the IPC client invokes Apply from its single reader loop (same contract as the tool registrar).
    /// </summary>
    public sealed class PromptManifestRegistrar : ManifestRegistrarBase<PromptDescriptor>, IManifestSink<PromptManifestMessage>
    {
        private readonly IProxyPromptSink _sink;
        private readonly IPromptCallChannel _channel;

        public PromptManifestRegistrar(IProxyPromptSink sink, IPromptCallChannel channel, ILogger? logger = null)
            : base(logger)
        {
            _sink = sink;
            _channel = channel;
        }

        /// <summary>Names of the prompts currently registered by this registrar (test/inspection aid).</summary>
        public IReadOnlyCollection<string> AppliedPromptNames => Applied.Keys;

        /// <summary>A snapshot of the currently-applied prompt descriptors (copied list; caller mutation-safe).</summary>
        public IReadOnlyList<PromptDescriptor> AppliedDescriptors => new List<PromptDescriptor>(Applied.Values);

        /// <summary>Apply a prompt manifest. Returns the diff that was applied (empty when ignored/no-op).</summary>
        public ManifestDiff<PromptDescriptor> Apply(PromptManifestMessage manifest)
            => ApplyEntries(manifest.Revision, manifest.Prompts);

        // --- IManifestSink<PromptManifestMessage> (the type-erased route the IPC client holds) ----------
        public void ApplyManifest(PromptManifestMessage manifest) => ApplyEntries(manifest.Revision, manifest.Prompts);
        // ResetForReconnect is provided by the base (public).

        protected override string KindLabel => "prompt";

        protected override string KeyOf(PromptDescriptor descriptor) => descriptor.Name;

        protected override ManifestDiff<PromptDescriptor> ComputeDiff(IReadOnlyList<PromptDescriptor> next)
            => PromptManifestDiffer.Compute(Applied, next);

        protected override void SinkRemove(string key) => _sink.RemovePrompt(key);

        protected override void SinkAdd(PromptDescriptor descriptor) =>
            _sink.AddPrompt(descriptor.Name, ProxyPromptFactory.Create(descriptor, _channel));

        protected override void SinkSetEnabled(string key, bool enabled) => _sink.SetPromptEnabled(key, enabled);

        protected override PromptDescriptor WithEnabled(PromptDescriptor descriptor, bool enabled)
        {
            // Mutate in place + return the same instance (preserves the tool-registrar behaviour exactly).
            descriptor.Enabled = enabled;
            return descriptor;
        }
    }
}
