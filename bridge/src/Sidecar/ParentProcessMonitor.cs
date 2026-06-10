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
using System.Diagnostics;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Sidecar
{
    /// <summary>
    /// Orphan-prevention layer 2 (docs/ARCHITECTURE.md §6): the sidecar self-exits when the editor
    /// process identified by <c>--parent-pid</c> vanishes. The identity is the PID <b>plus its process
    /// start time</b> captured at sidecar boot — so PID reuse (a different process inheriting the same
    /// id) cannot silently keep the sidecar alive. Poll <see cref="IsParentAlive"/> every 2 s; a false
    /// result means the editor is gone (or was replaced) → exit.
    /// </summary>
    public sealed class ParentProcessMonitor
    {
        private readonly int _parentPid;
        private readonly DateTime? _expectedStartTimeUtc;

        private ParentProcessMonitor(int parentPid, DateTime? expectedStartTimeUtc)
        {
            _parentPid = parentPid;
            _expectedStartTimeUtc = expectedStartTimeUtc;
        }

        public int ParentPid => _parentPid;
        public DateTime? ExpectedStartTimeUtc => _expectedStartTimeUtc;

        /// <summary>
        /// Capture the parent's identity at boot. Returns null when <paramref name="parentPid"/> is not
        /// provided (0) — the caller then relies on the handshake watchdog (layer 3) alone. If the PID is
        /// already gone at boot, the monitor is still created (start time unknown) and the first poll will
        /// report the parent dead.
        /// </summary>
        public static ParentProcessMonitor? Capture(int parentPid)
        {
            if (parentPid <= 0)
                return null;

            DateTime? startTime = null;
            try
            {
                using var parent = Process.GetProcessById(parentPid);
                startTime = parent.StartTime.ToUniversalTime();
            }
            catch
            {
                // Parent already gone or inaccessible — leave start time null; IsParentAlive() will report dead.
            }
            return new ParentProcessMonitor(parentPid, startTime);
        }

        /// <summary>
        /// True only when a process with the expected PID exists AND (when a start time was captured) its
        /// start time still matches. Any exception (process gone) is treated as "not alive".
        /// </summary>
        public bool IsParentAlive()
        {
            try
            {
                using var parent = Process.GetProcessById(_parentPid);
                if (parent.HasExited)
                    return false;
                if (_expectedStartTimeUtc is { } expected)
                    return parent.StartTime.ToUniversalTime() == expected;
                return true;
            }
            catch
            {
                return false;
            }
        }
    }
}
