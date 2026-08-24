/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.Text.Json;
using System.Text.Json.Nodes;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// IPC v2 (M16 P0, docs/ARCHITECTURE.md §A.1): version negotiation (old/new pair compatibility) and the
    /// new prompt/resource message DTO round-trips over the shared camelCase NDJSON serializer.
    /// </summary>
    public class IpcProtocolV2Tests
    {
        [Fact]
        public void IpcVersion_IsTwo()
        {
            // The bump 1→2 is the headline of P0; pin it so a later accidental revert is caught.
            Assert.Equal(2, IpcProtocol.IpcVersion);
        }

        [Theory]
        // local, remote → negotiated. min(local, remote); a missing/<=0 remote is treated as legacy v1.
        [InlineData(2, 2, 2)] // both new → v2: prompts/resources exchanged
        [InlineData(2, 1, 1)] // new sidecar, OLD plugin → v1: tools-only (old peer keeps working)
        [InlineData(1, 2, 1)] // old sidecar, new plugin → v1: tools-only
        [InlineData(1, 1, 1)] // both old → v1
        [InlineData(2, 0, 1)] // plugin omitted the field (legacy handshake) → treated as v1
        [InlineData(3, 2, 2)] // a future-newer sidecar still negotiates down to the plugin's v2
        public void NegotiateVersion_PicksTheHighestBothUnderstand(int local, int remote, int expected)
        {
            Assert.Equal(expected, IpcProtocol.NegotiateVersion(local, remote));
        }

        [Theory]
        [InlineData(2, true)]
        [InlineData(1, false)]
        [InlineData(0, false)]
        public void SupportsPromptsResources_GatesOnV2(int negotiated, bool expected)
        {
            Assert.Equal(expected, IpcProtocol.SupportsPromptsResources(negotiated));
        }

        [Fact]
        public void PromptManifest_RoundTripsThroughTheSharedSerializer()
        {
            var manifest = new PromptManifestMessage
            {
                Revision = 4,
                Prompts =
                {
                    new PromptDescriptor
                    {
                        Name = "level-design-brief",
                        Title = "Level design brief",
                        Description = "Drafts a brief",
                        Role = "user",
                        InputSchema = JsonNode.Parse("""{"type":"object","properties":{"theme":{"type":"string"}}}"""),
                        Enabled = true,
                        ExtensionId = "core",
                        SchemaHash = "abc123",
                    },
                },
            };

            var json = JsonSerializer.Serialize(manifest, IpcProtocol.JsonOptions);
            // camelCase discriminator + fields (the §A.1 NDJSON contract the C++ plugin must match).
            Assert.Contains("\"type\":\"prompt-manifest\"", json);
            Assert.Contains("\"role\":\"user\"", json);
            Assert.Contains("\"inputSchema\"", json);

            var back = JsonSerializer.Deserialize<PromptManifestMessage>(json, IpcProtocol.JsonOptions)!;
            Assert.Equal(4, back.Revision);
            var p = Assert.Single(back.Prompts);
            Assert.Equal("level-design-brief", p.Name);
            Assert.Equal("user", p.Role);
            Assert.Equal("core", p.ExtensionId);
            Assert.True(p.Enabled);
        }

        [Fact]
        public void ResourceManifest_RoundTripsThroughTheSharedSerializer()
        {
            var manifest = new ResourceManifestMessage
            {
                Revision = 7,
                Resources =
                {
                    new ResourceDescriptor
                    {
                        Uri = "unreal://project/levels",
                        Name = "Project levels",
                        Description = "All maps in the project",
                        MimeType = "application/json",
                        Enabled = true,
                        ExtensionId = "core",
                        SchemaHash = "def456",
                    },
                },
            };

            var json = JsonSerializer.Serialize(manifest, IpcProtocol.JsonOptions);
            Assert.Contains("\"type\":\"resource-manifest\"", json);
            Assert.Contains("\"uri\":\"unreal://project/levels\"", json);
            Assert.Contains("\"mimeType\":\"application/json\"", json);

            var back = JsonSerializer.Deserialize<ResourceManifestMessage>(json, IpcProtocol.JsonOptions)!;
            Assert.Equal(7, back.Revision);
            var r = Assert.Single(back.Resources);
            Assert.Equal("unreal://project/levels", r.Uri);
            Assert.Equal("application/json", r.MimeType);
        }

        [Fact]
        public void PromptGetAndResponse_RoundTrip()
        {
            var get = new PromptGetMessage
            {
                RequestId = "r1",
                Prompt = "level-design-brief",
                Arguments = new JsonObject { ["theme"] = "forest" },
                TimeoutMs = 12345,
            };
            var getJson = JsonSerializer.Serialize(get, IpcProtocol.JsonOptions);
            Assert.Contains("\"type\":\"prompt-get\"", getJson);
            var getBack = JsonSerializer.Deserialize<PromptGetMessage>(getJson, IpcProtocol.JsonOptions)!;
            Assert.Equal("level-design-brief", getBack.Prompt);
            Assert.Equal(12345, getBack.TimeoutMs);
            Assert.Equal("forest", getBack.Arguments!["theme"]!.GetValue<string>());

            var response = new PromptResponseMessage
            {
                RequestId = "r1",
                Status = IpcProtocol.Status.Success,
                Description = "ok",
                Messages = new() { new PromptResponseEntry { Role = "user", Text = "Design a forest level." } },
            };
            var respJson = JsonSerializer.Serialize(response, IpcProtocol.JsonOptions);
            Assert.Contains("\"type\":\"prompt-response\"", respJson);
            var respBack = JsonSerializer.Deserialize<PromptResponseMessage>(respJson, IpcProtocol.JsonOptions)!;
            Assert.Equal("r1", respBack.RequestId);
            var entry = Assert.Single(respBack.Messages!);
            Assert.Equal("user", entry.Role);
            Assert.Equal("Design a forest level.", entry.Text);
        }

        [Fact]
        public void ResourceReadAndResponse_RoundTrip_WithTextAndBlob()
        {
            var read = new ResourceReadMessage { RequestId = "r2", Uri = "unreal://project/levels", TimeoutMs = 5000 };
            var readJson = JsonSerializer.Serialize(read, IpcProtocol.JsonOptions);
            Assert.Contains("\"type\":\"resource-read\"", readJson);
            var readBack = JsonSerializer.Deserialize<ResourceReadMessage>(readJson, IpcProtocol.JsonOptions)!;
            Assert.Equal("unreal://project/levels", readBack.Uri);

            var response = new ResourceResponseMessage
            {
                RequestId = "r2",
                Status = IpcProtocol.Status.Success,
                Contents = new()
                {
                    new ResourceContentEntry { Uri = "unreal://project/levels", MimeType = "application/json", Text = "[]" },
                    new ResourceContentEntry { Uri = "unreal://thumb.png", MimeType = "image/png", Blob = "aGVsbG8=" },
                },
            };
            var respJson = JsonSerializer.Serialize(response, IpcProtocol.JsonOptions);
            Assert.Contains("\"type\":\"resource-response\"", respJson);

            var respBack = JsonSerializer.Deserialize<ResourceResponseMessage>(respJson, IpcProtocol.JsonOptions)!;
            Assert.Equal(2, respBack.Contents!.Count);
            Assert.Equal("[]", respBack.Contents![0].Text);
            Assert.Null(respBack.Contents![0].Blob);
            Assert.Equal("aGVsbG8=", respBack.Contents![1].Blob);   // base64 binary rides intact
            Assert.Null(respBack.Contents![1].Text);
        }

        // --- §1.3 `status` cloudAuthState (oauth-client-error-hygiene d1) -----------------------------------

        [Fact]
        public void Status_CloudAuthState_SignInRequired_RoundTripsOnTheWire()
        {
            // The third contract value (IpcMessages.cs StatusMessage.CloudAuthState): a terminal dead-family
            // verdict rides the status channel as "SignInRequired" — pin the exact wire spelling the C++
            // ApplyStatus matches on, so a casing/rename drift is caught here rather than as a silent no-op
            // in the editor (the C++ side deliberately IGNORES unrecognized values).
            var status = new StatusMessage
            {
                ConnectionState = "Connecting",
                KeepConnected = true,
                CloudAuthState = "SignInRequired",
            };

            var json = JsonSerializer.Serialize(status, IpcProtocol.JsonOptions);
            Assert.Contains("\"type\":\"status\"", json);
            Assert.Contains("\"cloudAuthState\":\"SignInRequired\"", json);

            var back = JsonSerializer.Deserialize<StatusMessage>(json, IpcProtocol.JsonOptions)!;
            Assert.Equal("SignInRequired", back.CloudAuthState);
            Assert.Equal("Connecting", back.ConnectionState);
            Assert.True(back.KeepConnected);
        }

        [Theory]
        [InlineData("Authorized")]    // the long-standing emitted value
        [InlineData("Unauthorized")]  // legacy documented value (never emitted) — must still parse
        [InlineData("SomeFutureState")] // a value from a NEWER peer — the string field must carry it, not throw
        public void Status_LegacyAndUnknownCloudAuthValues_StillParse(string value)
        {
            // The CloudAuthState field is a plain string on the wire (no C#-side enum/validation), so a
            // legacy or future value must deserialize losslessly — growing the value set can never break an
            // older sidecar's parse. This is the C# half of the "tolerate the third value" contract; the C++
            // half (ApplyStatus ignores unrecognized values) is locked in UnrealMcpEditorViewModelSpec.
            var json = $"{{\"type\":\"status\",\"connectionState\":\"Connected\",\"keepConnected\":true,\"cloudAuthState\":\"{value}\",\"aiAgents\":[]}}";
            var back = JsonSerializer.Deserialize<StatusMessage>(json, IpcProtocol.JsonOptions)!;
            Assert.Equal(value, back.CloudAuthState);
        }

        [Fact]
        public void Status_AbsentCloudAuthState_ParsesAsNull()
        {
            // Custom mode / no cloud credential: the field is simply absent (null) — the pre-cloud contract.
            var json = "{\"type\":\"status\",\"connectionState\":\"Connected\",\"keepConnected\":true,\"aiAgents\":[]}";
            var back = JsonSerializer.Deserialize<StatusMessage>(json, IpcProtocol.JsonOptions)!;
            Assert.Null(back.CloudAuthState);
        }
    }
}
