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
    /// mcp-authorize g3 (Phase-4 completion, design 04 D15 / 06): locks the local self-hosted invariant that
    /// <b>the editor local server's BIND port == the port Configure WRITES into a client config == the
    /// ProjectIdentity-derived per-project port</b>. Both come from the SAME McpPlugin
    /// <see cref="ProjectIdentity"/> derivation, so the "Configure button writes a URL the local server
    /// actually listens on" contract holds:
    /// <list type="bullet">
    ///   <item><b>Server bind side</b> — <see cref="ProjectConnectionResolver.Resolve"/> resolves the port the
    ///         sidecar delivers over the PR-4 <c>project-config</c> IPC; the C++
    ///         <c>FUnrealMcpEditorCoordinator</c> caches it as <c>DerivedLocalServerPort</c> and hands it to
    ///         <c>FUnrealMcpServerManager::Start</c> (off the old fixed-8080 <c>ParsePortFromHost</c> default,
    ///         migrated in PR #216).</item>
    ///   <item><b>Written config side</b> — the shared b6 writer
    ///         <see cref="AgentConfiguratorSettings.PinnedHttpUrl"/> rewrites a loopback <see cref="ConnectionMode.Local"/>
    ///         URL's port to <see cref="AgentConfiguratorSettings.ResolvedPort"/> (= <see cref="ProjectIdentity.Port"/>),
    ///         ignoring the raw engine-supplied host port.</item>
    /// </list>
    /// Both honor the D15 project-marker <c>portOverride</c>, so an explicit user override still wins on both
    /// sides. A future regression that repoints either side back onto a fixed port (or trusts the raw
    /// engine-supplied host port) breaks this — deterministically, without a live socket or editor.
    /// </summary>
    public class LocalServerPortConsistencyTests
    {
        // The URL shape the C++ editor UI actually sends as the b6 request's `host` on the default local path
        // (FAiAgentConnectionInfo::FromPluginConfig → ResolveCustomHost() + "/mcp", default host localhost:8080).
        // The b6 writer must DERIVE the loopback port from ProjectIdentity, NOT trust this raw 8080.
        private const string RawCustomHost = "http://localhost:8080/mcp";
        private const int RawHostPort = 8080;

        /// <summary>Build the settings the sidecar's <c>AgentConfigService.MapSettings</c> hands a configurator for the
        /// default (credential-free, native-OAuth) local path, then return the port of the URL that gets written.</summary>
        private static int WrittenConfigPort(string projectRoot)
        {
            var settings = AgentConfiguratorSettings.CreateForHost(
                projectRootPath: projectRoot,
                executableFullPath: string.Empty,
                port: RawHostPort,          // the raw engine-supplied port — must NOT be what gets written
                timeoutMs: 30000,
                host: RawCustomHost,
                token: null,
                connectionMode: ConnectionMode.Local);
            return new Uri(settings.PinnedHttpUrl).Port;
        }

        /// <summary>The port the C++ editor local server binds: the sidecar-resolved, IPC-delivered derived port.</summary>
        private static int ServerBindPort(string projectRoot) =>
            ProjectConnectionResolver.Resolve(projectRoot, instanceId: "editor-session").Port;

        [Fact]
        public void WrittenConfigPort_EqualsServerBindPort_OnDefaultLocalPath()
        {
            // Canonical golden vector (/home/user/my-game → 23940) — no project marker, so the hash-derived port.
            const string projectRoot = "/home/user/my-game";
            var derived = ProjectIdentity.Derive(projectRoot).Port;
            Assert.Equal(23940, derived);

            var bind = ServerBindPort(projectRoot);
            var written = WrittenConfigPort(projectRoot);

            // The DoD: server bind == written config == the ProjectIdentity-derived port.
            Assert.Equal(derived, bind);
            Assert.Equal(derived, written);
            Assert.Equal(bind, written);

            // And the written port is the DERIVED port, never the raw fixed host port — proving the migration off
            // the old fixed-8080 default really happened on the writer side too.
            Assert.NotEqual(RawHostPort, written);
        }

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
