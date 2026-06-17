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
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.Common.Model;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Specs for the connected-AI-agent roster (issue #109): before this, <c>StatusMessage.AiAgents</c> was
    /// hardcoded empty and <c>status</c> was re-pushed only on the editor's own connect/auth transitions, so an
    /// external agent joining never reached the editor. These lock the four behaviors the fix adds — (1) AiAgents
    /// populated from a roster, (2) empty when none, (3) the bridge's own self-entry excluded, (4) a fresh status
    /// re-emitted on an <c>OnClientsChanged</c> event — plus the connect-time seed (with retry/backoff). The pure
    /// label projection is tested directly; the wiring is driven through fakes (no live SignalR / editor).
    /// </summary>
    public class SidecarHostRosterTests
    {
        private static SidecarHost NewHost(out IpcClient ipc)
        {
            // Port is never dialed — the fake plugin replaces the real SignalR client, so no socket opens.
            ipc = new IpcClient("127.0.0.1", 39997, token: "ipc-token", sidecarVersion: "0.1.0");
            return new SidecarHost(ipc, "0.1.0");
        }

        private static McpClientData Agent(string name, string version = "1.0.0", bool connected = true) =>
            new() { IsConnected = connected, ClientName = name, ClientVersion = version };

        // ── (1)+(2)+(3) the pure projection: filter IsConnected, exclude self, Unity label format ────────────

        [Fact]
        public void BuildAgentLabels_PopulatesFromRoster_WithUnityLabelFormat()
        {
            var labels = SidecarHost.BuildAgentLabels(new[] { Agent("Claude", "1.2.3"), Agent("Cursor", "0.42") });

            Assert.Equal(
                new[] { "AI agent: Claude (1.2.3)", "AI agent: Cursor (0.42)" },
                labels);
        }

        [Fact]
        public void BuildAgentLabels_EmptyWhenNoConnectedClients()
        {
            Assert.Empty(SidecarHost.BuildAgentLabels(null));
            Assert.Empty(SidecarHost.BuildAgentLabels(new List<McpClientData>()));
            // A roster of only-disconnected entries projects to empty.
            Assert.Empty(SidecarHost.BuildAgentLabels(new[] { Agent("Claude", connected: false) }));
        }

        [Fact]
        public void BuildAgentLabels_ExcludesTheBridgeSelfEntry()
        {
            var labels = SidecarHost.BuildAgentLabels(new[]
            {
                Agent("Unreal-MCP-Bridge", "0.1.0"), // the plugin's own SignalR client label — must be dropped
                Agent("Claude", "1.0.0"),
            });

            Assert.Equal(new[] { "AI agent: Claude (1.0.0)" }, labels);
        }

        // ── (1)+(4) OnClientsChanged caches the roster AND re-emits a fresh status carrying it ────────────────

        [Fact]
        public void OnClientsChanged_CachesRoster_AndReEmitsStatusWithAiAgents()
        {
            using var host = NewHost(out _);
            var emitted = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { emitted.Add(s); return Task.CompletedTask; });

            var manager = new FakeMcpManager();
            var fake = new FakeMcpPlugin(onConnect: _ => Task.FromResult(true), mcpManager: manager);
            host.SubscribeToClientRoster(fake);

            // An agent joins → OnClientsChanged fires with the full active-client list.
            manager.PushClients(new[] { Agent("Claude", "1.0.0") });

            // The cache reflects the join, and a FRESH status was emitted carrying the roster (the live-refresh path).
            Assert.Equal(new[] { "AI agent: Claude (1.0.0)" }, host.ConnectedAgentsSnapshot);
            Assert.Single(emitted);
            Assert.Equal(new[] { "AI agent: Claude (1.0.0)" }, emitted[^1].AiAgents);

            // The agent leaves → a second status with an empty roster (live clear on disconnect).
            manager.PushClients(new List<McpClientData>());
            Assert.Empty(host.ConnectedAgentsSnapshot);
            Assert.Equal(2, emitted.Count);
            Assert.Empty(emitted[^1].AiAgents);
        }

        // ── connect-time seed via GetMcpClientData(), with retry/backoff when the agent session lags ─────────

        [Fact]
        public async Task SeedRosterAsync_SeedsFromGetMcpClientData_AndPopulatesAiAgents()
        {
            using var host = NewHost(out _);
            var emitted = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { emitted.Add(s); return Task.CompletedTask; });

            var hub = new FakeMcpManagerHub(new[] { Agent("Claude", "1.0.0") });
            var fake = new FakeMcpPlugin(onConnect: _ => Task.FromResult(true), mcpManagerHub: hub);
            host.SetPluginForTest(fake);

            await host.SeedRosterAsync(CancellationToken.None);

            Assert.Equal(1, hub.GetMcpClientDataCalls); // an agent was present on the first pull → no retry
            Assert.Equal(new[] { "AI agent: Claude (1.0.0)" }, host.ConnectedAgentsSnapshot);
            Assert.Equal(new[] { "AI agent: Claude (1.0.0)" }, emitted[^1].AiAgents);
        }

        [Fact]
        public async Task SeedRosterAsync_RetriesWhenNoAgentYet_ThenPicksUpTheLateJoiner()
        {
            using var host = NewHost(out _);
            var emitted = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { emitted.Add(s); return Task.CompletedTask; });

            // First pull empty (agent session lags the plugin connect), second pull has the agent — Unity's repro.
            var hub = new FakeMcpManagerHub(System.Array.Empty<McpClientData>(), new[] { Agent("Claude", "1.0.0") });
            var fake = new FakeMcpPlugin(onConnect: _ => Task.FromResult(true), mcpManagerHub: hub);
            host.SetPluginForTest(fake);
            host.RosterSeedRetryDelayMs = 0; // no real wait in the test

            await host.SeedRosterAsync(CancellationToken.None);

            Assert.True(hub.GetMcpClientDataCalls >= 2); // retried past the empty first pull
            Assert.Equal(new[] { "AI agent: Claude (1.0.0)" }, host.ConnectedAgentsSnapshot);
        }
    }
}
