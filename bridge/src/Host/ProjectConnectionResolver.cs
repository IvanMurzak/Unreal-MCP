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

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// The resolved connection identity of the editor's project (mcp-authorize PR 3, design docs 04/06):
    /// the routing <see cref="Pin"/> + deterministic local <see cref="Port"/> (McpPlugin
    /// <see cref="ProjectIdentity"/>), the enrolled <see cref="ServerTarget"/> and the full
    /// <see cref="ProjectPathHash"/> read/derived from the committable project marker + project root, plus the
    /// ready-to-attach <see cref="Metadata"/> handshake payload. Every field is derived deterministically —
    /// no live socket, no shared state.
    /// </summary>
    internal sealed record ProjectConnection(
        string Pin,
        int Port,
        bool PortIsOverridden,
        string? ServerTarget,
        string ProjectPathHash,
        string ProjectName,
        ConnectionInstanceMetadata Metadata);

    /// <summary>
    /// Resolves the sidecar's <b>connection identity</b> for the editor's project (mcp-authorize PR 3,
    /// design docs 04/06). Given the project root the C++ plugin reports over IPC (the handshake-ack
    /// <c>projectPath</c> — <c>FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())</c>), it:
    /// <list type="bullet">
    ///   <item>reads the committable project marker <c>&lt;project&gt;/.ai-game-dev/project.json</c>
    ///         (<see cref="ProjectMarker"/>) for the user's optional port override + the enrolled server target;</item>
    ///   <item>derives the routing pin + deterministic local port via the single-source
    ///         <see cref="ProjectIdentity"/> — byte-for-byte the committed golden vectors (trim trailing
    ///         separators, <c>ToLowerInvariant</c> only, separators NOT converted);</item>
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
        /// marker (may be absent) for the port override + server target, derives the pin/port, and builds the
        /// instance-metadata handshake payload. <paramref name="instanceId"/> is the per-editor-session GUID
        /// (stable across reconnects); <paramref name="machineName"/> defaults to the host machine name.
        /// </summary>
        public static ProjectConnection Resolve(string projectRoot, string instanceId, string? machineName = null)
        {
            if (projectRoot == null)
                throw new ArgumentNullException(nameof(projectRoot));
            if (string.IsNullOrEmpty(instanceId))
                throw new ArgumentException("InstanceId must be non-empty.", nameof(instanceId));

            var marker = SafeReadMarker(projectRoot);
            var identity = ProjectIdentity.Derive(projectRoot, marker);
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
                Port: identity.Port,
                PortIsOverridden: identity.PortIsOverridden,
                ServerTarget: marker?.ServerTarget,
                ProjectPathHash: metadata.ProjectPathHash,
                ProjectName: projectName,
                Metadata: metadata);
        }

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
