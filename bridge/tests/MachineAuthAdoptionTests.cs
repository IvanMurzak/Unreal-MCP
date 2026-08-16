/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
└───────────────────────────────────────────────────────────────────┘
*/

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Auth;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// unified-machine-auth task e1 — the sidecar's adoption of the McpPlugin 8.1 v2 credential machinery,
    /// asserted at the WIRING level (SidecarHost → PluginCredentialProvider → the shared
    /// <see cref="HttpTokenRefresher"/> → the actual HTTP form), with only the HTTP layer scripted:
    /// <list type="bullet">
    ///   <item>a refresh presents the family's STORED <c>clientId</c>, never the component default (04 §3.2 /
    ///   D8 — mirrors production probe Q1: presenting a different id is a cross-mint the AS answers with
    ///   <c>invalid_grant</c>, ~1,000/day in production before this taskflow);</item>
    ///   <item>the refresh form omits <c>scope</c> and <c>resource</c> ENTIRELY (04 §3.3 / P0-3 — a component
    ///   default would permanently narrow an agent family);</item>
    ///   <item>F11.1: a REAL on-disk v1 file (raw bytes, DPAPI-protected on Windows) is adopted as
    ///   <c>families.legacy</c>, refreshed with the component-default id + no scope (04 §3.7), and rewritten
    ///   as a v2 document (+ v1 mirror) on the first successful refresh;</item>
    ///   <item>A1: an expired stored credential self-heals through the refresh path with NO re-auth (the
    ///   device-authorization endpoint is never touched).</item>
    /// </list>
    /// A per-test temp directory isolates the store from the real <c>~/.ai-game-dev</c>; no network.
    /// </summary>
    public class MachineAuthAdoptionTests : IDisposable
    {
        private readonly List<string> _tempDirs = new();

        private string NewStoreDir()
        {
            var dir = Path.Combine(Path.GetTempPath(), "umcp-e1-" + Guid.NewGuid().ToString("N")[..12]);
            Directory.CreateDirectory(dir);
            _tempDirs.Add(dir);
            return dir;
        }

        public void Dispose()
        {
            foreach (var dir in _tempDirs)
            {
                try { Directory.Delete(dir, recursive: true); } catch { /* best-effort cleanup */ }
            }
        }

        private static IpcClient NewIpc() => new("127.0.0.1", 39998, token: "ipc-token", sidecarVersion: "0.1.0");

        private static JsonObject CloudConfig(string cloudUrl = "https://ai-game.dev") =>
            new() { ["mode"] = "Cloud", ["cloudUrl"] = cloudUrl };

        /// <summary>
        /// A scripted AS: answers <c>/oauth/token</c> with the given body, capturing every token-endpoint
        /// request body, and records (but rejects) any <c>/oauth/device_authorization</c> call so the A1
        /// "no re-auth" half is a positive assertion, not an absence by accident.
        /// </summary>
        private sealed class CapturingTokenHandler : HttpMessageHandler
        {
            private readonly string _tokenJson;
            public List<string> TokenBodies { get; } = new();
            public int DeviceAuthorizationCalls { get; private set; }

            public CapturingTokenHandler(string tokenJson) => _tokenJson = tokenJson;

            protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                var url = request.RequestUri!.AbsoluteUri;
                var body = request.Content != null ? await request.Content.ReadAsStringAsync(cancellationToken) : "";
                if (url.EndsWith("/oauth/device_authorization", StringComparison.Ordinal))
                {
                    DeviceAuthorizationCalls++;
                    return new HttpResponseMessage(HttpStatusCode.BadRequest)
                    {
                        Content = new StringContent("{\"error\":\"unexpected_device_flow\"}", Encoding.UTF8, "application/json"),
                    };
                }
                TokenBodies.Add(body);
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(_tokenJson, Encoding.UTF8, "application/json"),
                };
            }
        }

        private const string RotatedTokenJson =
            "{\"access_token\":\"rotated-jwt\",\"refresh_token\":\"rotated-refresh\",\"token_type\":\"Bearer\",\"expires_in\":3600}";

        /// <summary>
        /// The production factory with ONLY the HttpClient scripted (the <c>authHttpClient</c> seam): the
        /// refresher is the one the REAL default wiring constructs — the shared <see cref="HttpTokenRefresher"/>
        /// with this component's client id — so these tests observe the true prod refresh path on the wire.
        /// </summary>
        private static SidecarHost ProdHost(string storeDir, CapturingTokenHandler handler) =>
            SidecarHost.CreateForProduction(
                NewIpc(), "0.1.0",
                credentialStore: new MachineCredentialStore(storeDir),
                authHttpClient: new HttpClient(handler));

        /// <summary>Seed a v2 store whose PLUGIN family is expired (forces a proactive refresh on first use).</summary>
        private static void SeedExpiredV2Store(string dir, string familyClientId)
        {
            new MachineCredentialStore(dir).Write(new MachineCredentials
            {
                ServerTarget = "https://ai-game.dev",
                Subject = "usr_1",
                Families = new MachineCredentialFamilies
                {
                    Plugin = new MachineCredentialFamily
                    {
                        AccessToken = "stale-jwt",
                        RefreshToken = "old-refresh",
                        ExpiresAt = DateTimeOffset.UtcNow.AddMinutes(-5),
                        ClientId = familyClientId,
                        Scope = "mcp:plugin",
                    },
                },
            });
        }

        // --- 04 §3.2 / D8: the refresh presents the family's STORED clientId (probe Q1 mirror) ------------------

        [Fact]
        public async Task Refresh_PresentsTheFamilysStoredClientId_NeverTheComponentDefault()
        {
            // The family was minted by ANOTHER surface (machine-wide store — any component may have written it).
            // The pre-e1 refresher hardcoded this component's id; the AS answers such a cross-mint with
            // invalid_grant and the family survives only by luck (probe Q1). The shared wiring must present the
            // STORED id verbatim.
            var dir = NewStoreDir();
            SeedExpiredV2Store(dir, familyClientId: "cross-minted-surface");
            var handler = new CapturingTokenHandler(RotatedTokenJson);
            using var host = ProdHost(dir, handler);
            host.ApplyConnectionConfig(CloudConfig());

            var bearer = await Task.Run(() => host.CurrentBearer);

            Assert.Equal("rotated-jwt", bearer);
            var form = Assert.Single(handler.TokenBodies);
            Assert.Contains("client_id=cross-minted-surface", form);
            Assert.DoesNotContain("client_id=" + DeviceCodeAuthenticator.DefaultClientId, form);
        }

        // --- 04 §3.3 / P0-3: the refresh form omits scope AND resource entirely ---------------------------------

        [Fact]
        public async Task Refresh_OmitsScopeAndResourceEntirely()
        {
            // Sending a component-default scope on refresh PERMANENTLY narrows an agent family (the server
            // re-stamps the rotated row with the requested subset — verified oauth_token_service.py:569-574).
            // The wire contract is grant_type + refresh_token + client_id and NOTHING else.
            var dir = NewStoreDir();
            SeedExpiredV2Store(dir, familyClientId: "cross-minted-surface");
            var handler = new CapturingTokenHandler(RotatedTokenJson);
            using var host = ProdHost(dir, handler);
            host.ApplyConnectionConfig(CloudConfig());

            var bearer = await Task.Run(() => host.CurrentBearer);

            Assert.Equal("rotated-jwt", bearer);
            var form = Assert.Single(handler.TokenBodies);
            Assert.Contains("grant_type=refresh_token", form);   // the positive half: the form is a real refresh
            Assert.DoesNotContain("scope=", form);
            Assert.DoesNotContain("resource=", form);
        }

        // --- F11.1: a real on-disk v1 file → families.legacy → component-default refresh → v2 rewrite -----------

        [Fact]
        public async Task V1File_AdoptedAsLegacyFamily_RefreshUsesComponentDefaultNoScope_AndRewritesV2()
        {
            // A machine updated in place holds a V1 FILE written by the shipped fleet. The updated sidecar must
            // read it as families.legacy (unknown clientId/scope — 04 §1), stay signed in with ZERO user action,
            // refresh with the component-default id and NO scope (status-quo semantics, 04 §3.7), and rewrite
            // the document as v2 (+ v1 mirror) on the first successful refresh.
            var dir = NewStoreDir();
            WriteRawV1File(dir,
                accessToken: "v1-jwt", refreshToken: "v1-refresh",
                expiresAt: DateTimeOffset.UtcNow.AddMinutes(-5), // expired → first use refreshes
                serverTarget: "https://ai-game.dev");
            var handler = new CapturingTokenHandler(RotatedTokenJson);
            using var host = ProdHost(dir, handler);

            Assert.True(host.CredentialProvider!.IsSignedIn); // v1 adoption is zero-button (F11.1)

            host.ApplyConnectionConfig(CloudConfig());
            var bearer = await Task.Run(() => host.CurrentBearer);
            Assert.Equal("rotated-jwt", bearer);

            // Status-quo refresh semantics for a legacy family: component-default id, no scope, no resource.
            var form = Assert.Single(handler.TokenBodies);
            Assert.Contains("client_id=" + DeviceCodeAuthenticator.DefaultClientId, form);
            Assert.Contains("refresh_token=v1-refresh", form);
            Assert.DoesNotContain("scope=", form);
            Assert.DoesNotContain("resource=", form);

            // The first successful refresh rewrote the document as v2: families.legacy (still of unknown
            // clientId/scope) + the v1 compat mirror re-stamped from it, so old readers keep working.
            var read = new MachineCredentialStore(dir).TryRead();
            Assert.Equal(MachineCredentialStoreStatus.Ok, read.Status);
            var creds = read.Credentials!;
            Assert.Equal(MachineCredentials.CurrentVersion, creds.Version);
            var legacy = creds.Families?.Legacy;
            Assert.NotNull(legacy);
            Assert.Equal("rotated-jwt", legacy!.AccessToken);
            Assert.Equal("rotated-refresh", legacy.RefreshToken);
            Assert.Null(legacy.ClientId); // unknown by definition — never invented (04 §1)
            Assert.Null(legacy.Scope);
            Assert.Equal("rotated-jwt", creds.AccessToken);      // v1 mirror
            Assert.Equal("rotated-refresh", creds.RefreshToken); // v1 mirror
        }

        // --- A1: expiry self-heals with no re-auth --------------------------------------------------------------

        [Fact]
        public async Task ExpiredStoredCredential_SelfHeals_WithoutReauth_AndPersistsRotation()
        {
            // The A1 acceptance (08): a signed-in machine whose access token expired silently self-heals via
            // the locked refresh path — no device flow, no browser, no user action — and the rotation is
            // PERSISTED so the healed state survives a restart.
            var dir = NewStoreDir();
            SeedExpiredV2Store(dir, familyClientId: "unreal-mcp-plugin");
            var handler = new CapturingTokenHandler(RotatedTokenJson);
            using var host = ProdHost(dir, handler);
            host.ApplyConnectionConfig(CloudConfig());

            var bearer = await Task.Run(() => host.CurrentBearer);

            Assert.Equal("rotated-jwt", bearer);
            Assert.Equal(0, handler.DeviceAuthorizationCalls); // no re-auth: the device flow was never touched
            Assert.Single(handler.TokenBodies);                // exactly one refresh attempt (04 §3.6 discipline)

            // The rotation reached the DISK (survives a restart): a fresh host over the same dir is signed in
            // with the rotated tokens and needs no further network.
            var persisted = new MachineCredentialStore(dir).Read();
            Assert.Equal("rotated-jwt", persisted!.Families!.Plugin!.AccessToken);
            Assert.Equal("rotated-refresh", persisted.Families.Plugin.RefreshToken);
        }

        // --- Raw v1-file writer (the codec the shipped fleet wrote: DPAPI blob on Windows, 0600 JSON on POSIX) --

        private static void WriteRawV1File(string dir, string accessToken, string refreshToken, DateTimeOffset expiresAt, string serverTarget)
        {
            var json =
                "{\"version\":1," +
                "\"accessToken\":\"" + accessToken + "\"," +
                "\"refreshToken\":\"" + refreshToken + "\"," +
                "\"expiresAt\":\"" + expiresAt.UtcDateTime.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'") + "\"," +
                "\"serverTarget\":\"" + serverTarget + "\"}";
            var bytes = Encoding.UTF8.GetBytes(json);
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                bytes = DpapiProtect(bytes);
            File.WriteAllBytes(Path.Combine(dir, MachineCredentialStore.CredentialsFileName), bytes);
        }

        // Byte-compatible with the store's own codec (design D2): CurrentUser scope, null entropy,
        // CRYPTPROTECT_UI_FORBIDDEN — the same P/Invoke shape as MachineCredentialStore's Protect().
        private static byte[] DpapiProtect(byte[] data)
        {
            var inBlob = new DATA_BLOB();
            var outBlob = new DATA_BLOB();
            try
            {
                inBlob.pbData = Marshal.AllocHGlobal(data.Length);
                inBlob.cbData = data.Length;
                Marshal.Copy(data, 0, inBlob.pbData, data.Length);

                if (!CryptProtectData(ref inBlob, "ai-game-dev credentials", IntPtr.Zero, IntPtr.Zero,
                        IntPtr.Zero, CRYPTPROTECT_UI_FORBIDDEN, ref outBlob))
                    throw new CryptographicException(Marshal.GetLastWin32Error());

                var result = new byte[outBlob.cbData];
                Marshal.Copy(outBlob.pbData, result, 0, outBlob.cbData);
                return result;
            }
            finally
            {
                if (inBlob.pbData != IntPtr.Zero) Marshal.FreeHGlobal(inBlob.pbData);
                if (outBlob.pbData != IntPtr.Zero) LocalFree(outBlob.pbData);
            }
        }

        private const uint CRYPTPROTECT_UI_FORBIDDEN = 0x1;

        [StructLayout(LayoutKind.Sequential)]
        private struct DATA_BLOB
        {
            public int cbData;
            public IntPtr pbData;
        }

        [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern bool CryptProtectData(ref DATA_BLOB pDataIn, string szDataDescr, IntPtr pOptionalEntropy,
            IntPtr pvReserved, IntPtr pPromptStruct, uint dwFlags, ref DATA_BLOB pDataOut);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LocalFree(IntPtr hMem);
    }
}
