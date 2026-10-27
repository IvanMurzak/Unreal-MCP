// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpGameThreadDispatcher.h"
#include "UnrealMcpLog.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"
#include "Containers/Ticker.h"

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

namespace
{
	/** One dispatched call: runs on the game thread, again on a later tick while the body asks to retry. */
	struct FUnrealMcpDispatchJob : TSharedFromThis<FUnrealMcpDispatchJob, ESPMode::ThreadSafe>
	{
		TSharedRef<FDispatchState, ESPMode::ThreadSafe> State;
		FUnrealMcpToolCall Call;
		TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)> Body;

		FUnrealMcpDispatchJob(TSharedRef<FDispatchState, ESPMode::ThreadSafe> InState, FUnrealMcpToolCall InCall,
			TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)> InBody)
			: State(MoveTemp(InState)), Call(MoveTemp(InCall)), Body(MoveTemp(InBody))
		{
		}

		void Run()
		{
			// The caller already timed out or cancelled: never run it late.
			if (State->bCompleted || Call.IsCancelled())
			{
				State->Complete(FUnrealMcpToolResult::Error(TEXT("Tool call was cancelled before it started; it was not run.")));
				return;
			}

			FUnrealMcpToolResult Result = Body(Call);
			if (!Result.bRetryLater)
			{
				State->Complete(MoveTemp(Result));
				return;
			}

			// Not ready (e.g. Python before its first tick). A ticker runs on a later frame, so the engine keeps
			// ticking in between; the watchdog bounds the wait.
			UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] tool call not ready, retrying: %s"), *Result.Message);
			Schedule(0.1f);
		}

		/**
		 * Run on the main loop's core ticker (thread-safe to add to). A game-thread AsyncTask would run the body INSIDE
		 * the task graph's game-thread queue, and a body that waits on game-thread tasks itself (an Interchange import
		 * from python-execute, an asset compile) then trips the task graph's recursion guard and crashes the editor.
		 */
		void Schedule(float Delay = 0.f)
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Self = AsShared()](float)
			{
				Self->Run();
				return false;
			}), Delay);
		}
	};
}

TFuture<FUnrealMcpToolResult> FUnrealMcpGameThreadDispatcher::Dispatch(
	FUnrealMcpToolCall Call,
	TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)> Body,
	FTimespan Timeout)
{
	TSharedRef<FDispatchState, ESPMode::ThreadSafe> State = MakeShared<FDispatchState, ESPMode::ThreadSafe>();
	TFuture<FUnrealMcpToolResult> Future = State->Promise.GetFuture();
	const uint32 TimeoutMs = static_cast<uint32>(
		FMath::Clamp<int64>(static_cast<int64>(Timeout.GetTotalMilliseconds()), 1, static_cast<int64>(MAX_uint32)));
	TSharedPtr<const FThreadSafeBool, ESPMode::ThreadSafe> CancelFlag = Call.CancelRequested;
	TSharedRef<FUnrealMcpDispatchJob, ESPMode::ThreadSafe> Job =
		MakeShared<FUnrealMcpDispatchJob, ESPMode::ThreadSafe>(State, MoveTemp(Call), MoveTemp(Body));

	if (IsInGameThread())
	{
		Job->Run();
		if (!State->bCompleted) // the body asked to retry on a later tick
			FUnrealMcpDispatchWatchdog::Get().Watch(State, CancelFlag, TimeoutMs);
		return Future;
	}

	// Arm the deadline before posting so a body that finishes instantly is simply pruned by the watchdog.
	FUnrealMcpDispatchWatchdog::Get().Watch(State, CancelFlag, TimeoutMs);
	Job->Schedule();
	return Future;
}
