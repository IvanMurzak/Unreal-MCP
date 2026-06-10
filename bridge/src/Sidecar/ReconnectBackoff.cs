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

namespace com.IvanMurzak.Unreal.MCP.Bridge.Sidecar
{
    /// <summary>
    /// The IPC reconnect/backoff schedule (docs/ARCHITECTURE.md §1.5): 1 s, 2 s, 5 s, 10 s, 30 s (cap).
    /// Pure and deterministic so the schedule is unit-tested directly (the "backoff" xUnit row, §9.3).
    /// <see cref="Next"/> advances the delay; <see cref="Reset"/> returns to the first step after a
    /// successful (re)connect.
    /// </summary>
    public sealed class ReconnectBackoff
    {
        private static readonly int[] ScheduleMs = { 1000, 2000, 5000, 10000, 30000 };
        private int _attempt;

        /// <summary>Number of times <see cref="Next"/> has been called since the last <see cref="Reset"/>.</summary>
        public int Attempt => _attempt;

        /// <summary>
        /// The delay for the current attempt, then advance. Clamps to the final (30 s) step once the
        /// schedule is exhausted.
        /// </summary>
        public TimeSpan Next()
        {
            var index = Math.Min(_attempt, ScheduleMs.Length - 1);
            _attempt++;
            return TimeSpan.FromMilliseconds(ScheduleMs[index]);
        }

        /// <summary>Peek the delay <see cref="Next"/> would return without advancing.</summary>
        public TimeSpan Peek()
        {
            var index = Math.Min(_attempt, ScheduleMs.Length - 1);
            return TimeSpan.FromMilliseconds(ScheduleMs[index]);
        }

        public void Reset() => _attempt = 0;
    }
}
