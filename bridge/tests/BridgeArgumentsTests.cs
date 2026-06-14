/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    public class BridgeArgumentsTests
    {
        [Fact]
        public void Parse_FullLaunchContract_Succeeds()
        {
            // The exact launch shape the plugin uses (docs/ARCHITECTURE.md §1.4):
            // unreal-mcp-bridge --ipc-port=31234 --parent-pid=<editor pid>
            var args = BridgeArguments.Parse(new[] { "--ipc-port=31234", "--parent-pid=4242" });

            Assert.True(args.IsValid);
            Assert.Null(args.Error);
            Assert.Equal(31234, args.IpcPort);
            Assert.Equal(4242, args.ParentPid);
        }

        [Fact]
        public void Parse_SpaceSeparatedValues_Succeeds()
        {
            var args = BridgeArguments.Parse(new[] { "--ipc-port", "30001", "--parent-pid", "77" });

            Assert.True(args.IsValid);
            Assert.Equal(30001, args.IpcPort);
            Assert.Equal(77, args.ParentPid);
        }

        [Fact]
        public void Parse_MissingIpcPort_IsInvalid()
        {
            var args = BridgeArguments.Parse(new[] { "--parent-pid=4242" });

            Assert.False(args.IsValid);
            Assert.Contains("--ipc-port", args.Error);
        }

        [Theory]
        [InlineData("--ipc-port=0")]
        [InlineData("--ipc-port=65536")]
        [InlineData("--ipc-port=not-a-number")]
        public void Parse_OutOfRangeOrNonNumericPort_IsInvalid(string portArg)
        {
            var args = BridgeArguments.Parse(new[] { portArg });

            Assert.False(args.IsValid);
            Assert.Contains("--ipc-port", args.Error);
        }

        [Fact]
        public void Parse_UnknownArgument_IsInvalid()
        {
            // The token must NEVER travel via argv — reject anything that looks like it.
            var args = BridgeArguments.Parse(new[] { "--ipc-port=31234", "--token=secret" });

            Assert.False(args.IsValid);
            Assert.Contains("--token", args.Error);
        }

        [Fact]
        public void Parse_ParentPidIsOptional_DefaultsToZero()
        {
            var args = BridgeArguments.Parse(new[] { "--ipc-port=31234" });

            Assert.True(args.IsValid);
            Assert.Equal(0, args.ParentPid);
        }

        [Fact]
        public void Parse_LogFileIsOptional_DefaultsToNull()
        {
            var args = BridgeArguments.Parse(new[] { "--ipc-port=31234" });

            Assert.True(args.IsValid);
            Assert.Null(args.LogFilePath);
        }

        [Fact]
        public void Parse_LogFilePath_IsCaptured()
        {
            // Issue #69: the plugin emits --log-file=<absolute path> (a non-secret). On Windows the path
            // may contain spaces; the OS argv splitter strips the surrounding quotes before we see it, so
            // a single argv element carries the full path.
            const string path = @"C:\My Project\Saved\Logs\UnrealMcpBridge.log";
            var args = BridgeArguments.Parse(new[] { "--ipc-port=31234", "--parent-pid=4242", $"--log-file={path}" });

            Assert.True(args.IsValid);
            Assert.Equal(path, args.LogFilePath);
        }

        [Fact]
        public void Parse_LogFileSpaceSeparated_IsCaptured()
        {
            var args = BridgeArguments.Parse(new[] { "--ipc-port=31234", "--log-file", "/tmp/bridge.log" });

            Assert.True(args.IsValid);
            Assert.Equal("/tmp/bridge.log", args.LogFilePath);
        }

        [Fact]
        public void Parse_EmptyLogFile_IsTreatedAsNull()
        {
            var args = BridgeArguments.Parse(new[] { "--ipc-port=31234", "--log-file=" });

            Assert.True(args.IsValid);
            Assert.Null(args.LogFilePath);
        }
    }
}
