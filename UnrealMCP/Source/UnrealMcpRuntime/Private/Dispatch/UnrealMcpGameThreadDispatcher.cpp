// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpGameThreadDispatcher.h"
#include "UnrealMcpLog.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"
#include "CoreGlobals.h"

#include <atomic>

namespace
{
	/** FPlatformTime::Seconds() of the last end of a game-thread frame the dispatcher saw (0: none yet). */
	std::atomic<double> GUnrealMcpLastEndOfFrame{ 0.0 };

	/** Why a call ended before its body returned, for the agent: whether it ran, and what holds the game thread. */
	FString UnrealMcpDescribeEndedCall(const FString& Lead, bool bStarted)
	{
		FString Text = Lead + (bStarted
			? TEXT(" The call had started on the game thread and is still running; its outcome is unknown (nothing is undone).")
			: TEXT(" The call never started and will not run."));
		const double LastFrame = GUnrealMcpLastEndOfFrame.load();
		const double Stalled = LastFrame > 0.0 ? FPlatformTime::Seconds() - LastFrame : 0.0;
		if (Stalled >= 5.0)
			Text += FString::Printf(TEXT(" The editor's game thread has not finished a frame for %.0f s: a long operation or a dialog")
				TEXT(" waiting for an answer in the editor holds it, and no tool call can run until it ends."), Stalled);
		return Text;
	}

	/**
	 * FCoreDelegates are not thread-safe, and the process-lifetime helpers below are created lazily by the first
	 * Dispatch(), which may come from a worker thread (UE 5.8's access detector ensures on that data race). Add their
	 * subscriptions on the game thread: inline when already there, else posted (a trivial delegate add, not a tool body).
	 */
	void SubscribeOnGameThread(TUniqueFunction<void()> Subscribe)
	{
		if (IsInGameThread())
			Subscribe();
		else
			AsyncTask(ENamedThreads::GameThread, MoveTemp(Subscribe));
	}

	/**
	 * Shared completion state for one dispatched call. The promise is completed by whichever path wins
	 * the atomic guard first — the game-thread body or the timeout watchdog — so it is never set twice.
	 */
	struct FDispatchState
	{
		TPromise<FUnrealMcpToolResult> Promise;
		std::atomic<bool> bCompleted{ false };
		/** The body began on the game thread: an early end leaves its outcome unknown (it is not undone). */
		std::atomic<bool> bStarted{ false };

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
					Entry.End(FString::Printf(TEXT("Tool call timed out after %u ms."), Entry.TimeoutMs));

				const double WaitSeconds = FMath::Max(NextDeadline - FPlatformTime::Seconds(), 0.0);
				Wake->Wait(static_cast<uint32>(FMath::CeilToDouble(WaitSeconds * 1000.0)));
			}
			return 0;
		}

		/** End every call still waiting (queued or running) now, as an early timeout would; returns how many. */
		int32 FailAll(const FString& Reason)
		{
			TArray<FEntry> Ended;
			{
				FScopeLock Lock(&Mutex);
				for (FEntry& Entry : Entries)
				{
					if (!Entry.State->bCompleted)
						Ended.Add(MoveTemp(Entry));
				}
				Entries.Reset();
			}
			// Outside the lock, as Run does: completing runs the bridge's continuation on this thread.
			for (FEntry& Entry : Ended)
				Entry.End(Reason);
			return Ended.Num();
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

			/** End the call before its body returned (timeout or FailAll) with @p Lead as the reason. */
			void End(const FString& Lead)
			{
				// Cooperative cancel so a still-running body can bail at IsCancelled() (§4), and a body
				// still queued is skipped. The shared flag outlives the bridge's CancelFlags entry.
				if (CancelFlag.IsValid())
					const_cast<FThreadSafeBool&>(*CancelFlag) = true;
				State->Complete(FUnrealMcpToolResult::Error(UnrealMcpDescribeEndedCall(Lead, State->bStarted)));
			}
		};

		FUnrealMcpDispatchWatchdog()
			: Wake(FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false))
		{
			if (FPlatformProcess::SupportsMultithreading())
				Thread = FRunnableThread::Create(this, TEXT("UnrealMcpDispatchWatchdog"), 64 * 1024, TPri_BelowNormal);
			if (Thread == nullptr)
				UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] no watchdog thread; tool-call timeouts are not enforced."));
			SubscribeOnGameThread([this]()
			{
				FCoreDelegates::OnEnginePreExit.AddLambda([this]()
				{
					bStop = true;
					Wake->Trigger();
				});
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
			// Marked before the check: a call that ends between the two is reported "outcome unknown", never "not run".
			State->bStarted = true;
			// The caller already timed out or cancelled: never run it late.
			if (State->bCompleted || Call.IsCancelled())
			{
				State->Complete(FUnrealMcpToolResult::Error(TEXT("Tool call was cancelled before it started; it was not run.")));
				return;
			}

			FUnrealMcpToolResult Result;
			{
				// A tool call never waits for a person: as an unattended script, the engine cancels a modal window opened
				// during the body (FSlateApplication::AddModalWindow) and message boxes take their default answer, instead
				// of holding the game thread, and every later call, until someone answers in the editor (§4).
				TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
				Result = Body(Call);
			}
			if (!Result.bRetryLater)
			{
				State->Complete(MoveTemp(Result));
				return;
			}

			// Not ready (e.g. Python before its first tick): again on a later frame, so the engine keeps ticking in
			// between; the watchdog bounds the wait. Nothing ran yet: a call ended meanwhile never started.
			State->bStarted = false;
			UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] tool call not ready, retrying: %s"), *Result.Message);
			Schedule(0.1f);
		}

		/** Run on the game thread at the end of a frame (after @p Delay seconds): see FUnrealMcpEndOfFrameQueue. */
		void Schedule(float Delay = 0.f);
	};

	/**
	 * Where off-thread calls run: the game thread at the end of a frame (FCoreDelegates::OnEndFrame, right after the
	 * core ticker), outside both the task graph's game-thread processing and the ticker. A body run from a game-thread
	 * AsyncTask that waits on game-thread tasks itself (an Interchange import from python-execute) tripped the task graph's
	 * recursion guard; one run from the core ticker that ticks the ticker itself (HLOD builds, material baking) breaks
	 * the ticker's non-reentrant Tick. Both crashed the editor. Process-lifetime, like the watchdog; the delegate is
	 * removed on engine pre-exit (queued calls then end by their timeouts).
	 */
	class FUnrealMcpEndOfFrameQueue
	{
	public:
		static FUnrealMcpEndOfFrameQueue& Get()
		{
			static FUnrealMcpEndOfFrameQueue* Instance = new FUnrealMcpEndOfFrameQueue();
			return *Instance;
		}

		/** Thread-safe. */
		void Add(TSharedRef<FUnrealMcpDispatchJob, ESPMode::ThreadSafe> Job, double NotBefore)
		{
			FScopeLock Lock(&Mutex);
			Pending.Add({ MoveTemp(Job), NotBefore });
		}

	private:
		struct FEntry
		{
			TSharedRef<FUnrealMcpDispatchJob, ESPMode::ThreadSafe> Job;
			double NotBefore;
		};

		FUnrealMcpEndOfFrameQueue()
		{
			// Jobs added before the subscription lands simply wait for the first drained frame.
			SubscribeOnGameThread([this]()
			{
				EndFrame = FCoreDelegates::OnEndFrame.AddRaw(this, &FUnrealMcpEndOfFrameQueue::Drain);
				FCoreDelegates::OnEnginePreExit.AddLambda([this]() { FCoreDelegates::OnEndFrame.Remove(EndFrame); });
			});
		}

		void Drain()
		{
			GUnrealMcpLastEndOfFrame = FPlatformTime::Seconds();
			TArray<FEntry> Due;
			{
				FScopeLock Lock(&Mutex);
				const double Now = FPlatformTime::Seconds();
				for (int32 Index = 0; Index < Pending.Num(); )
				{
					if (Pending[Index].NotBefore <= Now)
					{
						Due.Add(MoveTemp(Pending[Index]));
						Pending.RemoveAt(Index); // keeps arrival order (FIFO)
					}
					else
						++Index;
				}
			}
			// Outside the lock: a body may dispatch (and so enqueue) again; that call runs on a later frame.
			for (FEntry& Entry : Due)
				Entry.Job->Run();
		}

		FCriticalSection Mutex;
		TArray<FEntry> Pending;
		FDelegateHandle EndFrame;
	};

	void FUnrealMcpDispatchJob::Schedule(float Delay)
	{
		FUnrealMcpEndOfFrameQueue::Get().Add(AsShared(), FPlatformTime::Seconds() + Delay);
	}
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

int32 FUnrealMcpGameThreadDispatcher::FailWaitingCalls(const FString& Reason)
{
	return FUnrealMcpDispatchWatchdog::Get().FailAll(Reason);
}
