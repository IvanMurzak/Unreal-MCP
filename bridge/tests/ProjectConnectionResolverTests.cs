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
using System.Linq;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the sidecar's project connection-identity resolution (mcp-authorize PR 3, design 04/06):
    /// the golden-vector pin/port parity of the consumed McpPlugin <see cref="ProjectIdentity"/>, the
    /// instance-metadata handshake payload the sidecar attaches to the hub connection, the project-name
    /// derivation, and the project-marker read/write round-trip (server target + port override) — all
    /// deterministically, without a live socket.
    /// </summary>
    public class ProjectConnectionResolverTests
    {
        // Canonical cross-language golden vectors, copied verbatim from the committed
        // McpPlugin/src/AgentConfig/ProjectIdentity.GoldenVectors.json. This locks that the McpPlugin version the
        // bridge consumes still derives the byte-for-byte pin+port the whole mcp-authorize routing plane pins on —
        // a future McpPlugin bump that silently changed the derivation would fail here.
        [Theory]
        [InlineData("/home/user/my-game", "34ea75f2", 23940)]
        [InlineData("/home/user/my-game/", "34ea75f2", 23940)]  // trailing slash trimmed
        [InlineData("/home/USER/My-Game", "34ea75f2", 23940)]   // ToLowerInvariant case-fold
        [InlineData("C:\\Users\\user\\my-game", "8ef72cf7", 29310)]
        [InlineData("C:\\Users\\user\\my-game\\", "8ef72cf7", 29310)] // trailing backslash trimmed
        [InlineData("C:/Users/user/my-game", "5a87324e", 24298)] // forward-slash DIFFERS — separators NOT normalized
        [InlineData("/home/İstanbul/game", "672d80a7", 25303)] // U+0130 — ToLowerInvariant leaves it unchanged
        [InlineData("/srv/games/space sim", "08c6cbb6", 27816)] // path with a space
        public void ProjectIdentity_MatchesGoldenVectors(string path, string expectedPin, int expectedPort)
        {
            var identity = ProjectIdentity.Derive(path);
            Assert.Equal(expectedPin, identity.Pin);
            Assert.Equal(expectedPort, identity.Port);
            Assert.False(identity.PortIsOverridden);
        }

        [Fact]
        public void Resolve_BuildsInstanceMetadata_WithUnrealEngineAndDerivedHash()
        {
            const string root = "/home/user/my-game";
            var resolved = ProjectConnectionResolver.Resolve(root, instanceId: "inst-123", machineName: "DESKTOP-X");

            Assert.Equal("unreal", resolved.Metadata.Engine);
            Assert.Equal("inst-123", resolved.Metadata.InstanceId);
            Assert.Equal("DESKTOP-X", resolved.Metadata.MachineName);
            // The pin is the first 8 hex chars of the full project-path hash by construction (server pin-match).
            Assert.Equal("34ea75f2", resolved.Pin);
            Assert.Equal(64, resolved.ProjectPathHash.Length);
            Assert.StartsWith(resolved.Pin, resolved.ProjectPathHash, StringComparison.Ordinal);
            Assert.Equal(resolved.ProjectPathHash, resolved.Metadata.ProjectPathHash);
            Assert.Equal(ProjectIdentity.DeriveProjectPathHash(root), resolved.ProjectPathHash);
            // No marker present → no server target, hash-derived (non-overridden) port.
            Assert.Null(resolved.ServerTarget);
            Assert.Equal(23940, resolved.Port);
            Assert.False(resolved.PortIsOverridden);
        }

        [Fact]
        public void Resolve_Metadata_TravelsAsHubQueryAndUrl()
        {
            var resolved = ProjectConnectionResolver.Resolve("/home/user/my-game", "inst-abc", "DESKTOP-X");

            var query = resolved.Metadata.ToQuery();
            Assert.Contains("inst-abc", query.Values);                       // InstanceId always present
            Assert.Contains(resolved.ProjectPathHash, query.Values);         // full hash for pin-match
            Assert.Contains("unreal", query.Values);

            // The hash is hex (no URL-escaping) so it appears verbatim on the appended hub URL; the token never does.
            var url = resolved.Metadata.AppendToUrl("https://ai-game.dev/mcp/hub/mcp-server");
            Assert.Contains(resolved.ProjectPathHash, url);
            Assert.StartsWith("https://ai-game.dev/mcp/hub/mcp-server?", url);
        }

#if USE_LOCAL_MCP_PLUGIN
        /// <summary>
        /// Dual-hash transition (auth-fixes T3 / defect B5, MCP-Plugin-dotnet #165). Built against the
        /// SOURCE LIB (<c>-p:UseLocalMcpPlugin=true</c>) the sidecar's instance metadata carries BOTH the v2
        /// <c>projectPathHash</c> (separator-normalized) AND the v1 legacy hash, so a session pinned by an
        /// OLD (v1) config still matches this NEW plugin. A Windows-style BACKSLASH root is used
        /// deliberately: v2 converts <c>'\' -&gt; '/'</c> before hashing while v1 does not, so the two
        /// hashes DIFFER — proving the legacy hash is emitted independently, not the v2 hash echoed.
        /// (Excluded from the default NuGet build, whose pinned McpPlugin lacks these primitives; the pin
        /// gains them in the release wave k3/R1.)
        /// </summary>
        [Fact]
        public void Resolve_Metadata_EmitsBothV2AndLegacyHashes_ForWindowsRoot()
        {
            const string root = @"C:\Users\user\my-game";
            var resolved = ProjectConnectionResolver.Resolve(root, instanceId: "inst-dual", machineName: "DESKTOP-X");

            var meta = resolved.Metadata;
            // Primary hash is the v2 (separator-normalized) hash; legacy is the v1 hash of the same root.
            Assert.Equal(ProjectIdentity.DeriveProjectPathHashV2(root), meta.ProjectPathHash);
            Assert.Equal(ProjectIdentity.DeriveProjectPathHash(root), meta.ProjectPathHashLegacy);
            // On a backslash path the two normalizations diverge, so the hashes MUST differ (the whole point
            // of the transition — kills the Windows v1/v2 pin mismatch, defect B5).
            Assert.NotEqual(meta.ProjectPathHash, meta.ProjectPathHashLegacy);
            Assert.Equal(64, meta.ProjectPathHash.Length);
            Assert.Equal(64, meta.ProjectPathHashLegacy.Length);

            // Both hashes travel as non-secret hub query params so the server can pin-match on EITHER.
            var values = resolved.Metadata.ToQuery().Values;
            Assert.Contains(meta.ProjectPathHash, values);
            Assert.Contains(meta.ProjectPathHashLegacy, values);
        }

        /// <summary>
        /// A POSIX root (no backslashes) hashes IDENTICALLY under v1 and v2 — the only v2 step is
        /// <c>'\' -&gt; '/'</c> — so the legacy hash EQUALS the primary; it is still emitted, so an old
        /// v1-pinned config matches. Locks that the dual-hash payload is present even when the two coincide.
        /// </summary>
        [Fact]
        public void Resolve_Metadata_EmitsLegacyHash_EvenWhenEqualToV2_ForPosixRoot()
        {
            const string root = "/home/user/my-game";
            var resolved = ProjectConnectionResolver.Resolve(root, instanceId: "inst-posix");

            var meta = resolved.Metadata;
            Assert.Equal(ProjectIdentity.DeriveProjectPathHashV2(root), meta.ProjectPathHash);
            Assert.Equal(ProjectIdentity.DeriveProjectPathHash(root), meta.ProjectPathHashLegacy);
            // No backslashes -> v1 and v2 normalization coincide -> identical hashes, both present.
            Assert.Equal(meta.ProjectPathHash, meta.ProjectPathHashLegacy);
            Assert.False(string.IsNullOrEmpty(meta.ProjectPathHashLegacy));
        }
#endif

        [Fact]
        public void ResolveProjectName_PrefersUProjectBasename_ThenDirectoryName()
        {
            RunInTempDir(dir =>
            {
                // No .uproject yet → directory-name fallback (matches the trimmed leaf).
                Assert.Equal(Path.GetFileName(dir.TrimEnd('/', '\\')), ProjectConnectionResolver.ResolveProjectName(dir));

                // A .uproject whose basename differs from the folder name (the testbed shape: test-project/UnrealTestProject.uproject).
                File.WriteAllText(Path.Combine(dir, "MyGame.uproject"), "{}");
                Assert.Equal("MyGame", ProjectConnectionResolver.ResolveProjectName(dir));
            });
        }

        [Fact]
        public void ResolveProjectName_BlankOrMissingRoot_IsSafe()
        {
            Assert.Equal(string.Empty, ProjectConnectionResolver.ResolveProjectName(""));
            Assert.Equal(string.Empty, ProjectConnectionResolver.ResolveProjectName("   "));
            // A non-existent path yields the leaf directory name (best-effort; never throws).
            Assert.Equal("does-not-exist", ProjectConnectionResolver.ResolveProjectName("/no/such/does-not-exist"));
        }

        [Fact]
        public void Marker_WriteThenResolve_RoundTripsServerTarget()
        {
            RunInTempDir(dir =>
            {
                // No marker → null server target.
                Assert.Null(ProjectConnectionResolver.SafeReadMarker(dir));
                Assert.Null(ProjectConnectionResolver.Resolve(dir, "id").ServerTarget);

                // WRITE the enrolled server target, then RESOLVE it back.
                Assert.True(ProjectConnectionResolver.PersistServerTarget(dir, "https://ai-game.dev"));
                Assert.True(File.Exists(Path.Combine(dir, ProjectMarker.DirectoryName, ProjectMarker.FileName)));
                Assert.Equal("https://ai-game.dev", ProjectConnectionResolver.Resolve(dir, "id").ServerTarget);

                // Idempotent: re-persisting the SAME value is a no-op (no file churn).
                Assert.False(ProjectConnectionResolver.PersistServerTarget(dir, "https://ai-game.dev"));
                // A changed value writes again.
                Assert.True(ProjectConnectionResolver.PersistServerTarget(dir, "http://localhost:5383"));
                Assert.Equal("http://localhost:5383", ProjectConnectionResolver.Resolve(dir, "id").ServerTarget);
            });
        }

        [Fact]
        public void Marker_PortOverride_WinsOverDerivedPort_AndPreservedOnServerTargetWrite()
        {
            RunInTempDir(dir =>
            {
                new ProjectMarker { ServerTarget = "https://ai-game.dev", PortOverride = 25555 }.Write(dir);

                var resolved = ProjectConnectionResolver.Resolve(dir, "id");
                Assert.Equal(25555, resolved.Port);
                Assert.True(resolved.PortIsOverridden);
                Assert.Equal("https://ai-game.dev", resolved.ServerTarget);

                // Re-persisting a DIFFERENT server target must PRESERVE the user's port override.
                Assert.True(ProjectConnectionResolver.PersistServerTarget(dir, "http://localhost:5383"));
                var reread = ProjectMarker.Read(dir)!;
                Assert.Equal(25555, reread.PortOverride);
                Assert.Equal("http://localhost:5383", reread.ServerTarget);
            });
        }

        // ── Local-server bind-port precedence (auth-fixes T1 / defect A, owner ruling 2026-07-19) ────────────
        // The binder's OWN contract: marker portOverride (1) > explicit port typed into the loopback host (2) >
        // deterministic derivation (3) — the same three levels the shared McpPlugin writer applies, so the port
        // the local gamedev-mcp-server BINDS is the port the Configure button WRITES. These assert the resolver
        // directly (given root + marker + host → port), so they hold on the CURRENT McpPlugin pin; the
        // writer-vs-binder parity that needs 7.3.0 lives in LocalServerPortConsistencyTests.

        /// <summary>
        /// Level 2 beats level 3: with no marker, a port the user typed into the Custom-mode loopback host is what
        /// the local server binds — NOT the hash-derived port. This is the behaviour reversal T1 delivers; before
        /// it the resolver ignored the typed port to mirror the OLD writer.
        /// </summary>
        [Fact]
        public void Resolve_TypedLoopbackHostPort_WinsOverDerivedPort()
        {
            const string root = "/home/user/my-game";   // golden vector → derived 23940
            var resolved = ProjectConnectionResolver.Resolve(
                root, instanceId: "inst-typed", machineName: null, localHost: "http://localhost:27618/mcp");

            Assert.Equal(27618, resolved.Port);
            Assert.Equal(LocalServerPortSource.TypedHost, resolved.PortSource);
            Assert.True(resolved.PortIsOverridden);
            // The typed port really displaced the derivation (not a coincidental match).
            Assert.NotEqual(ProjectIdentity.DerivePort(root), resolved.Port);
        }

        /// <summary>
        /// Level 1 beats level 2: a marker <c>portOverride</c> is a deliberate per-project pin, so it wins even
        /// when the host ALSO carries an explicit port. Both are user choices; the marker is the more specific one.
        /// </summary>
        [Fact]
        public void Resolve_MarkerPortOverride_WinsOverTypedLoopbackHostPort()
        {
            RunInTempDir(dir =>
            {
                // Step off the temp dir's own derived port so "override != derived" stays deterministic (same
                // reasoning as LocalServerPortConsistencyTests — both live in the 20000–29999 band).
                var derivedPort = ProjectIdentity.DerivePort(dir);
                var overridePort = derivedPort == 26543 ? 26544 : 26543;
                new ProjectMarker { PortOverride = overridePort }.Write(dir);

                var resolved = ProjectConnectionResolver.Resolve(
                    dir, instanceId: "inst-both", machineName: null, localHost: "http://localhost:27618/mcp");

                Assert.Equal(overridePort, resolved.Port);
                Assert.Equal(LocalServerPortSource.MarkerOverride, resolved.PortSource);
                Assert.True(resolved.PortIsOverridden);
                // Neither of the two losing levels was picked — so the ordering, not a value collision, decided.
                Assert.NotEqual(27618, resolved.Port);
                Assert.NotEqual(derivedPort, resolved.Port);
            });
        }

        /// <summary>
        /// Level 3 stands when the host carries NO explicit port. Load-bearing: <c>Uri.Port</c> would synthesise
        /// the scheme default (80) here, which would make "no port typed" indistinguishable from "80 typed" and
        /// bind 80 instead of the per-project derived port — which is why the resolver parses the RAW host string.
        /// </summary>
        [Fact]
        public void Resolve_HostWithoutExplicitPort_FallsBackToDerivedPort()
        {
            const string root = "/home/user/my-game";
            var resolved = ProjectConnectionResolver.Resolve(
                root, instanceId: "inst-portless", machineName: null, localHost: "http://localhost/mcp");

            Assert.Equal(23940, resolved.Port);
            Assert.Equal(LocalServerPortSource.Derived, resolved.PortSource);
            Assert.False(resolved.PortIsOverridden);
            Assert.NotEqual(80, resolved.Port);
        }

        /// <summary>
        /// A NON-loopback host contributes no level 2, mirroring the writer's
        /// <c>ConnectionMode.Local &amp;&amp; uri.IsLoopback</c> gate: a hosted target keeps its authority verbatim
        /// there and has no local server to bind here, so the derived port stands on both sides.
        /// </summary>
        [Fact]
        public void Resolve_NonLoopbackHostPort_IsIgnored()
        {
            const string root = "/home/user/my-game";
            var resolved = ProjectConnectionResolver.Resolve(
                root, instanceId: "inst-remote", machineName: null, localHost: "https://mcp.example.com:9999/mcp");

            Assert.Equal(23940, resolved.Port);
            Assert.Equal(LocalServerPortSource.Derived, resolved.PortSource);
        }

        /// <summary>
        /// An absent host (Cloud mode, or no config pushed yet) leaves the pre-T1 levels 1+3 behaviour exactly as
        /// it was — the change is strictly additive for every caller that supplies no host.
        /// </summary>
        [Fact]
        public void Resolve_NoHost_KeepsDerivedPort()
        {
            var resolved = ProjectConnectionResolver.Resolve("/home/user/my-game", instanceId: "inst-nohost");

            Assert.Equal(23940, resolved.Port);
            Assert.Equal(LocalServerPortSource.Derived, resolved.PortSource);
        }

        /// <summary>
        /// The level-2 host parser, mirroring <c>AgentConfiguratorSettings.TryGetExplicitPort</c> + the writer's
        /// loopback gate: which host strings yield a typed port at all. Anything that yields <c>null</c> falls
        /// through to the derived port rather than binding something unusable.
        /// </summary>
        [Theory]
        [InlineData("http://localhost:27618/mcp", 27618)]     // the canonical typed-port case
        [InlineData("http://127.0.0.1:27618", 27618)]         // IPv4 loopback literal
        [InlineData("http://[::1]:27618/mcp", 27618)]         // IPv6 literal — port follows the closing bracket
        [InlineData("http://user:pass@localhost:27618", 27618)] // userinfo colon is not a port separator
        [InlineData("http://LOCALHOST:27618", 27618)]         // loopback detection is case-insensitive
        [InlineData("http://localhost/mcp", null)]            // no port typed — NOT the scheme default 80
        [InlineData("http://localhost:/mcp", null)]           // empty port
        [InlineData("http://localhost:abc/mcp", null)]        // non-numeric
        [InlineData("http://localhost:70000", null)]          // out of range (> Consts.Hub.MaxPort)
        [InlineData("http://localhost:0", null)]              // zero is not a bindable port
        [InlineData("https://mcp.example.com:9999", null)]    // not loopback
        [InlineData("localhost:27618", null)]                 // not an absolute URI
        [InlineData("", null)]
        [InlineData(null, null)]
        public void TryGetExplicitLoopbackPort_ReadsOnlyAnExplicitLoopbackPort(string? host, int? expected)
        {
            Assert.Equal(expected, ProjectConnectionResolver.TryGetExplicitLoopbackPort(host));
        }

        [Fact]
        public void PersistServerTarget_BlankInputs_AreNoOps()
        {
            RunInTempDir(dir =>
            {
                Assert.False(ProjectConnectionResolver.PersistServerTarget(dir, null));
                Assert.False(ProjectConnectionResolver.PersistServerTarget(dir, "   "));
                Assert.False(File.Exists(Path.Combine(dir, ProjectMarker.DirectoryName, ProjectMarker.FileName)));
            });
        }

        private static void RunInTempDir(Action<string> body)
        {
            var dir = Path.Combine(Path.GetTempPath(), "umcp-pcr-" + Guid.NewGuid().ToString("N"));
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
