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
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Regression specs for the connection-transition queue (docs/ARCHITECTURE.md §6/§7, issue #35): the
    /// serialized queue must be genuinely LAST-WRITE-WINS. Before the fix it was strict FIFO with no way to
    /// interrupt an in-flight transition, so a Cloud reconnect whose <c>plugin.Connect()</c> never returned
    /// (McpPlugin's <c>StartConnectionLoop</c> retries forever while a transport keeps failing, holding its
    /// gate) pinned every later transition behind it. The user's switch-back-to-Custom re-dial was therefore
    /// enqueued and never ran — the bridge held no SignalR link while the UI kept a stale "Connected", and a
    /// "Restart bridge" was the only recovery. The fix supersedes (cancels) the in-flight transition on each
    /// new submission and threads the token into <c>plugin.Connect/Disconnect</c>.
    /// </summary>
    public class SidecarHostTransitionTests
    {
        private static JsonObject Cfg(string mode, string? host = null, string? cloudUrl = null, string? token = null)
        {
            var o = new JsonObject { ["type"] = "config", ["mode"] = mode };
            if (host != null) o["host"] = host;
            if (cloudUrl != null) o["cloudUrl"] = cloudUrl;
            if (token != null) o["token"] = token;
            return o;
        }

        private static SidecarHost NewHost(out IpcClient ipc)
        {
            // Port 39998 is never dialed — the fake plugin replaces the real SignalR client, so no socket opens.
            ipc = new IpcClient("127.0.0.1", 39998, token: "ipc-token", sidecarVersion: "0.1.0");
            return new SidecarHost(ipc, "0.1.0");
        }

        [Fact]
        public async Task RunConnectionTransition_NewSubmissionSupersedesAStalledPredecessor()
        {
            using var host = NewHost(out _);

            var aStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var aCancelled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var bRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

            // Transition A models the wedged Cloud connect: it never completes on its own, only on cancellation.
            _ = host.RunConnectionTransition(async ct =>
            {
                aStarted.TrySetResult();
                try { await Task.Delay(Timeout.Infinite, ct).ConfigureAwait(false); }
                catch (OperationCanceledException) { aCancelled.TrySetResult(); }
            });

            await aStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            // Transition B is the user's latest action. It MUST run even though A is still "connecting".
            _ = host.RunConnectionTransition(_ => { bRan.TrySetResult(); return Task.CompletedTask; });

            await bRan.Task.WaitAsync(TimeSpan.FromSeconds(5));        // pre-fix: pinned behind A → times out
            await aCancelled.Task.WaitAsync(TimeSpan.FromSeconds(5));  // A was superseded (cancelled), not abandoned
        }

        [Fact]
        public async Task PostDeviceAuth_SwitchBackToCustom_ReDialsLocal_NotPinnedBehindTheStalledCloudConnect()
        {
            using var host = NewHost(out _);

            var cloudStalling = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var localDialed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

            // The fake mirrors the live failure mode: a Cloud dial never returns on its own (stalls until its
            // token is cancelled = superseded); a Custom (localhost) dial succeeds at once and proves the
            // switch-back-to-Custom re-dial actually ran. It reads the live Host the bridge resolved, exactly
            // as the real ConnectionManager reads Endpoint from the shared ConnectionConfig.
            var fake = new FakeMcpPlugin(async ct =>
            {
                var target = host.Config.Host ?? "";
                if (target.Contains("localhost", StringComparison.OrdinalIgnoreCase))
                {
                    localDialed.TrySetResult();
                    return true;
                }
                cloudStalling.TrySetResult();
                await Task.Delay(Timeout.Infinite, ct).ConfigureAwait(false); // wedged Cloud connect
                return false;
            });
            host.SetPluginForTest(fake);

            // In Cloud mode, armed (the state right after the user switched to Cloud and authorized).
            host.ApplyConnectionConfig(Cfg("Cloud", cloudUrl: "https://ai-game.dev"));

            // The device-auth commit re-dials Cloud with the bearer — and that dial wedges. Fire-and-forget:
            // awaiting it would hang forever on the pre-fix code, which is precisely the bug.
            _ = host.CommitAuthorizedSessionAsync("cloud-bearer", CancellationToken.None);
            await cloudStalling.Task.WaitAsync(TimeSpan.FromSeconds(5)); // ensure the Cloud connect is in-flight

            // The user switches back to Custom. This MUST supersede the wedged Cloud connect and dial localhost.
            host.OnConfigReceived(Cfg("Custom", host: "http://localhost:8500", token: ""));

            // Pre-fix: the localhost dial is pinned behind the never-completing Cloud connect → times out (red).
            // Post-fix: the supersede cancels the wedged connect → the localhost dial runs (green).
            await localDialed.Task.WaitAsync(TimeSpan.FromSeconds(10));
        }
    }
}
