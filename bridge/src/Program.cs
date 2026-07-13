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
using System.Threading;
using System.Threading.Tasks;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Sidecar;
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge
{
    /// <summary>
    /// The <c>unreal-mcp-bridge</c> sidecar entry point (docs/ARCHITECTURE.md §0, §1, §6). Parses the
    /// launch contract (§1.4), reads the one-shot IPC token from stdin, dials the plugin's IPC listener,
    /// hosts the reused McpPlugin SignalR client, registers the plugin's tool manifest as ProxyTools, and
    /// runs the three no-orphan watchdog layers (§6). Exits cleanly on plugin <c>shutdown</c>, parent-editor
    /// death, or a no-handshake deadline.
    /// </summary>
    public static class Program
    {
        /// <summary>Single-sourced sidecar version (kept in lockstep with the csproj <c>&lt;Version&gt;</c>, §9.2).</summary>
        public const string SidecarVersion = "0.1.0";

        public static async Task<int> Main(string[] args)
        {
            var arguments = BridgeArguments.Parse(args);
            if (!arguments.IsValid)
            {
                Console.Error.WriteLine($"[unreal-mcp-bridge] {arguments.Error}");
                Console.Error.WriteLine($"Usage: unreal-mcp-bridge {BridgeArguments.IpcPortArg}=<port> {BridgeArguments.ParentPidArg}=<editor pid>");
                return 2;
            }

            // §1.4: the one-shot IPC auth token arrives as exactly one line on stdin (never argv). It is a
            // secret — only its PRESENCE may ever be logged.
            var token = Console.IsInputRedirected ? Console.In.ReadLine() : null;
            var tokenPresent = !string.IsNullOrWhiteSpace(token);

            var loggerProvider = new BridgeConsoleLoggerProvider(ResolveLogLevel(), arguments.LogFilePath);
            var logger = loggerProvider.CreateLogger("Program");

            // A per-start header line so a single appended log file (one per editor session, reused across
            // §1.5 crash-restarts) makes restarts distinguishable — timestamp + pid + resolved cloud host.
            var cloudHost = Environment.GetEnvironmentVariable("UNREAL_MCP_HOST")
                            ?? Environment.GetEnvironmentVariable("UNREAL_MCP_CLOUD_URL")
                            ?? "(default cloud)";
            logger.LogInformation(
                "=== unreal-mcp-bridge start {StartUtc:o} v{Version} pid={Pid} host={Host} ===",
                DateTime.UtcNow, SidecarVersion, Environment.ProcessId, cloudHost);

            logger.LogInformation(
                "Sidecar v{Version} starting — ipc-port={Port}, parent-pid={Pid}, token={TokenState}.",
                SidecarVersion, arguments.IpcPort, arguments.ParentPid, tokenPresent ? "received" : "absent");

            if (!tokenPresent)
                logger.LogWarning("No IPC token on stdin; the plugin will reject the handshake (orphan layer 3 will then exit the sidecar).");

            // Distinguish a clean exit (plugin `shutdown`, Ctrl+C, parent-editor-gone orphan avoidance) from a
            // layer-3 GIVE-UP (handshake rejected repeatedly / no successful handshake within the deadline) via
            // the process exit code, so the launcher/watchdog can tell "gave up" from "clean exit".
            var exit = new ExitState();

            using var cts = new CancellationTokenSource();
            Console.CancelKeyPress += (_, e) => { e.Cancel = true; cts.Cancel(); };

            await using var ipc = new IpcClient(
                host: "127.0.0.1",
                port: arguments.IpcPort,
                token: token ?? string.Empty,
                sidecarVersion: SidecarVersion,
                logger: loggerProvider.CreateLogger(nameof(IpcClient)));

            using var host = new SidecarHost(
                ipc,
                SidecarVersion,
                loggerProvider,
                fallbackHost: Environment.GetEnvironmentVariable("UNREAL_MCP_HOST")
                              ?? Environment.GetEnvironmentVariable("UNREAL_MCP_CLOUD_URL"),
                fallbackToken: Environment.GetEnvironmentVariable("UNREAL_MCP_TOKEN"),
                // mcp-authorize (D12): the shared machine credential store (~/.ai-game-dev/credentials.json, 0600 /
                // DPAPI) — read by the engine plugins, the CLIs, and the local server so sign-in is once-per-machine.
                // Its presence enables the device-code sign-in, boot auto-adopt (zero-button), and proactive/on-401
                // refresh. Left default (~/.ai-game-dev); the xUnit suite injects a temp dir instead.
                credentialStore: new MachineCredentialStore());
            host.Build();

            // No-orphan watchdogs (§6) ------------------------------------------------------------------
            var bootUtc = DateTime.UtcNow;
            var handshakeTracker = new HandshakeFailureTracker(bootUtc);
            var parentMonitor = ParentProcessMonitor.Capture(arguments.ParentPid); // layer 2 (null when no pid)

            ipc.HandshakeAccepted += _ => handshakeTracker.RecordSuccess(DateTime.UtcNow);
            ipc.HandshakeRejected += () =>
            {
                if (handshakeTracker.RecordRejection())
                {
                    logger.LogError("Handshake rejected {Count} times consecutively; exiting (orphan layer 3).",
                        HandshakeFailureTracker.MaxConsecutiveRejections);
                    exit.Code = ExitCodes.GaveUp;
                    cts.Cancel();
                }
            };
            ipc.ShutdownRequested += () =>
            {
                logger.LogInformation("Plugin requested shutdown; exiting.");
                cts.Cancel();
            };

            var ipcRun = ipc.RunAsync(cts.Token);
            var watchdog = RunWatchdogAsync(parentMonitor, handshakeTracker, logger, cts, exit);

            await Task.WhenAny(ipcRun, watchdog).ConfigureAwait(false);
            cts.Cancel();
            await SafeAwait(ipcRun).ConfigureAwait(false);
            await SafeAwait(watchdog).ConfigureAwait(false);

            logger.LogInformation("Sidecar exiting (code {Code}).", exit.Code);
            return exit.Code;
        }

        /// <summary>
        /// Layer 2 + 3 poll loop (§6): every 2 s, exit if the parent editor (PID + start time) vanished, or
        /// if the no-successful-handshake deadline (60 s) elapsed.
        /// </summary>
        private static async Task RunWatchdogAsync(
            ParentProcessMonitor? parentMonitor,
            HandshakeFailureTracker handshakeTracker,
            ILogger logger,
            CancellationTokenSource cts,
            ExitState exit)
        {
            try
            {
                while (!cts.IsCancellationRequested)
                {
                    await Task.Delay(2000, cts.Token).ConfigureAwait(false);

                    if (parentMonitor != null && !parentMonitor.IsParentAlive())
                    {
                        logger.LogInformation("Parent editor (pid {Pid}) is gone; exiting (orphan layer 2).", parentMonitor.ParentPid);
                        cts.Cancel();
                        return;
                    }

                    if (handshakeTracker.IsNoSuccessDeadlineExceeded(DateTime.UtcNow))
                    {
                        logger.LogError("No successful handshake within {Seconds} s; exiting (orphan layer 3).",
                            (int)HandshakeFailureTracker.NoSuccessDeadline.TotalSeconds);
                        exit.Code = ExitCodes.GaveUp;
                        cts.Cancel();
                        return;
                    }
                }
            }
            catch (OperationCanceledException) { /* shutting down */ }
        }

        private static LogLevel ResolveLogLevel()
        {
            var raw = Environment.GetEnvironmentVariable("UNREAL_MCP_LOG_LEVEL");
            return raw?.Trim().ToLowerInvariant() switch
            {
                "trace" => LogLevel.Trace,
                "debug" => LogLevel.Debug,
                "info" or "information" => LogLevel.Information,
                "warn" or "warning" => LogLevel.Warning,
                "error" => LogLevel.Error,
                _ => LogLevel.Information,
            };
        }

        private static async Task SafeAwait(Task task)
        {
            try { await task.ConfigureAwait(false); } catch { /* swallow on teardown */ }
        }

        /// <summary>Process exit codes (§6). 0 = clean; non-zero distinguishes a layer-3 give-up.</summary>
        private static class ExitCodes
        {
            public const int Clean = 0;
            public const int GaveUp = 3; // handshake rejected repeatedly, or no successful handshake by the deadline
        }

        /// <summary>Mutable holder for the terminal exit code, shared between Main and the watchdog loop.</summary>
        private sealed class ExitState
        {
            public int Code = ExitCodes.Clean;
        }
    }
}
