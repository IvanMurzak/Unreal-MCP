// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpSupervisedProcess.h"
#include "UnrealMcpLog.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"        // FPlatformTime::Seconds — explicit (not relying on a transitive editor-PCH include)
#include "Math/UnrealMathUtility.h"

namespace
{
	// §1.5 restart backoff schedule (seconds) + crash-rate circuit-breaker — identical across both managers.
	const int32 SupervisedBackoffSeconds[] = { 1, 2, 5, 10, 30 };
	constexpr int32 SupervisedMaxCrashesPerWindow = 5;
	constexpr double SupervisedCrashWindowSeconds = 300.0;
}

void FUnrealMcpSupervisedProcess::Start(
	TFunction<bool()> IsAlive, TFunction<bool()> Respawn, TFunction<void()> OnGiveUp, const TCHAR* Label)
{
	if (WatchdogFuture.IsValid())
		return;

	// Reset the crash window + stop flag for this run (the watchdog thread reads them below).
	bStopRequested = false;
	WindowStartSeconds = FPlatformTime::Seconds();
	CrashesInWindow = 0;

	const FString LabelStr = Label != nullptr ? FString(Label) : FString(TEXT("child process"));

	WatchdogFuture = Async(EAsyncExecution::Thread,
		[this, IsAlive = MoveTemp(IsAlive), Respawn = MoveTemp(Respawn), OnGiveUp = MoveTemp(OnGiveUp), LabelStr]()
	{
		int32 BackoffIndex = 0;
		while (!bStopRequested)
		{
			FPlatformProcess::Sleep(1.0f);
			if (bStopRequested)
				break;

			if (IsAlive())
			{
				BackoffIndex = 0;
				continue;
			}

			// Child died unexpectedly — crash-restart with backoff + a crash-rate guard.
			const double Now = FPlatformTime::Seconds();
			if (Now - WindowStartSeconds > SupervisedCrashWindowSeconds)
			{
				WindowStartSeconds = Now;
				CrashesInWindow = 0;
			}
			if (++CrashesInWindow > SupervisedMaxCrashesPerWindow)
			{
				UE_LOG(LogUnrealMcp, Error,
					TEXT("[Unreal-MCP] %s crashed > %d times in %.0fs; giving up auto-restart."),
					*LabelStr, SupervisedMaxCrashesPerWindow, SupervisedCrashWindowSeconds);
				if (OnGiveUp)
					OnGiveUp();
				return;
			}

			const int32 DelaySeconds = SupervisedBackoffSeconds[FMath::Min(BackoffIndex, (int32)UE_ARRAY_COUNT(SupervisedBackoffSeconds) - 1)];
			++BackoffIndex;
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] %s exited; restarting in %ds (restart #%d)."),
				*LabelStr, DelaySeconds, RestartCount.GetValue() + 1);

			// Chunk the backoff so editor shutdown (which joins this thread) is not stalled by one long sleep.
			const double BackoffUntil = FPlatformTime::Seconds() + DelaySeconds;
			while (!bStopRequested && FPlatformTime::Seconds() < BackoffUntil)
				FPlatformProcess::Sleep(0.5f);
			if (bStopRequested)
				break;

			if (Respawn())
				RestartCount.Increment();
		}
	});
}

void FUnrealMcpSupervisedProcess::Stop()
{
	bStopRequested = true;
	if (WatchdogFuture.IsValid())
	{
		WatchdogFuture.Wait();
		WatchdogFuture = TFuture<void>();
	}
}
