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
    /// <summary>Prompt manifest application + the §A.1 revision guard + the §1.5 reconnect reset (prompt sibling).</summary>
    public class PromptManifestRegistrarTests
    {
        private static PromptDescriptor Prompt(string name, string hash, bool enabled = true) =>
            new() { Name = name, SchemaHash = hash, Role = "user", Enabled = enabled };

        private static PromptManifestMessage Manifest(int revision, params PromptDescriptor[] prompts) =>
            new() { Revision = revision, Prompts = prompts.ToList() };

        private static (PromptManifestRegistrar reg, FakePromptSink sink) New()
        {
            var sink = new FakePromptSink();
            var reg = new PromptManifestRegistrar(sink, new FakePromptCallChannel());
            return (reg, sink);
        }

        [Fact]
        public void Apply_RegistersAddedPromptsAsProxies()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Prompt("level-design-brief", "h1"), Prompt("enemy-brief", "h2")));
            Assert.True(sink.HasPrompt("level-design-brief"));
            Assert.True(sink.HasPrompt("enemy-brief"));
            Assert.Equal(2, sink.Prompts.Count);
        }

        [Fact]
        public void Apply_RemovesDroppedPrompts()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Prompt("level-design-brief", "h1"), Prompt("doomed", "h2")));
            reg.Apply(Manifest(2, Prompt("level-design-brief", "h1")));
            Assert.True(sink.HasPrompt("level-design-brief"));
            Assert.False(sink.HasPrompt("doomed"));
        }

        [Fact]
        public void Apply_ReregistersChangedPrompt()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Prompt("level-design-brief", "h1")));
            var first = sink.Prompts["level-design-brief"];
            reg.Apply(Manifest(2, Prompt("level-design-brief", "h2")));
            Assert.NotSame(first, sink.Prompts["level-design-brief"]); // remove+add produced a fresh proxy
        }

        [Fact]
        public void Apply_TogglesEnabledOnly()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(1, Prompt("level-design-brief", "h1", enabled: true)));
            reg.Apply(Manifest(2, Prompt("level-design-brief", "h1", enabled: false)));
            Assert.True(sink.HasPrompt("level-design-brief"));
            Assert.False(sink.Enabled["level-design-brief"]);
        }

        [Fact]
        public void Apply_IgnoresStaleOrEqualRevision()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(5, Prompt("level-design-brief", "h1")));
            var diff = reg.Apply(Manifest(5, Prompt("level-design-brief", "h1"), Prompt("late", "h2")));
            Assert.True(diff.IsEmpty);          // revision 5 <= last applied 5 → ignored
            Assert.False(sink.HasPrompt("late"));
        }

        [Fact]
        public void ResetForReconnect_AllowsUnchangedRevisionReapply()
        {
            var (reg, sink) = New();
            reg.Apply(Manifest(3, Prompt("level-design-brief", "h1")));
            Assert.Equal(3, reg.LastAppliedRevision);

            reg.ResetForReconnect();
            Assert.Equal(-1, reg.LastAppliedRevision);

            var diff = reg.Apply(Manifest(3, Prompt("level-design-brief", "h1"), Prompt("added-offline", "h2")));
            Assert.False(diff.IsEmpty);
            Assert.True(sink.HasPrompt("added-offline"));
            Assert.Equal(3, reg.LastAppliedRevision);
        }

        [Fact]
        public void AppliedPromptNames_TracksRegistrations()
        {
            var (reg, _) = New();
            reg.Apply(Manifest(1, Prompt("a", "h1"), Prompt("b", "h2")));
            Assert.Equal(new[] { "a", "b" }, reg.AppliedPromptNames.OrderBy(n => n));
        }

        [Fact]
        public void ApplyManifest_TypeErasedRoute_RegistersPrompts()
        {
            // The IpcClient holds the registrar as IManifestSink<PromptManifestMessage>; exercise that route.
            var (reg, sink) = New();
            IManifestSink<PromptManifestMessage> sinkRoute = reg;
            sinkRoute.ApplyManifest(Manifest(1, Prompt("level-design-brief", "h1")));
            Assert.True(sink.HasPrompt("level-design-brief"));
        }
    }
}
