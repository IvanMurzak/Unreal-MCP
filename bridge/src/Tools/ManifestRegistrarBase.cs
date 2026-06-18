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
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tools
{
    /// <summary>
    /// The minimal contract the IPC client needs to route an inbound manifest message of a given kind to its
    /// registrar without knowing the descriptor type: apply a manifest snapshot, and reset the revision guard
    /// on (re)connect (§1.5). Implemented by the prompt (P1) / resource (P2) registrars over their manifest
    /// message type; the IPC client holds them as <c>IManifestSink&lt;PromptManifestMessage&gt;</c> etc. so the
    /// v2 routing compiles in the P0 scaffold before the concrete registrars exist.
    /// </summary>
    public interface IManifestSink<in TManifest>
    {
        /// <summary>Apply a manifest snapshot (diff against the retained set, honouring the revision guard).</summary>
        void ApplyManifest(TManifest manifest);

        /// <summary>Reset the revision guard after a (re)connection handshake (§1.5).</summary>
        void ResetForReconnect();
    }

    /// <summary>
    /// The kind-agnostic result of diffing a freshly-received manifest against the previously applied set
    /// (docs/ARCHITECTURE.md §2.2 step 1 / §A.1). <typeparamref name="TDescriptor"/> is the per-entry
    /// descriptor (tool / prompt / resource). Pure data; computed by a kind-specific differ and consumed by
    /// <see cref="ManifestRegistrarBase{TDescriptor}"/>.
    /// </summary>
    public sealed class ManifestDiff<TDescriptor>
    {
        /// <summary>Entries present in the new manifest but not the previous set → add.</summary>
        public List<TDescriptor> Added { get; } = new();

        /// <summary>Entries whose schema hash changed → remove then add.</summary>
        public List<TDescriptor> Changed { get; } = new();

        /// <summary>Keys removed from the new manifest → remove.</summary>
        public List<string> Removed { get; } = new();

        /// <summary>Entries whose ONLY change is the enabled flag → set-enabled.</summary>
        public List<(string Key, bool Enabled)> EnabledChanged { get; } = new();

        public bool IsEmpty =>
            Added.Count == 0 && Changed.Count == 0 && Removed.Count == 0 && EnabledChanged.Count == 0;
    }

    /// <summary>
    /// The kind-agnostic manifest-application machinery shared by the tool, prompt (P1), and resource (P2)
    /// registrars (docs/ARCHITECTURE.md §2.2 / §A.1): the out-of-order revision guard, the §1.5 reconnect
    /// reset, the retained applied-snapshot, and the remove → changed → add → toggle-enabled orchestration.
    /// A subclass supplies the kind-specific behaviour through the abstract hooks — the diff, the sink
    /// mutation (add/remove/set-enabled), the descriptor key, and the enabled-flag mutation on a retained
    /// descriptor. NOT thread-safe by itself: the host serializes manifest application (the IPC client
    /// invokes Apply from its single reader loop), exactly as the tool registrar always has.
    ///
    /// <para>
    /// Generalizing the (formerly tool-only) <c>ManifestRegistrar</c> into this base lets P1/P2 instantiate
    /// prompt/resource registrars cheaply WITHOUT re-deriving the revision guard / reconnect reset / diff
    /// loop. The tool registrar (<see cref="ManifestRegistrar"/>) derives from this base and is behaviourally
    /// identical to its pre-generalization form (gated by the existing bridge xUnit suite).
    /// </para>
    /// </summary>
    public abstract class ManifestRegistrarBase<TDescriptor>
    {
        protected readonly ILogger? Logger;

        /// <summary>The currently-applied descriptors, keyed by their kind-specific key (name / uri).</summary>
        protected readonly Dictionary<string, TDescriptor> Applied = new();

        private int _lastAppliedRevision = -1;

        protected ManifestRegistrarBase(ILogger? logger) => Logger = logger;

        public int LastAppliedRevision => _lastAppliedRevision;

        /// <summary>
        /// Reset the revision guard after a (re)connection handshake (§1.5). The applied snapshot is
        /// retained — the long-lived McpPlugin still has the proxies registered, so a re-pushed manifest
        /// that is byte-identical diffs to a no-op (correct), while a genuine change while the link was
        /// down is still caught by the diff.
        /// </summary>
        public void ResetForReconnect() => _lastAppliedRevision = -1;

        /// <summary>The kind label used in log lines ("tool" / "prompt" / "resource").</summary>
        protected abstract string KindLabel { get; }

        /// <summary>Extract the kind-specific dedup/registration key from a descriptor (tool name / resource uri).</summary>
        protected abstract string KeyOf(TDescriptor descriptor);

        /// <summary>Compute the diff of <paramref name="next"/> against the retained <see cref="Applied"/> set.</summary>
        protected abstract ManifestDiff<TDescriptor> ComputeDiff(IReadOnlyList<TDescriptor> next);

        /// <summary>Remove the registered proxy for <paramref name="key"/> from the underlying manager.</summary>
        protected abstract void SinkRemove(string key);

        /// <summary>Create + register a fresh proxy for <paramref name="descriptor"/> on the underlying manager.</summary>
        protected abstract void SinkAdd(TDescriptor descriptor);

        /// <summary>Toggle the enabled flag for <paramref name="key"/> on the underlying manager.</summary>
        protected abstract void SinkSetEnabled(string key, bool enabled);

        /// <summary>Return a copy of <paramref name="descriptor"/> with its enabled flag set to <paramref name="enabled"/>
        /// (the retained snapshot is updated so a later diff sees the new flag).</summary>
        protected abstract TDescriptor WithEnabled(TDescriptor descriptor, bool enabled);

        /// <summary>
        /// Apply a manifest revision carrying <paramref name="entries"/>. Honours the out-of-order revision
        /// guard (§2.2 step 3 — a revision &lt;= the last applied is ignored, unless <see cref="ResetForReconnect"/>
        /// reset it to −1). Returns the diff that was applied (empty when ignored / no-op).
        /// </summary>
        protected ManifestDiff<TDescriptor> ApplyEntries(int revision, IReadOnlyList<TDescriptor> entries)
        {
            if (revision <= _lastAppliedRevision)
            {
                Logger?.LogDebug(
                    "Ignoring {Kind}-manifest revision {Revision} (<= last applied {LastApplied}).",
                    KindLabel, revision, _lastAppliedRevision);
                return new ManifestDiff<TDescriptor>();
            }

            var diff = ComputeDiff(entries);

            foreach (var key in diff.Removed)
            {
                SinkRemove(key);
                Applied.Remove(key);
            }

            foreach (var desc in diff.Changed)
            {
                var key = KeyOf(desc);
                SinkRemove(key);
                SinkAdd(desc);
                Applied[key] = desc;
            }

            foreach (var desc in diff.Added)
            {
                SinkAdd(desc);
                Applied[KeyOf(desc)] = desc;
            }

            foreach (var (key, enabled) in diff.EnabledChanged)
            {
                SinkSetEnabled(key, enabled);
                if (Applied.TryGetValue(key, out var existing))
                    Applied[key] = WithEnabled(existing, enabled);
            }

            _lastAppliedRevision = revision;

            if (!diff.IsEmpty)
            {
                Logger?.LogInformation(
                    "Applied {Kind}-manifest revision {Revision}: +{Added} ~{Changed} -{Removed} ⏼{Enabled} (total {Total}).",
                    KindLabel, revision, diff.Added.Count, diff.Changed.Count, diff.Removed.Count,
                    diff.EnabledChanged.Count, Applied.Count);
            }

            return diff;
        }
    }
}
