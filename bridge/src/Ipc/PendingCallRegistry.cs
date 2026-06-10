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
using System.Threading.Tasks;
using com.IvanMurzak.Unreal.MCP.Bridge.Tools;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Ipc
{
    /// <summary>
    /// Correlates in-flight <c>tool-call</c>s with their <c>tool-response</c>s by <c>requestId</c>
    /// (docs/ARCHITECTURE.md §1.3, §2.2). Thread-safe. On IPC disconnect, <see cref="FailAll"/> completes
    /// every pending call with <see cref="IpcDisconnectedException"/> immediately rather than letting it
    /// hang to its timeout (§1.5, §2.2 step 4). A <c>tool-response</c> for an unknown / already-completed
    /// id is silently dropped (<see cref="TryComplete"/> returns false) — covering late responses that
    /// straddle a reconnect (§1.5).
    /// </summary>
    public sealed class PendingCallRegistry
    {
        private readonly object _gate = new();
        private readonly Dictionary<string, TaskCompletionSource<ToolResponseMessage>> _pending = new();

        public int Count
        {
            get { lock (_gate) return _pending.Count; }
        }

        /// <summary>
        /// Register a new pending call and return its completion task. The TCS runs continuations
        /// asynchronously so completing it from the reader loop never inlines proxy/handler code onto
        /// the reader thread.
        /// </summary>
        public Task<ToolResponseMessage> Register(string requestId)
        {
            var tcs = new TaskCompletionSource<ToolResponseMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
            lock (_gate)
            {
                // A duplicate id (should never happen — ids are unique per call) fails the prior waiter.
                if (_pending.TryGetValue(requestId, out var existing))
                    existing.TrySetException(new InvalidOperationException($"Duplicate requestId '{requestId}'."));
                _pending[requestId] = tcs;
            }
            return tcs.Task;
        }

        /// <summary>
        /// Complete a pending call with its response. Returns false (and drops the response) when no call
        /// with that id is pending — a late or duplicate response straddling a reconnect (§1.5).
        /// </summary>
        public bool TryComplete(string requestId, ToolResponseMessage response)
        {
            TaskCompletionSource<ToolResponseMessage>? tcs;
            lock (_gate)
            {
                if (!_pending.TryGetValue(requestId, out tcs))
                    return false;
                _pending.Remove(requestId);
            }
            return tcs.TrySetResult(response);
        }

        /// <summary>Cancel/forget a single pending call (e.g. the caller's CancellationToken fired).</summary>
        public bool TryFail(string requestId, Exception exception)
        {
            TaskCompletionSource<ToolResponseMessage>? tcs;
            lock (_gate)
            {
                if (!_pending.TryGetValue(requestId, out tcs))
                    return false;
                _pending.Remove(requestId);
            }
            return tcs.TrySetException(exception);
        }

        /// <summary>
        /// Fail every pending call at once (IPC link dropped, §1.5). Each waiter resolves immediately
        /// with <see cref="IpcDisconnectedException"/>.
        /// </summary>
        public void FailAll(Exception? exception = null)
        {
            List<TaskCompletionSource<ToolResponseMessage>> waiters;
            lock (_gate)
            {
                waiters = new List<TaskCompletionSource<ToolResponseMessage>>(_pending.Values);
                _pending.Clear();
            }
            var ex = exception ?? new IpcDisconnectedException();
            foreach (var tcs in waiters)
                tcs.TrySetException(ex);
        }
    }
}
