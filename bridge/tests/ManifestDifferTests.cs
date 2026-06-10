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
    /// <summary>The "manifest diff" xUnit target (docs/ARCHITECTURE.md §9.3, §2.2).</summary>
    public class ManifestDifferTests
    {
        private static ToolDescriptor Tool(string name, string hash, bool enabled = true) =>
            new() { Name = name, SchemaHash = hash, Enabled = enabled };

        private static Dictionary<string, ToolDescriptor> Prev(params ToolDescriptor[] tools) =>
            tools.ToDictionary(t => t.Name);

        [Fact]
        public void Added_WhenNameNotInPrevious()
        {
            var diff = ManifestDiffer.Compute(Prev(), new[] { Tool("ping", "h1") });
            Assert.Single(diff.Added);
            Assert.Equal("ping", diff.Added[0].Name);
            Assert.Empty(diff.Changed);
            Assert.Empty(diff.Removed);
        }

        [Fact]
        public void Removed_WhenNameDropped()
        {
            var diff = ManifestDiffer.Compute(Prev(Tool("ping", "h1")), System.Array.Empty<ToolDescriptor>());
            Assert.Equal(new[] { "ping" }, diff.Removed);
        }

        [Fact]
        public void Changed_WhenSchemaHashDiffers()
        {
            var diff = ManifestDiffer.Compute(Prev(Tool("ping", "h1")), new[] { Tool("ping", "h2") });
            Assert.Single(diff.Changed);
            Assert.Empty(diff.EnabledChanged);
        }

        [Fact]
        public void EnabledChanged_WhenOnlyEnabledFlagDiffers()
        {
            var diff = ManifestDiffer.Compute(
                Prev(Tool("ping", "h1", enabled: true)),
                new[] { Tool("ping", "h1", enabled: false) });
            Assert.Empty(diff.Changed);
            Assert.Single(diff.EnabledChanged);
            Assert.Equal(("ping", false), diff.EnabledChanged[0]);
        }

        [Fact]
        public void NoOp_WhenIdentical()
        {
            var diff = ManifestDiffer.Compute(Prev(Tool("ping", "h1")), new[] { Tool("ping", "h1") });
            Assert.True(diff.IsEmpty);
        }

        [Fact]
        public void SignatureFallback_WhenHashesAbsent()
        {
            var a = new ToolDescriptor { Name = "t", Description = "old" };
            var b = new ToolDescriptor { Name = "t", Description = "new" };
            var diff = ManifestDiffer.Compute(Prev(a), new[] { b });
            Assert.Single(diff.Changed);
        }

        [Fact]
        public void MixedDiff_PartitionsCorrectly()
        {
            var previous = Prev(Tool("keep", "h1"), Tool("drop", "h2"), Tool("mutate", "h3"), Tool("toggle", "h4", true));
            var next = new[]
            {
                Tool("keep", "h1"),               // no-op
                Tool("mutate", "h3-new"),         // changed
                Tool("toggle", "h4", false),      // enabled-only
                Tool("brand-new", "h5"),          // added
                                                  // "drop" removed
            };
            var diff = ManifestDiffer.Compute(previous, next);
            Assert.Equal(new[] { "brand-new" }, diff.Added.Select(t => t.Name));
            Assert.Equal(new[] { "mutate" }, diff.Changed.Select(t => t.Name));
            Assert.Equal(new[] { "drop" }, diff.Removed);
            Assert.Equal(("toggle", false), Assert.Single(diff.EnabledChanged));
        }
    }
}
