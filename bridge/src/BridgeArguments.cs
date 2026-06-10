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

namespace com.IvanMurzak.Unreal.MCP.Bridge
{
    /// <summary>
    /// Launch arguments of the sidecar (docs/ARCHITECTURE.md §1.4). argv carries
    /// ONLY non-secrets: <c>--ipc-port=&lt;port&gt;</c> and <c>--parent-pid=&lt;pid&gt;</c>.
    /// The IPC auth token is delivered over stdin (one line), never on the command
    /// line — /proc cmdline, Windows Event 4688 and WER dumps all persist argv.
    /// </summary>
    public sealed class BridgeArguments
    {
        public const string IpcPortArg = "--ipc-port";
        public const string ParentPidArg = "--parent-pid";

        /// <summary>Localhost TCP port the UnrealMCP plugin listens on (the sidecar dials it).</summary>
        public int IpcPort { get; private set; }

        /// <summary>Unreal Editor process id — the sidecar self-exits when it vanishes (§6 orphan layer 2). 0 = not provided.</summary>
        public int ParentPid { get; private set; }

        public bool IsValid { get; private set; }
        public string? Error { get; private set; }

        private BridgeArguments() { }

        public static BridgeArguments Parse(IReadOnlyList<string> args)
        {
            var result = new BridgeArguments();
            int? ipcPort = null;
            int? parentPid = null;

            for (var i = 0; i < args.Count; i++)
            {
                var arg = args[i];
                if (TryReadValue(args, ref i, IpcPortArg, out var portValue))
                {
                    if (!int.TryParse(portValue, out var port) || port < 1 || port > 65535)
                        return Invalid($"Invalid {IpcPortArg} value '{portValue}': expected an integer in [1, 65535].");
                    ipcPort = port;
                }
                else if (TryReadValue(args, ref i, ParentPidArg, out var pidValue))
                {
                    if (!int.TryParse(pidValue, out var pid) || pid <= 0)
                        return Invalid($"Invalid {ParentPidArg} value '{pidValue}': expected a positive integer.");
                    parentPid = pid;
                }
                else if (arg.StartsWith("--", StringComparison.Ordinal))
                {
                    return Invalid($"Unknown argument '{arg}'. Supported: {IpcPortArg}=<port> {ParentPidArg}=<pid>");
                }
            }

            if (ipcPort == null)
                return Invalid($"Missing required argument {IpcPortArg}=<port>.");

            result.IpcPort = ipcPort.Value;
            result.ParentPid = parentPid ?? 0;
            result.IsValid = true;
            return result;

            BridgeArguments Invalid(string error)
            {
                result.IsValid = false;
                result.Error = error;
                return result;
            }
        }

        /// <summary>Accepts both <c>--name=value</c> and <c>--name value</c> forms.</summary>
        private static bool TryReadValue(IReadOnlyList<string> args, ref int index, string name, out string value)
        {
            var arg = args[index];
            if (arg.StartsWith(name + "=", StringComparison.Ordinal))
            {
                value = arg.Substring(name.Length + 1);
                return true;
            }
            if (arg == name && index + 1 < args.Count)
            {
                index++;
                value = args[index];
                return true;
            }
            value = string.Empty;
            return false;
        }
    }
}
