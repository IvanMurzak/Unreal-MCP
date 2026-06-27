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
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// Pure manifest diffing (docs/ARCHITECTURE.md §2.2 / §A.1): compare a freshly-received manifest against
    /// the previously applied set by <c>key</c> + <c>schemaHash</c> (the hash excludes the enabled flag, §2.2),
    /// falling back to the descriptor's <see cref="IManifestDescriptor.StructuralSignature"/> when a hash is
    /// absent on either side. Generic over <typeparamref name="TDescriptor"/> through
    /// <see cref="IManifestDescriptor"/> so the SAME diff serves the tool, prompt (§A.1), and resource (§A.1)
    /// registrars — formerly three byte-identical per-kind differs. This is the "manifest diff" xUnit target (§9.3).
    /// </summary>
    public static class ManifestDiffer
    {
        public static ManifestDiff<TDescriptor> Compute<TDescriptor>(
            IReadOnlyDictionary<string, TDescriptor> previous,
            IReadOnlyList<TDescriptor> next)
            where TDescriptor : IManifestDescriptor
        {
            var diff = new ManifestDiff<TDescriptor>();
            var nextByKey = new Dictionary<string, TDescriptor>();

            foreach (var desc in next)
            {
                var key = desc.Key;
                if (string.IsNullOrEmpty(key))
                    continue;
                nextByKey[key] = desc;

                if (!previous.TryGetValue(key, out var prev))
                {
                    diff.Added.Add(desc);
                    continue;
                }

                if (!SchemaEquals(prev, desc))
                {
                    diff.Changed.Add(desc);
                }
                else if (prev.Enabled != desc.Enabled)
                {
                    diff.EnabledChanged.Add((key, desc.Enabled));
                }
                // else: identical → no-op.
            }

            foreach (var key in previous.Keys)
            {
                if (!nextByKey.ContainsKey(key))
                    diff.Removed.Add(key);
            }

            return diff;
        }

        /// <summary>
        /// Two descriptors are schema-equal when their <c>schemaHash</c> matches (the enabled flag is excluded
        /// from the hash by the plugin, §2.2). When a hash is absent on either side, fall back to comparing the
        /// structural signature so a manifest without hashes still diffs deterministically.
        /// </summary>
        private static bool SchemaEquals(IManifestDescriptor a, IManifestDescriptor b)
        {
            if (!string.IsNullOrEmpty(a.SchemaHash) || !string.IsNullOrEmpty(b.SchemaHash))
                return a.SchemaHash == b.SchemaHash;

            return a.StructuralSignature == b.StructuralSignature;
        }
    }
}
