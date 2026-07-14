/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the sidecar attaches the mcp-authorize PR 3 instance-metadata handshake payload to the SignalR
    /// hub connection when the plugin's handshake-ack reports a project path (design 04/06). Drives
    /// <see cref="SidecarHost.ApplyProjectIdentity"/> directly — the seam the handshake-ack routes through —
    /// so no live socket is needed (mirrors <c>ConnectionConfigTests</c>).
    /// </summary>
    public class SidecarHostInstanceMetadataTests
    {
        private static SidecarHost BuildHost()
        {
            var ipc = new IpcClient("127.0.0.1", 39997, token: "test-token", sidecarVersion: "0.1.0");
            var host = new SidecarHost(ipc, "0.1.0");
            host.Build();
            return host;
        }

        [Fact]
        public void ApplyProjectIdentity_SetsInstanceMetadata_FromReportedProjectPath()
        {
            const string projectPath = "/home/user/my-game";
            using var host = BuildHost();

            // Before a project path is known, the pre-b7 behaviour holds — no instance metadata attached.
            Assert.Null(host.InstanceMetadata);

            host.ApplyProjectIdentity(projectPath);

            var meta = host.InstanceMetadata;
            Assert.NotNull(meta);
            Assert.Equal("unreal", meta!.Engine);
            Assert.Equal(host.InstanceId, meta.InstanceId);
            Assert.Equal(ProjectIdentity.DeriveProjectPathHash(projectPath), meta.ProjectPathHash);
            // The connection layer anchors relative skills paths (disabled here) against the reported root.
            Assert.Equal(projectPath, host.Config.ProjectRootPath);
        }

        [Theory]
        [InlineData(null)]
        [InlineData("")]
        [InlineData("   ")]
        public void ApplyProjectIdentity_BlankPath_LeavesMetadataNull(string? projectPath)
        {
            using var host = BuildHost();
            host.ApplyProjectIdentity(projectPath);
            Assert.Null(host.InstanceMetadata);
        }

        [Fact]
        public void ApplyProjectIdentity_ReAppliedOnReconnect_KeepsSameInstanceId()
        {
            const string projectPath = "/home/user/my-game";
            using var host = BuildHost();

            host.ApplyProjectIdentity(projectPath);
            var firstInstanceId = host.InstanceMetadata!.InstanceId;

            // A re-handshake (IPC reconnect) re-resolves the identity — the per-editor-session id is stable.
            host.ApplyProjectIdentity(projectPath);
            Assert.Equal(firstInstanceId, host.InstanceMetadata!.InstanceId);
            Assert.Equal(host.InstanceId, firstInstanceId);
        }
    }
}
