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
using System.Text.Json.Nodes;
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

        // ── (issue #111) dedupe by display identity: one client = one label, distinct identities NOT collapsed ──

        [Fact]
        public void BuildAgentLabels_DedupesIdenticalNameAndVersion_ToOneLabel()
        {
            // A single client (Claude Code) holds two live MCP sessions → the server returns two McpClientData with
            // identical ClientName/ClientVersion. The row must collapse them to exactly ONE label (issue #111 Bug 1).
            var labels = SidecarHost.BuildAgentLabels(new[]
            {
                Agent("Claude Code", "1.2.3"),
                Agent("Claude Code", "1.2.3"),
            });

            Assert.Equal(new[] { "AI agent: Claude Code (1.2.3)" }, labels);
        }

        [Fact]
        public void BuildAgentLabels_DoesNotCollapseDistinctNamesOrVersions()
        {
            // Distinct display identities must remain separate: a different name OR a different version is a
            // different entry — only an exact (name, version) match is a duplicate.
            var labels = SidecarHost.BuildAgentLabels(new[]
            {
                Agent("Claude Code", "1.2.3"),
                Agent("Claude Code", "1.2.4"), // same name, different version → kept
                Agent("Cursor", "1.2.3"),      // different name, same version → kept
            });

            Assert.Equal(
                new[]
                {
                    "AI agent: Claude Code (1.2.3)",
                    "AI agent: Claude Code (1.2.4)",
                    "AI agent: Cursor (1.2.3)",
                },
                labels);
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

        // ── (issue #111 Bug 1 regression) the connected live-refresh path emits the DEDUPED non-empty list ───

        [Fact]
        public void OnClientsChanged_WhileConnected_EmitsDedupedNonEmptyList_NoRegression()
        {
            using var host = NewHost(out _); // keepConnected defaults to true → "Connected" emits
            var emitted = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { emitted.Add(s); return Task.CompletedTask; });

            var manager = new FakeMcpManager();
            var fake = new FakeMcpPlugin(onConnect: _ => Task.FromResult(true), mcpManager: manager);
            host.SubscribeToClientRoster(fake);

            // Two sessions for the same client (the #111 duplicate) plus a distinct one → deduped to two labels,
            // and (no #110 regression) the connected path still ships the populated list live.
            manager.PushClients(new[]
            {
                Agent("Claude Code", "1.2.3"),
                Agent("Claude Code", "1.2.3"),
                Agent("Cursor", "0.42"),
            });

            Assert.Equal(new[] { "AI agent: Claude Code (1.2.3)", "AI agent: Cursor (0.42)" }, host.ConnectedAgentsSnapshot);
            Assert.Single(emitted);
            Assert.Equal("Connected", emitted[^1].ConnectionState);
            Assert.Equal(new[] { "AI agent: Claude Code (1.2.3)", "AI agent: Cursor (0.42)" }, emitted[^1].AiAgents);
        }

        // ── (issue #111 Bug 2) a non-"Connected" emit ships an EMPTY AiAgents list even with a primed cache ──

        [Fact]
        public void OnClientsChanged_WhileDisarmed_ShipsEmptyAiAgents_EvenThoughCacheIsPrimed()
        {
            using var host = NewHost(out _);
            // Disarm BEFORE the roster push so the OnClientsChanged handler emits "Disconnected" (non-connected).
            host.ApplyConnectionConfig(new JsonObject { ["mode"] = "Custom", ["host"] = "http://localhost:8500", ["keepConnected"] = false });

            var emitted = new List<StatusMessage>();
            host.SetStatusEmitterForTest(s => { emitted.Add(s); return Task.CompletedTask; });

            var manager = new FakeMcpManager();
            var fake = new FakeMcpPlugin(onConnect: _ => Task.FromResult(true), mcpManager: manager);
            host.SubscribeToClientRoster(fake);

            // The roster cache is primed (UpdateRoster runs regardless of connection state)...
            manager.PushClients(new[] { Agent("Claude Code", "1.2.3") });

            Assert.Equal(new[] { "AI agent: Claude Code (1.2.3)" }, host.ConnectedAgentsSnapshot); // cache populated
            Assert.Single(emitted);
            Assert.Equal("Disconnected", emitted[^1].ConnectionState);
            Assert.Empty(emitted[^1].AiAgents); // ...but a non-connected status ships EMPTY (the dot stays Offline)
        }

        // ── (issue #111 Bug 2) a user Disconnect clears the cache AND ships empty AiAgents ────────────────────

        [Fact]
        public async Task UserDisconnect_ClearsRosterCache_AndShipsEmptyAiAgents()
        {
            using var host = NewHost(out _);
            var manager = new FakeMcpManager();
            var fake = new FakeMcpPlugin(onConnect: _ => Task.FromResult(true), mcpManager: manager);
            host.SetPluginForTest(fake);
            host.SubscribeToClientRoster(fake);

            var disconnectEmitted = new TaskCompletionSource<StatusMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
            // Prime the cache while connected, then capture the "Disconnected" status the disconnect transition emits.
            host.SetStatusEmitterForTest(s =>
            {
                if (string.Equals(s.ConnectionState, "Disconnected", System.StringComparison.Ordinal))
                    disconnectEmitted.TrySetResult(s);
                return Task.CompletedTask;
            });
            manager.PushClients(new[] { Agent("Claude Code", "1.2.3") });
            Assert.Equal(new[] { "AI agent: Claude Code (1.2.3)" }, host.ConnectedAgentsSnapshot); // primed

            // A user Disconnect = keepConnected true→false config push → HandleDisconnectAsync runs (via the queue).
            host.OnConfigReceived(new JsonObject
            {
                ["type"] = "config",
                ["mode"] = "Custom",
                ["host"] = "http://localhost:8500",
                ["keepConnected"] = false,
            });

            var status = await disconnectEmitted.Task.WaitAsync(System.TimeSpan.FromSeconds(5));
            Assert.Empty(status.AiAgents);                 // the emitted Disconnected status carries no agents
            Assert.Empty(host.ConnectedAgentsSnapshot);    // and the cache itself was cleared (fresh re-seed on reconnect)
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
