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
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using com.IvanMurzak.Unreal.MCP.Bridge.Sidecar;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>The "backoff" xUnit target (docs/ARCHITECTURE.md §9.3, §1.5).</summary>
    public class ReconnectBackoffTests
    {
        [Fact]
        public void Schedule_FollowsOneTwoFiveTenThirtyCap()
        {
            var b = new ReconnectBackoff();
            Assert.Equal(1000, (int)b.Next().TotalMilliseconds);
            Assert.Equal(2000, (int)b.Next().TotalMilliseconds);
            Assert.Equal(5000, (int)b.Next().TotalMilliseconds);
            Assert.Equal(10000, (int)b.Next().TotalMilliseconds);
            Assert.Equal(30000, (int)b.Next().TotalMilliseconds);
            Assert.Equal(30000, (int)b.Next().TotalMilliseconds); // capped
            Assert.Equal(30000, (int)b.Next().TotalMilliseconds);
        }

        [Fact]
        public void Reset_ReturnsToFirstStep()
        {
            var b = new ReconnectBackoff();
            b.Next(); b.Next(); b.Next();
            b.Reset();
            Assert.Equal(0, b.Attempt);
            Assert.Equal(1000, (int)b.Peek().TotalMilliseconds);
        }
    }

    public class PendingCallRegistryTests
    {
        [Fact]
        public async Task CompletePending_ResolvesWaiter()
        {
            var reg = new PendingCallRegistry();
            var task = reg.Register("r1");
            Assert.Equal(1, reg.Count);

            var response = new ToolResponseMessage { RequestId = "r1", Status = IpcProtocol.Status.Success };
            Assert.True(reg.TryComplete("r1", response));
            Assert.Same(response, await task);
            Assert.Equal(0, reg.Count);
        }

        [Fact]
        public void CompleteUnknownId_IsDropped()
        {
            var reg = new PendingCallRegistry();
            Assert.False(reg.TryComplete("nope", new ToolResponseMessage()));
        }

        [Fact]
        public async Task FailAll_FailsEveryWaiterWithDisconnect()
        {
            var reg = new PendingCallRegistry();
            var t1 = reg.Register("r1");
            var t2 = reg.Register("r2");
            reg.FailAll();
            await Assert.ThrowsAsync<IpcDisconnectedException>(() => t1);
            await Assert.ThrowsAsync<IpcDisconnectedException>(() => t2);
            Assert.Equal(0, reg.Count);
        }

        [Fact]
        public async Task TryFail_FailsSingleWaiter()
        {
            var reg = new PendingCallRegistry();
            var t = reg.Register("r1");
            Assert.True(reg.TryFail("r1", new OperationCanceledException()));
            await Assert.ThrowsAsync<OperationCanceledException>(() => t);
        }
    }

    /// <summary>Orphan layer 3 (docs/ARCHITECTURE.md §1.4, §6).</summary>
    public class HandshakeFailureTrackerTests
    {
        [Fact]
        public void ThreeConsecutiveRejections_AreFatal()
        {
            var t = new HandshakeFailureTracker(DateTime.UtcNow);
            Assert.False(t.RecordRejection());
            Assert.False(t.RecordRejection());
            Assert.True(t.RecordRejection()); // 3rd → fatal
        }

        [Fact]
        public void Success_ResetsRejectionCount()
        {
            var t = new HandshakeFailureTracker(DateTime.UtcNow);
            t.RecordRejection();
            t.RecordRejection();
            t.RecordSuccess(DateTime.UtcNow);
            Assert.False(t.RecordRejection()); // count restarted
            Assert.Equal(1, t.ConsecutiveRejections);
        }

        [Fact]
        public void NoSuccessDeadline_ElapsesFromBoot()
        {
            var boot = DateTime.UtcNow;
            var t = new HandshakeFailureTracker(boot, TimeSpan.FromSeconds(60));
            Assert.False(t.IsNoSuccessDeadlineExceeded(boot.AddSeconds(59)));
            Assert.True(t.IsNoSuccessDeadlineExceeded(boot.AddSeconds(61)));
        }

        [Fact]
        public void Rejection_DoesNotResetNoSuccessDeadline()
        {
            var boot = DateTime.UtcNow;
            var t = new HandshakeFailureTracker(boot, TimeSpan.FromSeconds(60));
            t.RecordRejection(); // a rejected dial must NOT extend the deadline (§6)
            Assert.True(t.IsNoSuccessDeadlineExceeded(boot.AddSeconds(61)));
        }

        [Fact]
        public void AfterSuccess_DeadlineNeverFires()
        {
            var boot = DateTime.UtcNow;
            var t = new HandshakeFailureTracker(boot, TimeSpan.FromSeconds(60));
            t.RecordSuccess(boot.AddSeconds(5));
            Assert.False(t.IsNoSuccessDeadlineExceeded(boot.AddSeconds(600)));
        }
    }

    /// <summary>Orphan layer 2 (docs/ARCHITECTURE.md §6) — PID + start-time identity.</summary>
    public class ParentProcessMonitorTests
    {
        [Fact]
        public void Capture_ZeroPid_ReturnsNull()
        {
            Assert.Null(ParentProcessMonitor.Capture(0));
        }

        [Fact]
        public void CurrentProcess_IsAlive()
        {
            using var self = Process.GetCurrentProcess();
            var monitor = ParentProcessMonitor.Capture(self.Id);
            Assert.NotNull(monitor);
            Assert.True(monitor!.IsParentAlive());
        }

        [Fact]
        public void NonexistentPid_IsNotAlive()
        {
            // Capture a live process, then build a monitor for an almost-certainly-dead PID.
            var monitor = ParentProcessMonitor.Capture(2147483); // implausible pid
            // Capture() may return a monitor with a null start time (process not found at boot); a poll
            // then reports it dead. If the platform refused the id outright, Capture still yields a monitor.
            Assert.True(monitor == null || !monitor.IsParentAlive());
        }
    }
}
