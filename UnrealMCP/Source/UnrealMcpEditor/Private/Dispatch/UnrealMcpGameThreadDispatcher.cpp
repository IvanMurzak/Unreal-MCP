// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpGameThreadDispatcher.h"
#include "UnrealMcpLog.h"
#include "Async/Async.h"
#include "HAL/Event.h"

#include <atomic>

namespace
{
	/**
	 * Shared completion state for one dispatched call. The promise is completed by whichever path wins
	 * the atomic guard first — the game-thread body or the timeout watcher — so it is never set twice.
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
	 * A pooled FEvent whose lifetime is shared by the body and the timeout watcher. Whichever finishes
	 * LAST releases the last reference, and only then is the event returned to the pool — so a slow body
	 * that triggers the event after the watcher already gave up can never poke a recycled event.
	 */
	struct FSharedPooledEvent
	{
		FEvent* Event;
		FSharedPooledEvent() : Event(FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true)) {}
		~FSharedPooledEvent()
		{
			if (Event != nullptr)
			{
				FPlatformProcess::ReturnSynchEventToPool(Event);
				Event = nullptr;
			}
		}
	};
}

TFuture<FUnrealMcpToolResult> FUnrealMcpGameThreadDispatcher::Dispatch(
	FUnrealMcpToolCall Call,
	TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)> Body,
	FTimespan Timeout)
{
	TSharedRef<FDispatchState, ESPMode::ThreadSafe> State = MakeShared<FDispatchState, ESPMode::ThreadSafe>();
	TSharedRef<FSharedPooledEvent, ESPMode::ThreadSafe> Done = MakeShared<FSharedPooledEvent, ESPMode::ThreadSafe>();
	TFuture<FUnrealMcpToolResult> Future = State->Promise.GetFuture();

	auto RunBody = [State, Body = MoveTemp(Body), Call, Done]() mutable
	{
		FUnrealMcpToolResult Result = Body(Call);
		State->Complete(MoveTemp(Result));
		Done->Event->Trigger();
	};

	if (IsInGameThread())
	{
		RunBody();
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, [RunBody = MoveTemp(RunBody)]() mutable { RunBody(); });
	}

	// Always arm the watcher. When the body already completed, the event is already triggered and Wait()
	// returns immediately. Both lambdas hold a ref to the pooled event, so it is returned to the pool only
	// after BOTH have finished — never while a still-pending body might trigger it.
	const uint32 TimeoutMs = static_cast<uint32>(
		FMath::Clamp<int64>(static_cast<int64>(Timeout.GetTotalMilliseconds()), 1, static_cast<int64>(MAX_uint32)));

	Async(EAsyncExecution::ThreadPool, [State, Done, TimeoutMs, Call]()
	{
		if (Done->Event->Wait(TimeoutMs))
			return; // the body finished first and already completed the promise — nothing to time out.

		// Timed out before the body finished. Signal cooperative cancellation so a still-running game-thread
		// body can observe IsCancelled() and bail (§4 — the registry header documents CancelRequested as
		// "set by a tool-cancel OR a timeout"; until now only tool-cancel set it). The captured Call copy
		// holds the shared cancel flag alive, so this stays valid even after the bridge drops its CancelFlags
		// map entry. Then complete the future with the timeout error; if the body completes in the meantime,
		// its Complete() is a no-op (single-completion guard).
		if (Call.CancelRequested.IsValid())
			const_cast<FThreadSafeBool&>(*Call.CancelRequested) = true;
		State->Complete(FUnrealMcpToolResult::Error(TEXT("Tool call timed out.")));
	});

	return Future;
}
