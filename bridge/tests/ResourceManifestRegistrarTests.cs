/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak)              │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Linq;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>Resource manifest application + the §A.1 revision guard + the §1.5 reconnect reset (resource sibling).</summary>
    public class ResourceManifestRegistrarTests
    {
        private static ResourceDescriptor Resource(string uri, string hash, bool enabled = true) =>
            new() { Uri = uri, Name = uri, SchemaHash = hash, MimeType = "application/json", Enabled = enabled };

        private static ResourceManifestMessage Manifest(int revision, params ResourceDescriptor[] resources) =>
            new() { Revision = revision, Resources = resources.ToList() };

        private static (ResourceManifestRegistrar reg, FakeResourceSink sink) New()
        {
            var sink = new FakeResourceSink();
            var reg = new ResourceManifestRegistrar(sink, new FakeResourceCallChannel());
            return (reg, sink);
        }

        [Fact]
        public void Apply_RegistersAddedResourcesAsProxies()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Resource("unreal://project/levels", "h1"), Resource("unreal://project/icon", "h2")));
            Assert.True(sink.HasResource("unreal://project/levels"));
            Assert.True(sink.HasResource("unreal://project/icon"));
            Assert.Equal(2, sink.Resources.Count);
        }

        [Fact]
        public void Apply_RemovesDroppedResources()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Resource("unreal://project/levels", "h1"), Resource("unreal://doomed", "h2")));
            reg.Apply(Manifest(2, Resource("unreal://project/levels", "h1")));
            Assert.True(sink.HasResource("unreal://project/levels"));
            Assert.False(sink.HasResource("unreal://doomed"));
        }

        [Fact]
        public void Apply_ReregistersChangedResource()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Resource("unreal://project/levels", "h1")));
            var first = sink.Resources["unreal://project/levels"];
            reg.Apply(Manifest(2, Resource("unreal://project/levels", "h2")));
            Assert.NotSame(first, sink.Resources["unreal://project/levels"]); // remove+add produced a fresh proxy
        }

        [Fact]
        public void Apply_TogglesEnabledOnly()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Resource("unreal://project/levels", "h1", enabled: true)));
            reg.Apply(Manifest(2, Resource("unreal://project/levels", "h1", enabled: false)));
            Assert.True(sink.HasResource("unreal://project/levels"));
            Assert.False(sink.Enabled["unreal://project/levels"]);
        }

        [Fact]
        public void Apply_IgnoresStaleOrEqualRevision()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(5, Resource("unreal://project/levels", "h1")));
            var diff = reg.Apply(Manifest(5, Resource("unreal://project/levels", "h1"), Resource("unreal://late", "h2")));
            Assert.True(diff.IsEmpty);          // revision 5 <= last applied 5 → ignored
            Assert.False(sink.HasResource("unreal://late"));
        }

        [Fact]
        public void ResetForReconnect_AllowsUnchangedRevisionReapply()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(3, Resource("unreal://project/levels", "h1")));
            Assert.Equal(3, reg.LastAppliedRevision);

            reg.ResetForReconnect();
            Assert.Equal(-1, reg.LastAppliedRevision);

            var diff = reg.Apply(Manifest(3, Resource("unreal://project/levels", "h1"), Resource("unreal://added-offline", "h2")));
            Assert.False(diff.IsEmpty);
            Assert.True(sink.HasResource("unreal://added-offline"));
            Assert.Equal(3, reg.LastAppliedRevision);
        }

        [Fact]
        public void AppliedResourceUris_TracksRegistrations()
        {
            var (reg, _) = New();
            reg.Apply(Manifest(1, Resource("unreal://a", "h1"), Resource("unreal://b", "h2")));
            Assert.Equal(new[] { "unreal://a", "unreal://b" }, reg.AppliedResourceUris.OrderBy(u => u));
        }

        [Fact]
        public void ApplyManifest_TypeErasedRoute_RegistersResources()
        {
            // The IpcClient holds the registrar as IManifestSink<ResourceManifestMessage>; exercise that route.
            var (reg, sink) = New();
            IManifestSink<ResourceManifestMessage> sinkRoute = reg;
            sinkRoute.ApplyManifest(Manifest(1, Resource("unreal://project/levels", "h1")));
            Assert.True(sink.HasResource("unreal://project/levels"));
        }
    }
}
