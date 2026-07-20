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
using System.IO;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using Xunit;
using ConnectionMode = com.IvanMurzak.McpPlugin.AgentConfig.ConnectionMode;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// mcp-authorize g3 (Phase-4 completion, design 04 D15 / 06), re-anchored by auth-fixes T1 (defect A, owner
    /// ruling 2026-07-19): locks the local self-hosted invariant that <b>the editor local server's BIND port ==
    /// the port Configure WRITES into a client config</b> — the "Configure button writes a URL the local server
    /// actually listens on" contract:
    /// <list type="bullet">
    ///   <item><b>Server bind side</b> — <see cref="ProjectConnectionResolver.Resolve"/> resolves the port the
    ///         sidecar delivers over the PR-4 <c>project-config</c> IPC; the C++
    ///         <c>FUnrealMcpEditorCoordinator</c> caches it as <c>DerivedLocalServerPort</c> and hands it to
    ///         <c>FUnrealMcpServerManager::Start</c> (off the old fixed-8080 <c>ParsePortFromHost</c> default,
    ///         migrated in PR #216).</item>
    ///   <item><b>Written config side</b> — the shared b6 writer
    ///         <see cref="AgentConfiguratorSettings.PinnedHttpUrl"/> sets a loopback
    ///         <see cref="ConnectionMode.Local"/> URL's port.</item>
    /// </list>
    ///
    /// <para><b>What changed.</b> The invariant used to be the stronger "…== the ProjectIdentity-DERIVED port",
    /// because both sides stopped at {marker <c>portOverride</c>, derived} and both deliberately IGNORED a port
    /// the user typed into the host. MCP-Plugin-dotnet #174/#176 reversed that on the writer side (owner ruling:
    /// "the Configure button must use exactly the port the user enters"), inserting a typed-host level BETWEEN
    /// the two. This suite therefore now locks <b>bind == written</b> under the shared three-level precedence —
    /// marker <c>portOverride</c> &gt; explicit port typed into the loopback host &gt; deterministic derivation —
    /// rather than pinning both onto the derivation. The per-level BINDER contract lives in
    /// <c>ProjectConnectionResolverTests</c>; this file is the writer-vs-binder PARITY suite.</para>
    ///
    /// <para><b>Pin sensitivity.</b> Parity on a host whose typed port differs from the derived one can only hold
    /// once the bridge consumes a writer that implements level 2 — McpPlugin <b>7.3.0</b>. On the current 7.2.0
    /// pin the writer still overwrites the typed port, so that one vector is marked pending below; the vectors
    /// that resolve identically under BOTH writers (marker override, and a host carrying no explicit port) stay
    /// green and keep this suite live. All deterministic — no live socket or editor.</para>
    /// </summary>
    public class LocalServerPortConsistencyTests
    {
        // The URL shape the C++ editor UI actually sends as the b6 request's `host` on the default local path
        // (FAiAgentConnectionInfo::FromPluginConfig → ResolveCustomHost() + "/mcp"). NOTE the default host is the
        // HARDCODED FUnrealMcpConfig::DefaultCustomHost "http://localhost:8080" — unlike Unity, whose default host
        // already carries the derived port. So under the new precedence this 8080 IS a "typed" port to both sides.
        private const string RawCustomHost = "http://localhost:8080/mcp";
        private const int RawHostPort = 8080;

        /// <summary>Build the settings the sidecar's <c>AgentConfigService.MapSettings</c> hands a configurator for the
        /// default (credential-free, native-OAuth) local path, then return the port of the URL that gets written.</summary>
        private static int WrittenConfigPort(string projectRoot, string host = RawCustomHost)
        {
            var settings = AgentConfiguratorSettings.CreateForHost(
                projectRootPath: projectRoot,
                executableFullPath: string.Empty,
                port: RawHostPort,          // the raw engine-supplied port — not itself a precedence level
                timeoutMs: 30000,
                host: host,
                token: null,
                connectionMode: ConnectionMode.Local);
            return new Uri(settings.PinnedHttpUrl).Port;
        }

        /// <summary>The port the C++ editor local server binds: the sidecar-resolved, IPC-delivered port. Fed the SAME
        /// host the writer gets — the sidecar reads it off the §8 <c>config</c> message (<c>SidecarHost.LocalBindHost</c>),
        /// so passing it here is what makes this a like-for-like parity check rather than two different questions.</summary>
        private static int ServerBindPort(string projectRoot, string host = RawCustomHost) =>
            ProjectConnectionResolver.Resolve(projectRoot, instanceId: "editor-session", localHost: host).Port;

        // Un-skipped by the McpPlugin 7.3.0 pin bump (PRs #174/#176), exactly as the skip note prescribed:
        // the pinned writer now honours the typed loopback port, so writer and binder agree again. No assertion
        // was changed -- the pin is what makes this pass.
        [Fact]
        public void WrittenConfigPort_EqualsServerBindPort_OnDefaultLocalPath()
        {
            // Canonical golden vector (/home/user/my-game → 23940) — no project marker, and the default host
            // carries an explicit 8080, so precedence level 2 (typed host) decides on BOTH sides.
            const string projectRoot = "/home/user/my-game";
            var derived = ProjectIdentity.Derive(projectRoot).Port;
            Assert.Equal(23940, derived);

            var bind = ServerBindPort(projectRoot);
            var written = WrittenConfigPort(projectRoot);

            // The DoD: server bind == written config.
            Assert.Equal(bind, written);

            // Both name the port typed into the host, not the derivation — the owner-ruled precedence. The
            // NotEqual proves level 2 actually decided (8080 and 23940 differ), so an accidental fallback to the
            // derivation on either side would fail here rather than pass by coincidence.
            Assert.Equal(RawHostPort, bind);
            Assert.Equal(RawHostPort, written);
            Assert.NotEqual(derived, written);
        }

        /// <summary>
        /// Parity on a host carrying NO explicit port: precedence level 2 does not apply, so both sides fall back
        /// to the deterministic derivation. Pin-independent — the 7.2.0 and 7.3.0 writers agree here — so this is
        /// the vector that keeps the derived-port half of the invariant under live guard while the default-path
        /// fact above waits on the pin bump. It also pins the load-bearing "explicit port" reading: a port-less
        /// host must NOT be read as the scheme default (<c>Uri.Port</c> would say 80 and bind 80).
        /// </summary>
        [Fact]
        public void WrittenConfigPort_EqualsServerBindPort_WhenHostCarriesNoExplicitPort()
        {
            const string projectRoot = "/home/user/my-game";
            const string portlessHost = "http://localhost/mcp";
            var derived = ProjectIdentity.Derive(projectRoot).Port;
            Assert.Equal(23940, derived);

            var bind = ServerBindPort(projectRoot, portlessHost);
            var written = WrittenConfigPort(projectRoot, portlessHost);

            Assert.Equal(bind, written);
            Assert.Equal(derived, bind);
            Assert.NotEqual(80, bind);
        }

        /// <summary>
        /// D15: an explicit user override in the committable project marker is precedence level 1 — it outranks
        /// BOTH the typed host port and the derivation, on the bind and written sides alike. Pin-independent: the
        /// 7.2.0 writer resolves it via <c>ResolvedPort</c> and the 7.3.0 writer via <c>PinnedPort</c>'s level 1,
        /// to the same value. Deliberately left as it was written — the new precedence does not disturb it, and
        /// weakening or re-pointing a still-correct assertion would lose the coverage.
        /// </summary>
        [Fact]
        public void WrittenConfigPort_EqualsServerBindPort_WithD15PortOverride()
        {
            RunInTempDir(projectRoot =>
            {
                // D15: an explicit user override in the committable project marker wins over the hash-derived port —
                // and must win IDENTICALLY on both the server-bind and written-config sides. Pick an override that is
                // GUARANTEED to differ from THIS temp dir's hash-derived port — both live in the same 20000–29999
                // band, so a fixed constant has a ~1/10000 chance of colliding with the random-temp-path hash, which
                // would flip the override-is-exercised assertion below into a spurious failure. Deriving first and
                // stepping off the collision keeps the assertion meaningful (bind == override != derived ⇒ the
                // override path really won) while making it deterministic.
                var derivedPort = ProjectIdentity.DerivePort(projectRoot);
                var overridePort = derivedPort == 26543 ? 26544 : 26543;
                new ProjectMarker { PortOverride = overridePort }.Write(projectRoot);

                var bind = ServerBindPort(projectRoot);
                var written = WrittenConfigPort(projectRoot);

                Assert.Equal(overridePort, bind);
                Assert.Equal(overridePort, written);
                Assert.Equal(bind, written);
                // The override is NOT the hash-derived port (proves the override path is exercised, not a coincidence).
                Assert.NotEqual(overridePort, derivedPort);
                // …nor the port typed into RawCustomHost, so this also proves level 1 outranks level 2: the marker
                // wins even though the host carries an explicit 8080. (Structural — the override is drawn from the
                // 20000–29999 band — but asserted so a future band/default change cannot silently void the claim.)
                Assert.NotEqual(RawHostPort, overridePort);
            });
        }

        private static void RunInTempDir(Action<string> body)
        {
            var dir = Path.Combine(Path.GetTempPath(), "umcp-portcons-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            try
            {
                body(dir);
            }
            finally
            {
                try { Directory.Delete(dir, recursive: true); } catch { /* best-effort cleanup */ }
            }
        }
    }
}
