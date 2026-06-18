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
    /// The minimal resource-set mutation surface the registrar needs (the resource sibling of
    /// <see cref="IProxyToolSink"/> / <see cref="IProxyPromptSink"/>). Abstracts <see cref="IResourceManager"/>
    /// (adapted by <see cref="ResourceManagerSink"/>) so the manifest-application logic is unit-tested with a
    /// fake sink. The <c>key</c> is the resource's URI (which the proxy uses as both Name and Route, so the
    /// manager's Name-keyed dictionary lines up with the registrar's uri-keyed diff).
    /// </summary>
    public interface IProxyResourceSink
    {
        bool HasResource(string key);
        bool AddResource(string key, ProxyResource resource);
        bool RemoveResource(string key);
        bool SetResourceEnabled(string key, bool enabled);
    }

    /// <summary>Adapts the reused <see cref="IResourceManager"/> to <see cref="IProxyResourceSink"/>.</summary>
    public sealed class ResourceManagerSink : IProxyResourceSink
    {
        private readonly IResourceManager _resourceManager;
        public ResourceManagerSink(IResourceManager resourceManager) => _resourceManager = resourceManager;

        // The manager keys by IRunResource.Name; ProxyResourceFactory sets Name == Route == uri, so the `key`
        // (the descriptor uri) the registrar passes is exactly the manager's key.
        public bool HasResource(string key) => _resourceManager.HasResource(key);

        // IResourceManager.AddResource takes ONLY the runner (its name == resource.Name == the uri key); the
        // key param is kept on the sink interface for symmetry/testability with the tool/prompt sinks.
        public bool AddResource(string key, ProxyResource resource) => _resourceManager.AddResource(resource);

        public bool RemoveResource(string key) => _resourceManager.RemoveResource(key);
        public bool SetResourceEnabled(string key, bool enabled) => _resourceManager.SetResourceEnabled(key, enabled);
    }

    /// <summary>
    /// The resource-specific differ (docs/ARCHITECTURE.md §A.1) — the resource sibling of
    /// <see cref="ManifestDiffer"/> / <see cref="PromptManifestDiffer"/>: compare by <c>uri</c> +
    /// <c>schemaHash</c> (the hash excludes the enabled flag), with a structural-signature fallback when a hash
    /// is absent on either side.
    /// </summary>
    internal static class ResourceManifestDiffer
    {
        public static ManifestDiff<ResourceDescriptor> Compute(
            IReadOnlyDictionary<string, ResourceDescriptor> previous,
            IReadOnlyList<ResourceDescriptor> next)
        {
            var diff = new ManifestDiff<ResourceDescriptor>();
            var nextByUri = new Dictionary<string, ResourceDescriptor>();

            foreach (var desc in next)
            {
                if (string.IsNullOrEmpty(desc.Uri))
                    continue;
                nextByUri[desc.Uri] = desc;

                if (!previous.TryGetValue(desc.Uri, out var prev))
                {
                    diff.Added.Add(desc);
                    continue;
                }

                if (!SchemaEquals(prev, desc))
                    diff.Changed.Add(desc);
                else if (prev.Enabled != desc.Enabled)
                    diff.EnabledChanged.Add((desc.Uri, desc.Enabled));
                // else: identical → no-op.
            }

            foreach (var uri in previous.Keys)
            {
                if (!nextByUri.ContainsKey(uri))
                    diff.Removed.Add(uri);
            }

            return diff;
        }

        private static bool SchemaEquals(ResourceDescriptor a, ResourceDescriptor b)
        {
            if (!string.IsNullOrEmpty(a.SchemaHash) || !string.IsNullOrEmpty(b.SchemaHash))
                return a.SchemaHash == b.SchemaHash;

            return Signature(a) == Signature(b);
        }

        private static string Signature(ResourceDescriptor d)
        {
            // Structural fallback: everything that defines the resource's surface EXCEPT the enabled flag.
            return string.Join("", d.Uri, d.Name, d.Description, d.MimeType, d.ExtensionId);
        }
    }

    /// <summary>
    /// Applies resource manifests (docs/ARCHITECTURE.md §A.1) to an <see cref="IProxyResourceSink"/> — the
    /// resource sibling of <see cref="ManifestRegistrar"/> / <see cref="PromptManifestRegistrar"/>. Derives from
    /// the shared, kind-agnostic <see cref="ManifestRegistrarBase{TDescriptor}"/> (the revision guard / reconnect
    /// reset / diff-application loop) and supplies the resource-specific bits: the <see cref="ProxyResource"/>
    /// sink + the <see cref="ResourceManifestDiffer"/>. Implements
    /// <see cref="IManifestSink{ResourceManifestMessage}"/> so the IPC client routes a <c>resource-manifest</c>
    /// to it without knowing the descriptor type. NOT thread-safe by itself: the IPC client invokes Apply from
    /// its single reader loop (same contract as the tool/prompt registrars).
    /// </summary>
    public sealed class ResourceManifestRegistrar : ManifestRegistrarBase<ResourceDescriptor>, IManifestSink<ResourceManifestMessage>
    {
        private readonly IProxyResourceSink _sink;
        private readonly IResourceCallChannel _channel;

        public ResourceManifestRegistrar(IProxyResourceSink sink, IResourceCallChannel channel, ILogger? logger = null)
            : base(logger)
        {
            _sink = sink;
            _channel = channel;
        }

        /// <summary>URIs of the resources currently registered by this registrar (test/inspection aid).</summary>
        public IReadOnlyCollection<string> AppliedResourceUris => Applied.Keys;

        /// <summary>A snapshot of the currently-applied resource descriptors (copied list; caller mutation-safe).</summary>
        public IReadOnlyList<ResourceDescriptor> AppliedDescriptors => new List<ResourceDescriptor>(Applied.Values);

        /// <summary>Apply a resource manifest. Returns the diff that was applied (empty when ignored/no-op).</summary>
        public ManifestDiff<ResourceDescriptor> Apply(ResourceManifestMessage manifest)
            => ApplyEntries(manifest.Revision, manifest.Resources);

        // --- IManifestSink<ResourceManifestMessage> (the type-erased route the IPC client holds) ----------
        public void ApplyManifest(ResourceManifestMessage manifest) => ApplyEntries(manifest.Revision, manifest.Resources);
        // ResetForReconnect is provided by the base (public).

        protected override string KindLabel => "resource";

        protected override string KeyOf(ResourceDescriptor descriptor) => descriptor.Uri;

        protected override ManifestDiff<ResourceDescriptor> ComputeDiff(IReadOnlyList<ResourceDescriptor> next)
            => ResourceManifestDiffer.Compute(Applied, next);

        protected override void SinkRemove(string key) => _sink.RemoveResource(key);

        protected override void SinkAdd(ResourceDescriptor descriptor) =>
            _sink.AddResource(descriptor.Uri, ProxyResourceFactory.Create(descriptor, _channel));

        protected override void SinkSetEnabled(string key, bool enabled) => _sink.SetResourceEnabled(key, enabled);

        protected override ResourceDescriptor WithEnabled(ResourceDescriptor descriptor, bool enabled)
        {
            // Mutate in place + return the same instance (preserves the tool/prompt-registrar behaviour exactly).
            descriptor.Enabled = enabled;
            return descriptor;
        }
    }
}
