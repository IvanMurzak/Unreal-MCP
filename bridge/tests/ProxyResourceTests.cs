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
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// The §A.1 resource proxy: content round-trip (text + base64 blob), the no-IPC static list synthesis, and
    /// the §2.2 step-4 disconnect fail-fast — the resource sibling of <see cref="ProxyPromptFactoryTests"/>.
    /// </summary>
    public class ProxyResourceFactoryTests
    {
        [Fact]
        public async Task Create_RoundTripsReadThroughChannel_MapsTextContent()
        {
            var channel = new FakeResourceCallChannel(uri => new ResourceResponseMessage
            {
                RequestId = "x",
                Status = IpcProtocol.Status.Success,
                Contents = new List<ResourceContentEntry>
                {
                    new() { Uri = uri, MimeType = "application/json", Text = "{\"hasWorld\":true}" },
                },
            });

            var proxy = ProxyResourceFactory.Create(
                new ResourceDescriptor { Uri = "unreal://project/levels", Name = "Project Levels", MimeType = "application/json" },
                channel);

            var contents = await proxy.RunGetContent.Run(new object?[0]);

            Assert.Equal("unreal://project/levels", channel.LastUri);
            Assert.Single(contents);
            Assert.Equal("unreal://project/levels", contents[0].Uri);
            Assert.Equal("application/json", contents[0].MimeType);
            Assert.Equal("{\"hasWorld\":true}", contents[0].Text);
            Assert.Null(contents[0].Blob);
        }

        [Fact]
        public async Task Create_RoundTripsRead_MapsBase64BlobContent()
        {
            // A base64 blob (binary content) must round-trip as a Blob block, NOT a Text block — the Text-vs-Blob
            // discrimination the brief calls out. Encode a known PNG-ish byte sequence so the assertion is exact.
            var rawBytes = new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
            var base64 = Convert.ToBase64String(rawBytes);

            var channel = new FakeResourceCallChannel(uri => new ResourceResponseMessage
            {
                RequestId = "x",
                Status = IpcProtocol.Status.Success,
                Contents = new List<ResourceContentEntry>
                {
                    new() { Uri = uri, MimeType = "image/png", Blob = base64 },
                },
            });

            var proxy = ProxyResourceFactory.Create(
                new ResourceDescriptor { Uri = "unreal://project/icon", Name = "Project Icon", MimeType = "image/png" },
                channel);

            var contents = await proxy.RunGetContent.Run(new object?[0]);

            Assert.Single(contents);
            Assert.Equal("image/png", contents[0].MimeType);
            Assert.Equal(base64, contents[0].Blob);
            Assert.Null(contents[0].Text);
            // The blob decodes back to the original bytes — proves the binary survived the round-trip intact.
            Assert.Equal(rawBytes, Convert.FromBase64String(contents[0].Blob!));
        }

        [Fact]
        public async Task Create_ErrorStatus_ThrowsSoManagerFailsFast()
        {
            var channel = new FakeResourceCallChannel(uri => new ResourceResponseMessage
            {
                RequestId = "x",
                Status = IpcProtocol.Status.Error,
                Error = "Unknown resource 'unreal://nope'.",
            });

            var proxy = ProxyResourceFactory.Create(new ResourceDescriptor { Uri = "unreal://nope" }, channel);

            var ex = await Assert.ThrowsAsync<InvalidOperationException>(() => proxy.RunGetContent.Run(new object?[0]));
            Assert.Contains("Unknown resource", ex.Message);
        }

        [Fact]
        public async Task Create_DisconnectedChannel_FailsFast()
        {
            var channel = new FakeResourceCallChannel { Connected = false };
            var proxy = ProxyResourceFactory.Create(new ResourceDescriptor { Uri = "unreal://project/levels" }, channel);

            // The disconnect surfaces as the structured IpcDisconnectedException — never hangs to the timeout.
            await Assert.ThrowsAsync<IpcDisconnectedException>(() => proxy.RunGetContent.Run(new object?[0]));
        }

        [Fact]
        public async Task Create_ListContext_SynthesizesSingleStaticEntry_NoIpc()
        {
            // The static MVP list is synthesized from the descriptor with NO IPC round-trip: the channel must
            // never be touched by RunListContext (only RunGetContent reads over IPC).
            var channel = new FakeResourceCallChannel();
            var proxy = ProxyResourceFactory.Create(
                new ResourceDescriptor
                {
                    Uri = "unreal://project/levels",
                    Name = "Project Levels",
                    Description = "Levels snapshot",
                    MimeType = "application/json",
                    Enabled = true,
                },
                channel);

            var list = await proxy.RunListContext.Run(new object?[0]);

            Assert.Equal(0, channel.Calls); // NO IPC for the static list
            Assert.Single(list);
            Assert.Equal("unreal://project/levels", list[0].Uri);
            Assert.Equal("Project Levels", list[0].Name); // the friendly display name, not the uri
            Assert.Equal("application/json", list[0].MimeType);
            Assert.Equal("Levels snapshot", list[0].Description);
            Assert.True(list[0].Enabled);
        }

        [Fact]
        public void Create_MapsRouteAndNameToUri_AndPropagatesEnabledFlag()
        {
            var channel = new FakeResourceCallChannel();
            var proxy = ProxyResourceFactory.Create(
                new ResourceDescriptor { Uri = "unreal://project/levels", Name = "Project Levels", Enabled = false },
                channel);

            // Name == Route == uri so the manager's Name-key and route lookup share one identity.
            Assert.Equal("unreal://project/levels", proxy.Route);
            Assert.Equal("unreal://project/levels", proxy.Name);
            Assert.False(proxy.Enabled);
        }

        [Fact]
        public async Task Create_EmptyContents_MapsToEmptyArray()
        {
            var channel = new FakeResourceCallChannel(uri => new ResourceResponseMessage
            {
                Status = IpcProtocol.Status.Success,
                Contents = new List<ResourceContentEntry>(),
            });
            var proxy = ProxyResourceFactory.Create(new ResourceDescriptor { Uri = "unreal://empty" }, channel);

            var contents = await proxy.RunGetContent.Run(new object?[0]);
            Assert.Empty(contents);
        }
    }
}
