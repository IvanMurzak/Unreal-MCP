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
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>Manifest application + the §2.2 revision guard + the §1.5 reconnect reset.</summary>
    public class ManifestRegistrarTests
    {
        private static ToolDescriptor Tool(string name, string hash, bool enabled = true) =>
            new() { Name = name, SchemaHash = hash, Enabled = enabled };

        private static ToolManifestMessage Manifest(int revision, params ToolDescriptor[] tools) =>
            new() { Revision = revision, Tools = tools.ToList() };

        private static (ManifestRegistrar reg, FakeToolSink sink) New()
        {
            var sink = new FakeToolSink();
            var reg = new ManifestRegistrar(sink, new FakeToolCallChannel());
            return (reg, sink);
        }

        [Fact]
        public void Apply_RegistersAddedToolsAsProxies()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Tool("ping", "h1"), Tool("actor-create", "h2")));
            Assert.True(sink.HasTool("ping"));
            Assert.True(sink.HasTool("actor-create"));
            Assert.Equal(2, sink.Tools.Count);
        }

        [Fact]
        public void Apply_RemovesDroppedTools()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Tool("ping", "h1"), Tool("doomed", "h2")));
            reg.Apply(Manifest(2, Tool("ping", "h1")));
            Assert.True(sink.HasTool("ping"));
            Assert.False(sink.HasTool("doomed"));
        }

        [Fact]
        public void Apply_ReregistersChangedTool()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Tool("ping", "h1")));
            var first = sink.Tools["ping"];
            reg.Apply(Manifest(2, Tool("ping", "h2")));
            Assert.NotSame(first, sink.Tools["ping"]); // remove+add produced a fresh proxy
        }

        [Fact]
        public void Apply_TogglesEnabledOnly()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Tool("ping", "h1", enabled: true)));
            reg.Apply(Manifest(2, Tool("ping", "h1", enabled: false)));
            Assert.True(sink.HasTool("ping"));
            Assert.False(sink.Enabled["ping"]);
        }

        [Fact]
        public void Apply_IgnoresStaleOrEqualRevision()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(5, Tool("ping", "h1")));
            var diff = reg.Apply(Manifest(5, Tool("ping", "h1"), Tool("late", "h2")));
            Assert.True(diff.IsEmpty);          // revision 5 <= last applied 5 → ignored
            Assert.False(sink.HasTool("late"));
        }

        [Fact]
        public void ResetForReconnect_AllowsUnchangedRevisionReapply()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(3, Tool("ping", "h1")));
            Assert.Equal(3, reg.LastAppliedRevision);

            // Simulate reconnect: the plugin re-pushes the SAME revision after a handshake-ack.
            reg.ResetForReconnect();
            Assert.Equal(-1, reg.LastAppliedRevision);

            // Re-push with an unchanged revision is no longer skipped (§1.5). The snapshot is retained,
            // so a byte-identical manifest diffs to a no-op (the proxy is still registered) — but a tool
            // ADDED while the link was down is now caught.
            var diff = reg.Apply(Manifest(3, Tool("ping", "h1"), Tool("added-offline", "h2")));
            Assert.False(diff.IsEmpty);
            Assert.True(sink.HasTool("added-offline"));
            Assert.Equal(3, reg.LastAppliedRevision);
        }

        [Fact]
        public void AppliedToolNames_TracksRegistrations()
        {
            var (reg, _) = New();
            reg.Apply(Manifest(1, Tool("a", "h1"), Tool("b", "h2")));
            Assert.Equal(new[] { "a", "b" }, reg.AppliedToolNames.OrderBy(n => n));
        }
    }
}
