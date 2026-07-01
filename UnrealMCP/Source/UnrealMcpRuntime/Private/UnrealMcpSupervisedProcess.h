// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "Async/Future.h"
#include "Templates/Function.h"

/**
 * A shared crash-restart WATCHDOG for an external child process (docs/ARCHITECTURE.md §1.5 / §6).
 *
 * The local-server manager (§7) and the sidecar manager (§6) carried byte-identical watchdog loops — a
 * background thread that polls liveness every second and, when the child died unexpectedly, relaunches it with
 * an exponential backoff ({1,2,5,10,30}s) under a crash-rate circuit-breaker (give up after 5 crashes in a
 * 300s window). This class owns that loop + its state ONCE; each manager supplies three callbacks for the
 * parts that genuinely differ:
 *
 *   - @c IsAlive   — whether the supervised child is currently running (each manager guards its own handle
 *                    with its own locking model, so the liveness probe is injected, not shared).
 *   - @c Respawn   — relaunch the child + any per-manager cleanup (kill an orphan on the port, rotate the IPC
 *                    token, close the dead handle, …). Returns true when the child was relaunched; the watchdog
 *                    bumps the restart counter on true. Runs only after the backoff elapses.
 *   - @c OnGiveUp  — runs once if the crash threshold is exceeded (e.g. clear the pid file). May be null.
 *
 * Every callback runs ON the watchdog thread. The backoff sleep is chunked on a short tick so Stop() (called
 * on the game thread at editor shutdown, which joins this thread) is never stalled for the full backoff.
 */
class UNREALMCPRUNTIME_API FUnrealMcpSupervisedProcess
{
public:
	~FUnrealMcpSupervisedProcess() { Stop(); }

	/**
	 * Launch the watchdog thread (no-op if already running). Resets the crash-window counters + the stop flag,
	 * then polls @p IsAlive every second; on an unexpected death it backs off and calls @p Respawn, or calls
	 * @p OnGiveUp once and exits when the circuit-breaker trips. @p Label names the child in log lines
	 * (e.g. "local server" / "sidecar").
	 */
	void Start(TFunction<bool()> IsAlive, TFunction<bool()> Respawn, TFunction<void()> OnGiveUp, const TCHAR* Label);

	/** Signal the watchdog to stop and JOIN it (idempotent; safe to call when not running). */
	void Stop();

	/** True while the watchdog thread is live. */
	bool IsWatching() const { return WatchdogFuture.IsValid(); }

	/** Number of successful relaunches since construction (the §1.5 restart counter the managers expose). */
	int32 GetRestartCount() const { return RestartCount.GetValue(); }

	/** Reset the restart counter (the local-server manager zeroes it on a fresh Start). */
	void ResetRestartCount() { RestartCount.Reset(); }

private:
	FThreadSafeBool bStopRequested = false;
	FThreadSafeCounter RestartCount;
	TFuture<void> WatchdogFuture;
	double WindowStartSeconds = 0.0;
	int32 CrashesInWindow = 0;
};
