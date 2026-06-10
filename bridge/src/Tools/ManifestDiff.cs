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
    /// The result of diffing a freshly-received tool manifest against the previously applied set
    /// (docs/ARCHITECTURE.md §2.2 step 1). Pure data; computed by <see cref="ManifestDiffer"/>.
    /// </summary>
    public sealed class ManifestDiff
    {
        /// <summary>Tools present in the new manifest but not the previous set → <c>AddTool</c>.</summary>
        public List<ToolDescriptor> Added { get; } = new();

        /// <summary>Tools whose schema hash changed → <c>RemoveTool</c> then <c>AddTool</c>.</summary>
        public List<ToolDescriptor> Changed { get; } = new();

        /// <summary>Tool names removed from the new manifest → <c>RemoveTool</c>.</summary>
        public List<string> Removed { get; } = new();

        /// <summary>Tools whose ONLY change is the enabled flag → <c>SetToolEnabled</c>.</summary>
        public List<(string Name, bool Enabled)> EnabledChanged { get; } = new();

        public bool IsEmpty =>
            Added.Count == 0 && Changed.Count == 0 && Removed.Count == 0 && EnabledChanged.Count == 0;
    }

    /// <summary>
    /// Pure manifest diffing (docs/ARCHITECTURE.md §2.2): compare by <c>name</c> + <c>schemaHash</c>
    /// (the hash excludes the enabled flag, §2.2). This is the "manifest diff" xUnit target (§9.3).
    /// </summary>
    public static class ManifestDiffer
    {
        public static ManifestDiff Compute(
            IReadOnlyDictionary<string, ToolDescriptor> previous,
            IReadOnlyList<ToolDescriptor> next)
        {
            var diff = new ManifestDiff();
            var nextByName = new Dictionary<string, ToolDescriptor>();

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
                {
                    diff.Changed.Add(desc);
                }
                else if (prev.Enabled != desc.Enabled)
                {
                    diff.EnabledChanged.Add((desc.Name, desc.Enabled));
                }
                // else: identical → no-op.
            }

            foreach (var name in previous.Keys)
            {
                if (!nextByName.ContainsKey(name))
                    diff.Removed.Add(name);
            }

            return diff;
        }

        /// <summary>
        /// Two descriptors are schema-equal when their <c>schemaHash</c> matches (the enabled flag is
        /// excluded from the hash by the plugin, §2.2). When a hash is absent on either side, fall back
        /// to comparing the structural signature so a manifest without hashes still diffs deterministically.
        /// </summary>
        private static bool SchemaEquals(ToolDescriptor a, ToolDescriptor b)
        {
            if (!string.IsNullOrEmpty(a.SchemaHash) || !string.IsNullOrEmpty(b.SchemaHash))
                return a.SchemaHash == b.SchemaHash;

            return Signature(a) == Signature(b);
        }

        private static string Signature(ToolDescriptor d)
        {
            // Structural fallback: everything that defines the tool's surface EXCEPT the enabled flag.
            var input = d.InputSchema?.ToJsonString() ?? "null";
            var output = d.OutputSchema?.ToJsonString() ?? "null";
            return string.Join("",
                d.Name, d.Title, d.Description, d.SkillDescription, d.SkillBody,
                input, output,
                d.ReadOnlyHint, d.DestructiveHint, d.IdempotentHint, d.OpenWorldHint, d.ExtensionId);
        }
    }
}
