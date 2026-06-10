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
    /// Orphan-prevention layer 3 (docs/ARCHITECTURE.md §1.4, §6): the sidecar self-exits when it cannot
    /// achieve a SUCCESSFUL handshake. Two independent triggers, both modelled here (pure, unit-tested):
    /// <list type="bullet">
    ///   <item><b>Consecutive-rejection fatal</b>: after <see cref="MaxConsecutiveRejections"/> (3)
    ///   rejected handshakes in a row, the sidecar exits instead of re-dialing forever (§1.4).</item>
    ///   <item><b>No-success deadline</b>: 60 s without a successful handshake → exit. A TCP connect whose
    ///   handshake is REJECTED does NOT reset that deadline (otherwise a stale-token orphan re-dialing a
    ///   relaunched editor would keep itself alive forever, §6).</item>
    /// </list>
    /// A successful handshake clears both. The owning watchdog loop polls <see cref="IsNoSuccessDeadlineExceeded"/>.
    /// </summary>
    public sealed class HandshakeFailureTracker
    {
        public const int MaxConsecutiveRejections = 3;
        public static readonly TimeSpan NoSuccessDeadline = TimeSpan.FromSeconds(60);

        private readonly TimeSpan _deadline;
        private int _consecutiveRejections;
        private DateTime? _lastSuccessUtc;

        /// <param name="bootUtc">When the sidecar started — the no-success deadline runs from here until the first success.</param>
        /// <param name="deadline">Override the 60 s deadline (tests).</param>
        public HandshakeFailureTracker(DateTime bootUtc, TimeSpan? deadline = null)
        {
            _deadline = deadline ?? NoSuccessDeadline;
            _lastSuccessUtc = null;
            BootUtc = bootUtc;
        }

        public DateTime BootUtc { get; }
        public int ConsecutiveRejections => _consecutiveRejections;
        public bool HasEverSucceeded => _lastSuccessUtc.HasValue;

        /// <summary>Record a successful handshake — clears the rejection count and the no-success deadline.</summary>
        public void RecordSuccess(DateTime nowUtc)
        {
            _consecutiveRejections = 0;
            _lastSuccessUtc = nowUtc;
        }

        /// <summary>
        /// Record a rejected handshake. Returns true when the consecutive-rejection cap is reached and the
        /// sidecar must exit. Does NOT touch the no-success deadline (deliberately, §6).
        /// </summary>
        public bool RecordRejection()
        {
            _consecutiveRejections++;
            return _consecutiveRejections >= MaxConsecutiveRejections;
        }

        /// <summary>
        /// True when the sidecar has never had a successful handshake and the deadline elapsed since boot.
        /// Once any handshake has succeeded this is permanently false (the parent-process watchdog covers
        /// the established-then-lost case).
        /// </summary>
        public bool IsNoSuccessDeadlineExceeded(DateTime nowUtc)
        {
            if (_lastSuccessUtc.HasValue)
                return false;
            return nowUtc - BootUtc >= _deadline;
        }
    }
}
