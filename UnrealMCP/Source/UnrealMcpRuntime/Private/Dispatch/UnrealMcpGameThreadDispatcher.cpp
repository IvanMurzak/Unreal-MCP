// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpGameThreadDispatcher.h"
#include "UnrealMcpLog.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"

#include <atomic>

namespace
{
	/**
	 * Shared completion state for one dispatched call. The promise is completed by whichever path wins
	 * the atomic guard first — the game-thread body or the timeout watchdog — so it is never set twice.
	 */
	struct FDispatchState
	{
		TPromise<FUnrealMcpToolResult> Promise;
		std::atomic<bool> bCompleted{ false };

		void Complete(FUnrealMcpToolResult&& Result)
		{
			bool Expected = false;
			if (bCompleted.compare_exchange_strong(Expected, true))
				Promise.SetValue(MoveTemp(Result));
		}
	};

	/**
	 * One thread that enforces every pending call's deadline. Tool calls may run for minutes, so a
	 * per-call waiter on the shared GThreadPool would hold pool workers (also used for asset and DDC
	 * work) for the whole call. Process-lifetime: created on first use and never destroyed, so a late
	 * deadline can never touch a freed watchdog; it stops on engine pre-exit.
	 */
	class FUnrealMcpDispatchWatchdog final : public FRunnable
	{
	public:
		static FUnrealMcpDispatchWatchdog& Get()
		{
			static FUnrealMcpDispatchWatchdog* Instance = new FUnrealMcpDispatchWatchdog();
			return *Instance;
		}

		/** Complete @p State with a timeout error (and set @p CancelFlag) unless it completes first. */
		void Watch(const TSharedRef<FDispatchState, ESPMode::ThreadSafe>& State,
			TSharedPtr<const FThreadSafeBool, ESPMode::ThreadSafe> CancelFlag, uint32 TimeoutMs)
		{
			{
				FScopeLock Lock(&Mutex);
				Entries.Add({ FPlatformTime::Seconds() + TimeoutMs / 1000.0, TimeoutMs, State, MoveTemp(CancelFlag) });
			}
			Wake->Trigger();
		}

		virtual uint32 Run() override
		{
			while (!bStop)
			{
				TArray<FEntry> Expired;
				double NextDeadline = FPlatformTime::Seconds() + 1.0;
				{
					FScopeLock Lock(&Mutex);
					const double Now = FPlatformTime::Seconds();
					for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
					{
						FEntry& Entry = Entries[Index];
						if (Entry.State->bCompleted)
							Entries.RemoveAtSwap(Index);
						else if (Entry.Deadline <= Now)
						{
							Expired.Add(MoveTemp(Entry));
							Entries.RemoveAtSwap(Index);
						}
						else
							NextDeadline = FMath::Min(NextDeadline, Entry.Deadline);
					}
				}

				// Outside the lock: completing the promise runs the bridge's continuation on this thread.
				for (FEntry& Entry : Expired)
				{
					// Cooperative cancel so a still-running body can bail at IsCancelled() (§4), and a body
					// still queued is skipped. The shared flag outlives the bridge's CancelFlags entry.
					if (Entry.CancelFlag.IsValid())
						const_cast<FThreadSafeBool&>(*Entry.CancelFlag) = true;
					Entry.State->Complete(FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Tool call timed out after %u ms."), Entry.TimeoutMs)));
				}

				const double WaitSeconds = FMath::Max(NextDeadline - FPlatformTime::Seconds(), 0.0);
				Wake->Wait(static_cast<uint32>(FMath::CeilToDouble(WaitSeconds * 1000.0)));
			}
			return 0;
		}

		/** False when the platform has no threads; then deadlines are not enforced. */
		bool IsRunning() const { return Thread != nullptr; }

	private:
		struct FEntry
		{
			double Deadline;
			uint32 TimeoutMs;
			TSharedRef<FDispatchState, ESPMode::ThreadSafe> State;
			TSharedPtr<const FThreadSafeBool, ESPMode::ThreadSafe> CancelFlag;
		};

		FUnrealMcpDispatchWatchdog()
			: Wake(FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false))
		{
			if (FPlatformProcess::SupportsMultithreading())
				Thread = FRunnableThread::Create(this, TEXT("UnrealMcpDispatchWatchdog"), 64 * 1024, TPri_BelowNormal);
			if (Thread == nullptr)
				UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] no watchdog thread; tool-call timeouts are not enforced."));
			FCoreDelegates::OnEnginePreExit.AddLambda([this]()
			{
				bStop = true;
				Wake->Trigger();
			});
		}

		FCriticalSection Mutex;
		TArray<FEntry> Entries;
		FEvent* Wake;
		FRunnableThread* Thread = nullptr;
		std::atomic<bool> bStop{ false };
	};
}

TFuture<FUnrealMcpToolResult> FUnrealMcpGameThreadDispatcher::Dispatch(
	FUnrealMcpToolCall Call,
	TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)> Body,
	FTimespan Timeout)
{
	TSharedRef<FDispatchState, ESPMode::ThreadSafe> State = MakeShared<FDispatchState, ESPMode::ThreadSafe>();
	TFuture<FUnrealMcpToolResult> Future = State->Promise.GetFuture();

	auto RunBody = [State, Body = MoveTemp(Body), Call]() mutable
	{
		// The caller already timed out or cancelled while this call was queued: never run it late.
		const bool bAbandoned = State->bCompleted || Call.IsCancelled();
		FUnrealMcpToolResult Result = bAbandoned
			? FUnrealMcpToolResult::Error(TEXT("Tool call was cancelled before it started; it was not run."))
			: Body(Call);
		State->Complete(MoveTemp(Result));
	};

	if (IsInGameThread())
	{
		RunBody(); // completes the promise synchronously; nothing left to time out.
		return Future;
	}

	// Arm the deadline before posting so a body that finishes instantly is simply pruned by the watchdog.
	const uint32 TimeoutMs = static_cast<uint32>(
		FMath::Clamp<int64>(static_cast<int64>(Timeout.GetTotalMilliseconds()), 1, static_cast<int64>(MAX_uint32)));
	FUnrealMcpDispatchWatchdog::Get().Watch(State, Call.CancelRequested, TimeoutMs);

	AsyncTask(ENamedThreads::GameThread, [RunBody = MoveTemp(RunBody)]() mutable { RunBody(); });
	return Future;
}
