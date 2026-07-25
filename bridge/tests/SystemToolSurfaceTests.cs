/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak)               │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;
// McpPlugin ships its own ProxyTool/ProxyToolFactory, which collide with the bridge's local mirrors under
// this file's two usings (see the same alias in Fakes.cs). Pin both bare names to the LOCAL types.
using ProxyTool = com.IvanMurzak.Unreal.MCP.Bridge.Tools.ProxyTool;
using ProxyToolFactory = com.IvanMurzak.Unreal.MCP.Bridge.Tools.ProxyToolFactory;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// The §2.4 STANDARD-vs-SYSTEM tool surface: descriptor parsing, proxy tagging, sink routing, and the
    /// end-to-end manifest path. The load-bearing assertion for the whole task is
    /// <see cref="Manifest_PutsPingOnTheSystemSurfaceOnly"/> — it fails the moment `ping` regresses back to the
    /// standard surface.
    /// </summary>
    public class SystemToolSurfaceTests
    {
        private static ToolDescriptor Tool(string name, string hash, string? toolType = null, bool enabled = true) =>
            new() { Name = name, SchemaHash = hash, ToolType = toolType, Enabled = enabled };

        private static ToolManifestMessage Manifest(int revision, params ToolDescriptor[] tools) =>
            new() { Revision = revision, Tools = tools.ToList() };

        // --- ToolDescriptor.IsSystem -------------------------------------------------------------------

        [Theory]
        [InlineData("system", true)]
        [InlineData("System", true)]
        [InlineData("SYSTEM", true)]
        [InlineData("standard", false)]
        [InlineData("", false)]
        [InlineData(null, false)]
        [InlineData("sytsem", false)]  // a typo must NOT hide the tool from every agent
        public void IsSystem_OptsInOnlyOnTheExactToken(string? toolType, bool expected)
        {
            Assert.Equal(expected, Tool("t", "h", toolType).IsSystem);
        }

        [Fact]
        public void ToolType_DeserializesFromTheManifestWireField()
        {
            const string json = """{"name":"ping","toolType":"system","schemaHash":"h"}""";
            var descriptor = JsonSerializer.Deserialize<ToolDescriptor>(json);
            Assert.NotNull(descriptor);
            Assert.Equal("system", descriptor!.ToolType);
            Assert.True(descriptor.IsSystem);
        }

        [Fact]
        public void StructuralSignature_ChangesWithTheSurface()
        {
            // Without this, a tool that moves between surfaces would diff as unchanged and keep serving from the
            // manager it was first registered on.
            Assert.NotEqual(
                Tool("ping", "h").StructuralSignature,
                Tool("ping", "h", "system").StructuralSignature);
        }

        // --- ProxyToolFactory --------------------------------------------------------------------------

        [Fact]
        public void ProxyToolFactory_TagsTheProxyWithTheDeclaredSurface()
        {
            Assert.Equal(McpToolType.System,
                ProxyToolFactory.Create(Tool("ping", "h", "system"), new FakeToolCallChannel()).ToolType);
            Assert.Equal(McpToolType.Standard,
                ProxyToolFactory.Create(Tool("actor-create", "h"), new FakeToolCallChannel()).ToolType);
        }

        // --- SystemToolSink ----------------------------------------------------------------------------

        [Fact]
        public void SystemToolSink_AddsRemovesAndTogglesInTheBackingCollection()
        {
            var runners = new Dictionary<string, IRunTool>();
            var sink = new SystemToolSink(runners);
            var proxy = ProxyToolFactory.Create(Tool("ping", "h", "system"), new FakeToolCallChannel());

            Assert.True(sink.AddTool("ping", proxy));
            Assert.True(sink.HasTool("ping"));
            Assert.Same(proxy, runners["ping"]);

            Assert.True(sink.SetToolEnabled("ping", false));
            Assert.False(proxy.Enabled);

            Assert.True(sink.RemoveTool("ping"));
            Assert.False(sink.HasTool("ping"));
            Assert.Empty(runners);
        }

        [Fact]
        public void SystemToolSink_AddIsLastWriteWins()
        {
            // Mirrors SystemToolRunnerCollection's own indexer semantics, and the registrar's remove-then-add.
            var runners = new Dictionary<string, IRunTool>();
            var sink = new SystemToolSink(runners);
            sink.AddTool("ping", ProxyToolFactory.Create(Tool("ping", "h1", "system"), new FakeToolCallChannel()));
            var second = ProxyToolFactory.Create(Tool("ping", "h2", "system"), new FakeToolCallChannel());

            Assert.True(sink.AddTool("ping", second));
            Assert.Same(second, runners["ping"]);
        }

        // --- SurfaceRoutingToolSink --------------------------------------------------------------------

        private static (SurfaceRoutingToolSink sink, FakeToolSink standard, FakeToolSink system) Routing()
        {
            var standard = new FakeToolSink();
            var system = new FakeToolSink();
            return (new SurfaceRoutingToolSink(standard, system), standard, system);
        }

        [Fact]
        public void Routing_SendsEachProxyToItsDeclaredSurface()
        {
            var (sink, standard, system) = Routing();
            sink.AddTool("ping", ProxyToolFactory.Create(Tool("ping", "h", "system"), new FakeToolCallChannel()));
            sink.AddTool("actor-create", ProxyToolFactory.Create(Tool("actor-create", "h"), new FakeToolCallChannel()));

            Assert.True(system.HasTool("ping"));
            Assert.False(standard.HasTool("ping"));
            Assert.True(standard.HasTool("actor-create"));
            Assert.False(system.HasTool("actor-create"));
            Assert.Equal(McpToolType.System, sink.SurfaceOf("ping"));
            Assert.Equal(McpToolType.Standard, sink.SurfaceOf("actor-create"));
        }

        [Fact]
        public void Routing_RemovesFromTheSurfaceTheToolWasRegisteredOn()
        {
            var (sink, standard, system) = Routing();
            sink.AddTool("ping", ProxyToolFactory.Create(Tool("ping", "h", "system"), new FakeToolCallChannel()));

            Assert.True(sink.RemoveTool("ping"));
            Assert.False(system.HasTool("ping"));
            Assert.False(standard.HasTool("ping"));
            Assert.Null(sink.SurfaceOf("ping"));
        }

        [Fact]
        public void Routing_TogglesEnabledOnTheRecordedSurface()
        {
            var (sink, standard, system) = Routing();
            sink.AddTool("ping", ProxyToolFactory.Create(Tool("ping", "h", "system"), new FakeToolCallChannel()));

            Assert.True(sink.SetToolEnabled("ping", false));
            Assert.False(system.Enabled["ping"]);
            Assert.False(standard.Enabled.ContainsKey("ping"));
        }

        [Fact]
        public void Routing_UnknownNameFallsBackToBothSurfaces()
        {
            // Defensive path: bookkeeping can only ever leak, never strand a proxy on the wrong manager.
            var (sink, standard, system) = Routing();
            standard.AddTool("orphan", ProxyToolFactory.Create(Tool("orphan", "h"), new FakeToolCallChannel()));

            Assert.True(sink.RemoveTool("orphan"));
            Assert.False(standard.HasTool("orphan"));
        }

        // --- End-to-end through the registrar ----------------------------------------------------------

        [Fact]
        public void Manifest_PutsPingOnTheSystemSurfaceOnly()
        {
            // THE regression guard: `ping` is a system tool (owner ruling 2026-07-25). If it ever comes back as a
            // standard tool, it lands on the standard sink and this fails.
            var (sink, standard, system) = Routing();
            var registrar = new ManifestRegistrar(sink, new FakeToolCallChannel());

            registrar.Apply(Manifest(1,
                Tool("ping", "h1", "system"),
                Tool("unreal-skill-create", "h2", "system"),
                Tool("actor-create", "h3")));

            Assert.True(system.HasTool("ping"));
            Assert.False(standard.HasTool("ping"));
            Assert.True(system.HasTool("unreal-skill-create"));
            Assert.False(standard.HasTool("unreal-skill-create"));
            Assert.True(standard.HasTool("actor-create"));
            Assert.False(system.HasTool("actor-create"));
        }

        [Fact]
        public void Manifest_MovesAToolWhenItsSurfaceChanges()
        {
            // The plugin hashes the WHOLE descriptor — `toolType` included — so a surface flip always arrives
            // with a fresh schemaHash and diffs as CHANGED (remove-then-add). The remove must drop the proxy
            // from the manager it actually lived on, not blindly from the standard one.
            var (sink, standard, system) = Routing();
            var registrar = new ManifestRegistrar(sink, new FakeToolCallChannel());

            registrar.Apply(Manifest(1, Tool("ping", "h-standard")));
            Assert.True(standard.HasTool("ping"));

            registrar.Apply(Manifest(2, Tool("ping", "h-system", "system")));
            Assert.True(system.HasTool("ping"));
            Assert.False(standard.HasTool("ping"));
        }

        [Fact]
        public void Manifest_MovesAToolWhenTheSurfaceChangesWithNoSchemaHash()
        {
            // The hashless fallback path (ManifestDiffer.SchemaEquals): with no schemaHash on either side the
            // diff compares StructuralSignature, which is why the surface is part of that signature.
            var (sink, standard, system) = Routing();
            var registrar = new ManifestRegistrar(sink, new FakeToolCallChannel());

            registrar.Apply(Manifest(1, Tool("ping", hash: null!)));
            Assert.True(standard.HasTool("ping"));

            registrar.Apply(Manifest(2, Tool("ping", hash: null!, toolType: "system")));
            Assert.True(system.HasTool("ping"));
            Assert.False(standard.HasTool("ping"));
        }

        [Fact]
        public void Manifest_WithoutAToolTypeKeepsThePreSurfaceBehaviour()
        {
            // Back-compat: an older plugin sends no `toolType` at all — everything stays standard.
            var (sink, standard, system) = Routing();
            var registrar = new ManifestRegistrar(sink, new FakeToolCallChannel());

            registrar.Apply(Manifest(1, Tool("ping", "h1"), Tool("actor-create", "h2")));

            Assert.Equal(2, standard.Tools.Count);
            Assert.Empty(system.Tools);
        }
    }
}
