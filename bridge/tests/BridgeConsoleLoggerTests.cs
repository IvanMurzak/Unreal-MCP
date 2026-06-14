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
using System.IO;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using Microsoft.Extensions.Logging;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// Issue #69: the bridge tees its log to a file at a path the plugin passes via <c>--log-file=</c>,
    /// so the cloud-connection lifecycle is visible even though the plugin discards the child's stderr.
    /// These tests prove (a) lines land in the file when a path is set, (b) no path → no file created
    /// (stderr-only, unchanged), and (c) an unwritable/invalid path never throws out of the logging path.
    /// </summary>
    public class BridgeConsoleLoggerTests
    {
        [Fact]
        public void WithLogFile_WritesLinesToFile()
        {
            var dir = Path.Combine(Path.GetTempPath(), "unreal-mcp-bridge-tests", Guid.NewGuid().ToString("N"));
            // Parent directory deliberately does NOT exist yet — the sink must create it.
            var path = Path.Combine(dir, "nested", "UnrealMcpBridge.log");
            try
            {
                using (var provider = new BridgeConsoleLoggerProvider(LogLevel.Information, path))
                {
                    var logger = provider.CreateLogger("CloudConn");
                    logger.LogInformation("SignalR connected to {Host}.", "cloud.example");
                    logger.LogWarning("reconnecting (attempt {N}).", 2);
                    logger.LogDebug("this is below the min level and must NOT appear");
                }

                Assert.True(File.Exists(path), "the log file should have been created");
                var contents = File.ReadAllText(path);
                Assert.Contains("SignalR connected to cloud.example.", contents);
                Assert.Contains("reconnecting (attempt 2).", contents);
                Assert.Contains("[unreal-mcp-bridge]", contents);
                Assert.DoesNotContain("below the min level", contents);
            }
            finally
            {
                TryDeleteDir(Path.Combine(Path.GetTempPath(), "unreal-mcp-bridge-tests"));
            }
        }

        [Fact]
        public void WithLogFile_AppendsAcrossProviders()
        {
            // One file per editor session is reused (appended) across §1.5 crash-restarts: a second
            // provider over the same path must not truncate the first run's lines.
            var dir = Path.Combine(Path.GetTempPath(), "unreal-mcp-bridge-tests", Guid.NewGuid().ToString("N"));
            var path = Path.Combine(dir, "UnrealMcpBridge.log");
            try
            {
                using (var p1 = new BridgeConsoleLoggerProvider(LogLevel.Information, path))
                    p1.CreateLogger("Run1").LogInformation("first run line");
                using (var p2 = new BridgeConsoleLoggerProvider(LogLevel.Information, path))
                    p2.CreateLogger("Run2").LogInformation("second run line");

                var contents = File.ReadAllText(path);
                Assert.Contains("first run line", contents);
                Assert.Contains("second run line", contents);
            }
            finally
            {
                TryDeleteDir(Path.Combine(Path.GetTempPath(), "unreal-mcp-bridge-tests"));
            }
        }

        [Fact]
        public void WithoutLogFile_DoesNotCreateAnyFile()
        {
            // No path → stderr-only, unchanged behaviour. The logger must still function.
            using var provider = new BridgeConsoleLoggerProvider(LogLevel.Information, logFilePath: null);
            var logger = provider.CreateLogger("NoFile");

            var ex = Record.Exception(() => logger.LogInformation("stderr-only line"));
            Assert.Null(ex);
        }

        [Fact]
        public void WithBlankLogFile_DoesNotCreateAnyFile()
        {
            using var provider = new BridgeConsoleLoggerProvider(LogLevel.Information, logFilePath: "   ");
            var logger = provider.CreateLogger("Blank");

            var ex = Record.Exception(() => logger.LogInformation("stderr-only line"));
            Assert.Null(ex);
        }

        [Fact]
        public void WithUnwritablePath_DoesNotThrow()
        {
            // A directory path used as a file (or any invalid/unwritable target) must NOT throw — logging
            // must never crash the sidecar; it silently degrades to stderr-only.
            var dirAsFile = Path.Combine(Path.GetTempPath(), "unreal-mcp-bridge-tests-dir", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dirAsFile); // points the "log file" at an existing directory → AppendAllText throws internally
            try
            {
                BridgeConsoleLoggerProvider provider = null!;
                var ctorEx = Record.Exception(() => provider = new BridgeConsoleLoggerProvider(LogLevel.Information, dirAsFile));
                Assert.Null(ctorEx);

                var logger = provider.CreateLogger("Bad");
                var logEx = Record.Exception(() =>
                {
                    logger.LogInformation("line 1");
                    logger.LogError("line 2");
                });
                Assert.Null(logEx);
                provider.Dispose();
            }
            finally
            {
                TryDeleteDir(Path.Combine(Path.GetTempPath(), "unreal-mcp-bridge-tests-dir"));
            }
        }

        [Fact]
        public void WithInvalidPathCharacters_DoesNotThrow()
        {
            // A path that GetFullPath rejects must be swallowed at TryCreate (→ stderr-only), never thrown.
            const string invalid = "\0:bad|path?*";
            var ctorEx = Record.Exception(() =>
            {
                using var provider = new BridgeConsoleLoggerProvider(LogLevel.Information, invalid);
                provider.CreateLogger("Invalid").LogInformation("line");
            });
            Assert.Null(ctorEx);
        }

        private static void TryDeleteDir(string dir)
        {
            try
            {
                if (Directory.Exists(dir))
                    Directory.Delete(dir, recursive: true);
            }
            catch { /* best-effort cleanup */ }
        }
    }
}
