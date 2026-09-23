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
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Project keys in the sidecar's agent-config service (project-keys contract §7): a Cloud HTTP Configure writes
    /// <c>Authorization: Bearer agd_pk_…</c> for every agent (get-or-mint through the REAL
    /// <see cref="ProjectKeyProvider"/> + an isolated cache, with a scripted HTTP server), falls back to the URL-only
    /// config when signed out or when the mint is refused, leaves Local mode alone, and "Regenerate key" mints a new
    /// key, rewrites the configured agents and revokes the old one.
    /// </summary>
    public class AgentConfigProjectKeyTests : IDisposable
    {
        private const string Issuer = "https://ai-game.dev";
        private readonly string _projectRoot;
        private readonly string _cacheDir;
        private readonly FakeProjectKeyServer _server = new();

        public AgentConfigProjectKeyTests()
        {
            var root = Path.Combine(Path.GetTempPath(), "unreal-mcp-pk-" + Guid.NewGuid().ToString("N"));
            _projectRoot = Path.Combine(root, "project");
            _cacheDir = Path.Combine(root, "machine");
            Directory.CreateDirectory(_projectRoot);
            Directory.CreateDirectory(_cacheDir);
        }

        public void Dispose()
        {
            try { Directory.Delete(Path.GetDirectoryName(_projectRoot)!, recursive: true); }
            catch { /* best-effort temp cleanup */ }
        }

        private ProjectKeyProvider NewProvider() => new(
            _ => Task.FromResult<string?>("machine-access-token"),
            Issuer,
            new ProjectKeyStore(_cacheDir),
            new HttpClient(_server),
            subjectFallback: () => "user-1");

        private AgentConfigService SignedIn()
        {
            var provider = NewProvider();
            return new AgentConfigService(projectKeys: () => provider);
        }

        private static AgentConfigService SignedOut() => new(projectKeys: () => null);

        private AgentSettingsDto Settings(string mode = "Cloud") => new()
        {
            ProjectRootPath = _projectRoot,
            ExecutableFullPath = Path.Combine(_projectRoot, "server.exe"),
            Port = 12345,
            TimeoutMs = 30000,
            Host = mode == "Cloud" ? Issuer + "/mcp" : "http://localhost:12345/mcp",
            ConnectionMode = mode,
            AuthRequired = mode == "Cloud",
        };

        private Task<AgentConfigResultMessage> Configure(AgentConfigService service, string agentId, string mode = "Cloud") =>
            service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "cfg-" + agentId, AgentId = agentId, Transport = "streamableHttp", Settings = Settings(mode),
            });

        private string McpJson => File.ReadAllText(Path.Combine(_projectRoot, ".mcp.json"));

        [Fact]
        public async Task Cloud_Configure_WritesTheProjectKeyHeader_AndThePinnedUrl()
        {
            var service = SignedIn();

            var result = await Configure(service, "claude-code");

            Assert.True(result.Ok);
            Assert.Equal(ProjectKeyStatus.InUse, result.ProjectKeyStatus);
            Assert.Equal(1, _server.Mints);
            var content = McpJson;
            Assert.Contains("Bearer " + _server.KeyFor(1), content);
            var pin = AgentConfiguratorSettings.CreateForHost(_projectRoot, string.Empty, 0, 0, Issuer + "/mcp",
                null, ConnectionMode.Cloud).ProjectPin;
            Assert.Contains("/mcp/p/" + pin, content);
            // The minted key is bound to THIS project's pin and recorded as an Unreal key.
            Assert.Equal(pin, _server.LastMintBody!.RootElement.GetProperty("project_pin").GetString());
            Assert.Equal("unreal", _server.LastMintBody!.RootElement.GetProperty("engine").GetString());
            // Status compares against what was written (the key attached), and the key is never displayed.
            Assert.True(result.Description!.IsConfigured);
            Assert.DoesNotContain(_server.KeyFor(1), JsonSerializer.Serialize(result, IpcProtocol.JsonOptions));
        }

        [Fact]
        public async Task Cloud_Configure_EveryAgentGetsTheSameCachedKey_AndStatusNeedsNoNetwork()
        {
            var service = SignedIn();
            await Configure(service, "claude-code");
            await Configure(service, "cursor");

            // One mint: the second configure reuses the cached key (validated with GET …/current).
            Assert.Equal(1, _server.Mints);
            var cursor = File.ReadAllText(Path.Combine(_projectRoot, ".cursor", "mcp.json"));
            Assert.Contains("Bearer " + _server.KeyFor(1), cursor);

            var before = _server.Requests;
            var status = service.HandleStatus(new AgentStatusRequestMessage
            {
                RequestId = "st", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });
            Assert.Equal(before, _server.Requests); // status reads the cache only
            Assert.Equal(ProjectKeyStatus.InUse, status.ProjectKeyStatus);
            Assert.Equal("Configured", status.Description!.Status);
        }

        [Fact]
        public async Task Cloud_Configure_Codex_UsesHttpHeaders_NotTheEnvVarIndirection()
        {
            var result = await Configure(SignedIn(), "codex");

            Assert.True(result.Ok);
            var toml = File.ReadAllText(Path.Combine(_projectRoot, ".codex", "config.toml"));
            Assert.Contains("http_headers", toml);
            Assert.Contains("Bearer " + _server.KeyFor(1), toml);
            Assert.DoesNotContain("bearer_token_env_var", toml);
        }

        [Fact]
        public async Task Cloud_SignedOut_WritesUrlOnly_AndSaysSo()
        {
            var result = await Configure(SignedOut(), "claude-code");

            Assert.True(result.Ok);
            Assert.Equal(ProjectKeyStatus.SignedOut, result.ProjectKeyStatus);
            Assert.False(string.IsNullOrEmpty(result.ProjectKeyHint));
            Assert.DoesNotContain("Authorization", McpJson);
            Assert.Equal(0, _server.Requests);
        }

        [Fact]
        public async Task Cloud_MintRefused_404_DegradesToUrlOnly_NotAnError()
        {
            _server.MintStatus = HttpStatusCode.NotFound; // the server's mint flag is still off

            var result = await Configure(SignedIn(), "claude-code");

            Assert.True(result.Ok);
            Assert.Equal(ProjectKeyStatus.None, result.ProjectKeyStatus);
            Assert.DoesNotContain("Authorization", McpJson);
            Assert.True(result.Description!.IsConfigured);
        }

        [Fact]
        public async Task Local_Configure_NeverMints_AndStaysUrlOnly()
        {
            var result = await Configure(SignedIn(), "claude-code", mode: "Local");

            Assert.True(result.Ok);
            Assert.Equal(ProjectKeyStatus.NotApplicable, result.ProjectKeyStatus);
            Assert.Equal(0, _server.Requests);
            Assert.DoesNotContain("Authorization", McpJson);
        }

        [Fact]
        public async Task RegenerateKey_MintsNewKey_RewritesConfiguredAgents_AndRevokesTheOldKey()
        {
            var service = SignedIn();
            await Configure(service, "claude-code");
            Assert.Contains(_server.KeyFor(1), McpJson);

            var result = await service.HandleRegenerateKeyAsync(new AgentRegenerateKeyRequestMessage
            {
                RequestId = "regen", AgentId = "claude-code", Transport = "streamableHttp", Settings = Settings(),
            });

            Assert.True(result.Ok, result.Error);
            Assert.Equal(ProjectKeyStatus.InUse, result.ProjectKeyStatus);
            Assert.Equal(2, _server.Mints);
            Assert.Contains("claude-code", result.RewrittenAgents!);
            Assert.DoesNotContain("cursor", result.RewrittenAgents!); // never configured — not written
            var content = McpJson;
            Assert.Contains(_server.KeyFor(2), content);
            Assert.DoesNotContain(_server.KeyFor(1), content);
            Assert.Equal(new[] { _server.KeyIdFor(1) }, _server.Revoked);
            Assert.True(result.Description!.IsConfigured);
        }

        [Fact]
        public async Task RegenerateKey_RewritesAnEntryHoldingAnUnknownKey_ButLeavesStdioAlone()
        {
            var service = SignedIn();
            await Configure(service, "claude-code");
            // Cursor holds this project's pinned route with a key the cache never knew (e.g. regenerated and revoked
            // by the CLI); Gemini is configured over stdio for this project.
            var pin = AgentConfiguratorSettings.CreateForHost(_projectRoot, string.Empty, 0, 0, Issuer + "/mcp",
                null, ConnectionMode.Cloud).ProjectPin;
            var cursorPath = Path.Combine(_projectRoot, ".cursor", "mcp.json");
            Directory.CreateDirectory(Path.GetDirectoryName(cursorPath)!);
            File.WriteAllText(cursorPath, "{\"mcpServers\":{\"ai-game-developer\":{\"url\":\"" + Issuer + "/mcp/p/" + pin
                + "\",\"headers\":{\"Authorization\":\"Bearer agd_pk_stale\"}}}}");
            var gemini = await service.HandleConfigureAsync(new AgentConfigureRequestMessage
            {
                RequestId = "cfg-gemini", AgentId = "gemini", Transport = "stdio", Settings = Settings(),
            });
            Assert.True(gemini.Ok, gemini.Error);

            var result = await service.HandleRegenerateKeyAsync(new AgentRegenerateKeyRequestMessage
            {
                RequestId = "regen", AgentId = "claude-code", Settings = Settings(),
            });

            Assert.True(result.Ok, result.Error);
            Assert.Contains("cursor", result.RewrittenAgents!);
            Assert.DoesNotContain("gemini", result.RewrittenAgents!);
            var cursor = File.ReadAllText(cursorPath);
            Assert.Contains(_server.KeyFor(2), cursor);
            Assert.DoesNotContain("agd_pk_stale", cursor);
        }

        [Fact]
        public async Task RegenerateKey_SignedOut_Fails_AndChangesNothing()
        {
            await Configure(SignedOut(), "claude-code");
            var before = McpJson;

            var result = await SignedOut().HandleRegenerateKeyAsync(new AgentRegenerateKeyRequestMessage
            {
                RequestId = "regen", AgentId = "claude-code", Settings = Settings(),
            });

            Assert.False(result.Ok);
            Assert.Equal(ProjectKeyStatus.SignedOut, result.ProjectKeyStatus);
            Assert.Equal(before, McpJson);
        }

        [Fact]
        public async Task RegenerateKey_LocalMode_Fails()
        {
            var result = await SignedIn().HandleRegenerateKeyAsync(new AgentRegenerateKeyRequestMessage
            {
                RequestId = "regen", AgentId = "claude-code", Settings = Settings(mode: "Local"),
            });

            Assert.False(result.Ok);
            Assert.Equal(0, _server.Requests);
        }

        /// <summary>A scripted ai-game.dev project-key API (contract §2): mint, validate, revoke.</summary>
        private sealed class FakeProjectKeyServer : HttpMessageHandler
        {
            private int _mints;
            private int _requests;

            public HttpStatusCode MintStatus { get; set; } = HttpStatusCode.Created;
            public int Mints => _mints;
            public int Requests => _requests;
            public JsonDocument? LastMintBody { get; private set; }
            public List<string> Revoked { get; } = new();

            public string KeyFor(int n) => "agd_pk_test_" + n;
            public string KeyIdFor(int n) => "key-" + n;

            protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                Interlocked.Increment(ref _requests);
                var path = request.RequestUri!.AbsolutePath;
                if (request.Method == HttpMethod.Post && path == ProjectKeyProvider.MintPath)
                {
                    LastMintBody = JsonDocument.Parse(await request.Content!.ReadAsStringAsync(cancellationToken));
                    if (MintStatus != HttpStatusCode.Created)
                        return new HttpResponseMessage(MintStatus);
                    var n = Interlocked.Increment(ref _mints);
                    var pin = LastMintBody.RootElement.GetProperty("project_pin").GetString();
                    return Json(HttpStatusCode.Created, new { key = KeyFor(n), key_id = KeyIdFor(n), project_pin = pin, created_at = "2026-09-23T00:00:00Z" });
                }
                if (request.Method == HttpMethod.Get && path == ProjectKeyProvider.CurrentPath)
                    return Json(HttpStatusCode.OK, new { key_id = "any", active = true });
                if (request.Method == HttpMethod.Delete && path.StartsWith(ProjectKeyProvider.MintPath + "/", StringComparison.Ordinal))
                {
                    Revoked.Add(path.Substring(ProjectKeyProvider.MintPath.Length + 1));
                    return new HttpResponseMessage(HttpStatusCode.NoContent);
                }
                return new HttpResponseMessage(HttpStatusCode.NotFound);
            }

            private static HttpResponseMessage Json(HttpStatusCode status, object body) => new(status)
            {
                Content = new StringContent(JsonSerializer.Serialize(body), Encoding.UTF8, "application/json"),
            };
        }
    }
}
