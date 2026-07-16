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
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Proves the mcp-authorize g5/g6 <c>server-launch-args</c> consolidation: the sidecar composes the LOCAL
    /// gamedev-mcp-server launch-arg string via the SHARED
    /// <c>com.IvanMurzak.McpPlugin.ServerLaunch.ServerLaunchArguments</c> builder (none/oauth/token) so the C++
    /// <c>FUnrealMcpServerManager</c> holds no duplicate arg logic. Drives <see cref="SidecarHost.BuildServerLaunchArgsResult"/>
    /// directly (no live socket), asserting the target-state <c>auth=&lt;mode&gt;</c> key and the fail-closed
    /// missing-credential handling.
    /// </summary>
    public class SidecarHostServerLaunchArgsTests
    {
        private static SidecarHost BuildHost()
        {
            var ipc = new IpcClient("127.0.0.1", 39995, token: "test-token", sidecarVersion: "0.1.0");
            var host = new SidecarHost(ipc, "0.1.0");
            host.Build();
            return host;
        }

        private static JsonObject Request(string requestId, string authMode, int port = 20123, int timeoutMs = 10000,
            string? token = null, string? authIssuer = null, string? publicUrl = null)
        {
            var node = new JsonObject
            {
                ["type"] = IpcProtocol.Type.ServerLaunchArgs,
                ["requestId"] = requestId,
                ["port"] = port,
                ["pluginTimeoutMs"] = timeoutMs,
                ["authMode"] = authMode,
            };
            if (token != null) node["token"] = token;
            if (authIssuer != null) node["authIssuer"] = authIssuer;
            if (publicUrl != null) node["publicUrl"] = publicUrl;
            return node;
        }

        [Fact]
        public void ServerLaunchArgsMessages_RoundTripOverTheWire()
        {
            var request = new ServerLaunchArgsRequestMessage
            {
                RequestId = "req-1", Port = 20123, PluginTimeoutMs = 10000, AuthMode = "token", Token = "secret123",
            };
            var reqJson = JsonSerializer.Serialize(request, IpcProtocol.JsonOptions);
            var reqBack = JsonSerializer.Deserialize<ServerLaunchArgsRequestMessage>(reqJson, IpcProtocol.JsonOptions)!;
            Assert.Equal(IpcProtocol.Type.ServerLaunchArgs, reqBack.Type);
            Assert.Equal("req-1", reqBack.RequestId);
            Assert.Equal(20123, reqBack.Port);
            Assert.Equal("token", reqBack.AuthMode);
            Assert.Equal("secret123", reqBack.Token);

            var result = new ServerLaunchArgsResultMessage { RequestId = "req-1", Ok = true, Args = "port=20123 auth=token token=secret123" };
            var resJson = JsonSerializer.Serialize(result, IpcProtocol.JsonOptions);
            var resBack = JsonSerializer.Deserialize<ServerLaunchArgsResultMessage>(resJson, IpcProtocol.JsonOptions)!;
            Assert.Equal(IpcProtocol.Type.ServerLaunchArgsResult, resBack.Type);
            Assert.True(resBack.Ok);
            Assert.Equal("port=20123 auth=token token=secret123", resBack.Args);
        }

        [Fact]
        public void BuildServerLaunchArgsResult_NoneMode_ComposesAnonymousArgs()
        {
            using var host = BuildHost();

            var result = host.BuildServerLaunchArgsResult(Request("r-1", "none"));

            Assert.True(result.Ok);
            Assert.Equal("r-1", result.RequestId);
            Assert.NotNull(result.Args);
            Assert.Contains("port=20123", result.Args!);
            Assert.Contains("plugin-timeout=10000", result.Args!);
            Assert.Contains("client-transport=streamableHttp", result.Args!);
            Assert.Contains("auth=none", result.Args!);              // target-state key (not legacy `authorization=`)
            Assert.DoesNotContain("token=", result.Args!);
            Assert.DoesNotContain("authorization=", result.Args!);
        }

        [Fact]
        public void BuildServerLaunchArgsResult_TokenMode_AppendsTokenArg()
        {
            using var host = BuildHost();

            var result = host.BuildServerLaunchArgsResult(Request("r-2", "token", token: "secret123"));

            Assert.True(result.Ok);
            Assert.Contains("auth=token", result.Args!);
            Assert.Contains("token=secret123", result.Args!);
        }

        [Fact]
        public void BuildServerLaunchArgsResult_OauthMode_AppendsIssuerAndPublicUrl()
        {
            using var host = BuildHost();

            var result = host.BuildServerLaunchArgsResult(Request(
                "r-3", "oauth", authIssuer: "https://ai-game.dev", publicUrl: "http://localhost:20123/mcp/p/34ea75f2"));

            Assert.True(result.Ok);
            Assert.Contains("auth=oauth", result.Args!);
            Assert.Contains("auth-issuer=https://ai-game.dev", result.Args!);
            Assert.Contains("public-url=http://localhost:20123/mcp/p/34ea75f2", result.Args!);
        }

        [Fact]
        public void BuildServerLaunchArgsResult_TokenModeMissingSecret_FailsClosed()
        {
            using var host = BuildHost();

            var result = host.BuildServerLaunchArgsResult(Request("r-4", "token", token: null));

            Assert.False(result.Ok);
            Assert.Equal("r-4", result.RequestId);
            Assert.False(string.IsNullOrEmpty(result.Error));
            Assert.Null(result.Args);
        }

        [Fact]
        public void BuildServerLaunchArgsResult_OauthModeMissingPublicUrl_FailsClosed()
        {
            using var host = BuildHost();

            var result = host.BuildServerLaunchArgsResult(Request("r-5", "oauth", authIssuer: "https://ai-game.dev"));

            Assert.False(result.Ok);
            Assert.False(string.IsNullOrEmpty(result.Error));
            Assert.Null(result.Args);
        }

        [Fact]
        public void BuildServerLaunchArgsResult_LegacyRequiredMode_RejectedNotSilentlyDowngraded()
        {
            using var host = BuildHost();

            // `required` is the retired legacy value — the plugin migrates it to `token` BEFORE sending, so the sidecar
            // never receives it. If one ever arrives, fail closed (never silently spawn an anonymous/mis-authed server).
            var result = host.BuildServerLaunchArgsResult(Request("r-6", "required"));

            Assert.False(result.Ok);
            Assert.False(string.IsNullOrEmpty(result.Error));
            Assert.Null(result.Args);
        }

        [Fact]
        public void BuildServerLaunchArgsResult_NumericAuthMode_RejectedNotSilentlyDowngraded()
        {
            using var host = BuildHost();

            // Enum.TryParse also parses NUMERIC strings ("0" -> the first enum member, none). A numeric authMode must
            // NOT be accepted — that would silently spawn an anonymous (auth=none) server. Only the NAMES none|oauth|token
            // are valid; a numeric form fails closed, exactly like the legacy `required` rejection above.
            var result = host.BuildServerLaunchArgsResult(Request("r-7", "0"));

            Assert.False(result.Ok);
            Assert.Equal("r-7", result.RequestId);
            Assert.False(string.IsNullOrEmpty(result.Error));
            Assert.Null(result.Args);
        }
    }
}
