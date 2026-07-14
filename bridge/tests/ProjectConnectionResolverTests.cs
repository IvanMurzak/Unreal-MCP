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
