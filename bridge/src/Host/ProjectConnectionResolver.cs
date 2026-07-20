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
using System.Globalization;
using System.IO;
using System.Linq;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.McpPlugin.Common;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// Which of the three precedence levels supplied the resolved local-server bind
    /// <see cref="ProjectConnection.Port"/> (auth-fixes T1 / defect A, owner ruling 2026-07-19). Ordered
    /// least- to most-specific so <see cref="ProjectConnection.PortIsOverridden"/> can be expressed as
    /// "anything but <see cref="Derived"/>".
    /// </summary>
    internal enum LocalServerPortSource
    {
        /// <summary>Level 3 — the deterministic <see cref="ProjectIdentity"/> hash-derived per-project port.</summary>
        Derived,

        /// <summary>Level 2 — an explicit port the user typed into the Custom-mode loopback host.</summary>
        TypedHost,

        /// <summary>Level 1 — the committable project marker's <c>portOverride</c>.</summary>
        MarkerOverride,
    }

    /// <summary>
    /// The resolved connection identity of the editor's project (mcp-authorize PR 3, design docs 04/06):
    /// the routing <see cref="Pin"/> + local-server bind <see cref="Port"/> (McpPlugin
    /// <see cref="ProjectIdentity"/> plus the typed-host level — see
    /// <see cref="ProjectConnectionResolver.Resolve"/>), the enrolled <see cref="ServerTarget"/> and the full
    /// <see cref="ProjectPathHash"/> read/derived from the committable project marker + project root, plus the
    /// ready-to-attach <see cref="Metadata"/> handshake payload. Every field is derived deterministically —
    /// no live socket, no shared state.
    /// </summary>
    internal sealed record ProjectConnection(
        string Pin,
        int Port,
        LocalServerPortSource PortSource,
        string? ServerTarget,
        string ProjectPathHash,
        string ProjectName,
        ConnectionInstanceMetadata Metadata)
    {
        /// <summary>
        /// Whether <see cref="Port"/> is a USER-chosen port — the marker's <c>portOverride</c> (level 1) or a
        /// port typed into the loopback host (level 2) — rather than the deterministic derivation (level 3).
        /// Informational only: it rides the <c>project-config-result</c> so the C++ plugin can log
        /// "[user override]".
        /// </summary>
        public bool PortIsOverridden => PortSource != LocalServerPortSource.Derived;
    }

    /// <summary>
    /// Resolves the sidecar's <b>connection identity</b> for the editor's project (mcp-authorize PR 3,
    /// design docs 04/06). Given the project root the C++ plugin reports over IPC (the handshake-ack
    /// <c>projectPath</c> — <c>FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())</c>), it:
    /// <list type="bullet">
    ///   <item>reads the committable project marker <c>&lt;project&gt;/.ai-game-dev/project.json</c>
    ///         (<see cref="ProjectMarker"/>) for the user's optional port override + the enrolled server target;</item>
    ///   <item>derives the routing pin + deterministic local port via the single-source
    ///         <see cref="ProjectIdentity"/> — byte-for-byte the committed golden vectors (trim trailing
    ///         separators, <c>ToLowerInvariant</c> only, separators NOT converted) — then layers the
    ///         user-typed loopback port over it (see <see cref="Resolve"/>'s precedence list);</item>
    ///   <item>builds the <see cref="ConnectionInstanceMetadata"/> the sidecar attaches to its SignalR hub
    ///         connection (via <see cref="ConnectionConfig.InstanceMetadata"/>) so the server's account+instance
    ///         pairing plane (b3) registers THIS editor session under the resolved account.</item>
    /// </list>
    /// Pure/stateless — the bridge xUnit suite locks the golden-vector parity + marker round-trip without a socket.
    /// This PR wires the pin + server-target RESOLUTION in C#; it does not add a new IPC message and does not
    /// change the C++ deterministic port (PR 4).
    /// </summary>
    internal static class ProjectConnectionResolver
    {
        /// <summary>The engine identifier this sidecar reports in its instance metadata (design 04/06).</summary>
        internal const string Engine = "unreal";

        /// <summary>
        /// Resolve the full connection identity for <paramref name="projectRoot"/>. Reads the on-disk project
        /// marker (may be absent) for the port override + server target, resolves the local-server bind port,
        /// derives the pin, and builds the instance-metadata handshake payload. <paramref name="instanceId"/> is
        /// the per-editor-session GUID (stable across reconnects); <paramref name="machineName"/> defaults to the
        /// host machine name.
        ///
        /// <para><b>Bind-port precedence</b> (auth-fixes T1 / defect A, owner ruling 2026-07-19) — the SAME three
        /// levels the shared McpPlugin writer applies in <c>AgentConfiguratorSettings.PinnedPort</c>, so the port
        /// the local <c>gamedev-mcp-server</c> BINDS is the port the Configure button WRITES:
        /// <list type="number">
        ///   <item>the project marker's <c>portOverride</c> — a deliberate per-project pin, wins outright;</item>
        ///   <item>an explicit port in <paramref name="localHost"/> — the port the USER typed into the Custom-mode
        ///         host — applied only for an absolute LOOPBACK host, mirroring the writer's
        ///         <c>ConnectionMode.Local &amp;&amp; uri.IsLoopback</c> gate;</item>
        ///   <item>the deterministic <see cref="ProjectIdentity"/> hash-derived per-project port.</item>
        /// </list>
        /// Level 2 is the auth-fixes T1 change. Before it this resolver stopped at levels 1+3 — deliberately, to
        /// mirror the OLD writer, which overwrote a loopback URL's port with the derived one. MCP-Plugin-dotnet
        /// #174/#176 reversed that on the writer side, so keeping the binder at two levels would make the sidecar
        /// listen on the derived port while the written config named the typed one — the reported "Configure
        /// writes a port the server does not listen on" bug, newly introduced into Unreal.</para>
        ///
        /// <para><paramref name="localHost"/> is the plugin's effective <b>Custom-mode</b> host
        /// (<c>FUnrealMcpConfig::ResolveCustomHost()</c>, delivered over the §8 <c>config</c> IPC message). Pass
        /// <c>null</c> in Cloud mode or when no config has been applied — level 2 is then skipped and the
        /// pre-change levels 1+3 behaviour holds exactly.</para>
        /// </summary>
        public static ProjectConnection Resolve(
            string projectRoot,
            string instanceId,
            string? machineName = null,
            string? localHost = null)
        {
            if (projectRoot == null)
                throw new ArgumentNullException(nameof(projectRoot));
            if (string.IsNullOrEmpty(instanceId))
                throw new ArgumentException("InstanceId must be non-empty.", nameof(instanceId));

            var marker = SafeReadMarker(projectRoot);
            var identity = ProjectIdentity.Derive(projectRoot, marker);
            var typedPort = TryGetExplicitLoopbackPort(localHost);
            var (port, portSource) = identity.PortIsOverridden
                ? (identity.Port, LocalServerPortSource.MarkerOverride)   // 1. marker portOverride
                : typedPort.HasValue
                    ? (typedPort.Value, LocalServerPortSource.TypedHost)  // 2. port typed into the loopback host
                    : (identity.Port, LocalServerPortSource.Derived);     // 3. deterministic derivation
            var projectName = ResolveProjectName(projectRoot);

            // ConnectionInstanceMetadata.Create derives projectPathHash from the SAME normalized root the pin
            // came from, so the server can pin-match the session (pin is a prefix of the hash by construction).
            var metadata = ConnectionInstanceMetadata.Create(
                engine: Engine,
                projectName: projectName,
                projectRootPath: projectRoot,
                instanceId: instanceId,
                machineName: machineName);

            return new ProjectConnection(
                Pin: identity.Pin,
                Port: port,
                PortSource: portSource,
                ServerTarget: marker?.ServerTarget,
                ProjectPathHash: metadata.ProjectPathHash,
                ProjectName: projectName,
                Metadata: metadata);
        }

        /// <summary>
        /// The port the user explicitly typed into <paramref name="host"/>, or <c>null</c> when there is none —
        /// precedence level 2 of <see cref="Resolve"/>. Returns <c>null</c> unless <paramref name="host"/> parses
        /// as an ABSOLUTE LOOPBACK URI, mirroring the writer's <c>ConnectionMode.Local &amp;&amp; uri.IsLoopback</c>
        /// gate in <c>AgentConfiguratorSettings.BuildPinnedHttpUrl</c>: a hosted / non-loopback target keeps its
        /// authority verbatim there and has no local server to bind here, so neither side rewrites its port.
        /// </summary>
        internal static int? TryGetExplicitLoopbackPort(string? host)
        {
            if (string.IsNullOrWhiteSpace(host))
                return null;
            if (!Uri.TryCreate(host, UriKind.Absolute, out var uri) || !uri.IsLoopback)
                return null;
            return TryGetExplicitPort(host!);
        }

        /// <summary>
        /// Read an EXPLICITLY-typed port out of a host string, or <c>null</c> when there is none. Deliberately
        /// parses the RAW string rather than reading <c>Uri.Port</c>: <c>Uri.Port</c> synthesises the scheme
        /// default (80/443) for a port-less host, which would make "the user typed no port" indistinguishable from
        /// "the user typed 80" and silently bind 80 instead of the derived per-project port.
        ///
        /// <para>Mirrors <c>AgentConfiguratorSettings.TryGetExplicitPort</c> (McpPlugin #174) step for step —
        /// including the <c>&gt; 0 &amp;&amp; &lt;= Consts.Hub.MaxPort</c> guard, so an unusable port falls back to
        /// the derived one on BOTH sides rather than diverging. It is duplicated here rather than called because
        /// the LIB member is <c>internal</c> and ships in 7.3.0, while this binder must hold on the 7.2.0 pin.</para>
        /// </summary>
        internal static int? TryGetExplicitPort(string? host)
        {
            if (string.IsNullOrWhiteSpace(host))
                return null;

            // Isolate the authority: after "scheme://" (if present), up to the first '/', '?' or '#'.
            var schemeEnd = host!.IndexOf("://", StringComparison.Ordinal);
            var start = schemeEnd >= 0 ? schemeEnd + 3 : 0;
            var end = host.IndexOfAny(AuthorityTerminators, start);
            var authority = end >= 0 ? host.Substring(start, end - start) : host.Substring(start);

            // Drop any userinfo ("user:pass@host:port") — its colon is not a port separator.
            var at = authority.LastIndexOf('@');
            if (at >= 0)
                authority = authority.Substring(at + 1);

            // For an IPv6 literal the port follows the closing bracket ("[::1]:8080"); the colons inside the
            // brackets are part of the address. LastIndexOf returns -1 for a normal host, so the search then
            // starts at 0 and finds a plain "host:port" colon.
            var colon = authority.IndexOf(':', authority.LastIndexOf(']') + 1);
            if (colon < 0 || colon == authority.Length - 1)
                return null;

            // NumberStyles.None is the whole validator: it rejects sign, whitespace, separators and any
            // non-ASCII-digit character, so a hand-rolled digit scan on top would be redundant.
            if (!int.TryParse(authority.Substring(colon + 1), NumberStyles.None, CultureInfo.InvariantCulture, out var port))
                return null;

            return port > 0 && port <= Consts.Hub.MaxPort ? port : (int?)null;
        }

        /// <summary>The characters that terminate a URL's authority component (path / query / fragment).</summary>
        private static readonly char[] AuthorityTerminators = { '/', '?', '#' };

        /// <summary>
        /// The human-facing project name: the basename (no extension) of the first top-level <c>*.uproject</c>
        /// in <paramref name="projectRoot"/> (matches <c>FApp::GetProjectName()</c>), falling back to the
        /// trimmed directory name when no <c>.uproject</c> is present (or the path cannot be read). Never throws.
        /// </summary>
        public static string ResolveProjectName(string projectRoot)
        {
            if (string.IsNullOrWhiteSpace(projectRoot))
                return string.Empty;

            try
            {
                if (Directory.Exists(projectRoot))
                {
                    var uproject = Directory
                        .EnumerateFiles(projectRoot, "*.uproject", SearchOption.TopDirectoryOnly)
                        .FirstOrDefault();
                    if (!string.IsNullOrEmpty(uproject))
                        return Path.GetFileNameWithoutExtension(uproject);
                }
            }
            catch
            {
                // Fall through to the directory-name fallback — a name is best-effort (the routing key is the hash).
            }

            var trimmed = projectRoot.TrimEnd('/', '\\');
            var name = Path.GetFileName(trimmed);
            return string.IsNullOrEmpty(name) ? trimmed : name;
        }

        /// <summary>
        /// Read the project marker for <paramref name="projectRoot"/>, returning <c>null</c> when it is absent or
        /// unreadable (a project that has never been enrolled/configured, or a malformed file — never fatal to a
        /// connect). Wraps <see cref="ProjectMarker.Read"/> so a resolution never throws on marker IO.
        /// </summary>
        public static ProjectMarker? SafeReadMarker(string projectRoot)
        {
            if (string.IsNullOrWhiteSpace(projectRoot))
                return null;
            try
            {
                return ProjectMarker.Read(projectRoot);
            }
            catch
            {
                return null;
            }
        }

        /// <summary>
        /// Persist the enrolled <paramref name="serverTarget"/> into the committable project marker
        /// <c>&lt;project&gt;/.ai-game-dev/project.json</c>, preserving any existing port override. Read-modify-write
        /// and idempotent: a no-op returning <c>false</c> when the marker already records the same target (no file
        /// churn), else it writes and returns <c>true</c>. Non-secret — credentials NEVER land in the marker (design 06).
        /// The write is the counterpart of <see cref="SafeReadMarker"/>: a target written here resolves back through
        /// <see cref="Resolve"/>'s <see cref="ProjectConnection.ServerTarget"/>.
        /// </summary>
        public static bool PersistServerTarget(string projectRoot, string? serverTarget)
        {
            if (string.IsNullOrWhiteSpace(projectRoot) || string.IsNullOrWhiteSpace(serverTarget))
                return false;

            var marker = SafeReadMarker(projectRoot) ?? new ProjectMarker();
            if (string.Equals(marker.ServerTarget, serverTarget, StringComparison.Ordinal))
                return false;

            marker.ServerTarget = serverTarget;
            marker.Write(projectRoot);
            return true;
        }
    }
}
