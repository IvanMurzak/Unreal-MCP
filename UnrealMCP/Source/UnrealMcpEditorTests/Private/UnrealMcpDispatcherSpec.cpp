// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"
#include "Async/Async.h"

/**
 * Game-thread dispatcher specs (docs/ARCHITECTURE.md §4). Proves the load-bearing DoD claim: a tool
 * body always runs on the game thread, even when dispatched from a background thread (marshalled), and
 * a never-finishing body completes the future with a timeout error.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpDispatcherSpec, "UnrealMcp.Dispatch.GameThread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpDispatcherSpec)

void FUnrealMcpDispatcherSpec::Define()
{
	Describe("Dispatch", [this]()
	{
		It("runs the body and returns its result (inline on the game thread)", [this]()
		{
			FUnrealMcpGameThreadDispatcher Dispatcher;
			bool bRanOnGameThread = false;
			TFuture<FUnrealMcpToolResult> Future = Dispatcher.Dispatch(
				FUnrealMcpToolCall(),
				[&bRanOnGameThread](const FUnrealMcpToolCall&)
				{
					bRanOnGameThread = IsInGameThread();
					return FUnrealMcpToolResult::Success(TEXT("done"));
				},
				FTimespan::FromSeconds(5));

			const FUnrealMcpToolResult Result = Future.Get();
			TestTrue(TEXT("body ran on game thread"), bRanOnGameThread);
			TestTrue(TEXT("success"), Result.bSuccess);
			TestEqual(TEXT("message"), Result.Message, FString(TEXT("done")));
		});

		// Dispatched from a BACKGROUND thread — the body must still execute on the game thread (§4). The
		// background task blocks on the future until the game thread (pumped by the automation runner)
		// processes the marshalled AsyncTask.
		LatentIt("marshals an off-thread call onto the game thread", FTimespan::FromSeconds(15), [this](const FDoneDelegate& Done)
		{
			Async(EAsyncExecution::Thread, [this, Done]()
			{
				FUnrealMcpGameThreadDispatcher Dispatcher;
				const bool bCalledOffGameThread = !IsInGameThread();
				bool bBodyOnGameThread = false;

				TFuture<FUnrealMcpToolResult> Future = Dispatcher.Dispatch(
					FUnrealMcpToolCall(),
					[&bBodyOnGameThread](const FUnrealMcpToolCall&)
					{
						bBodyOnGameThread = IsInGameThread();
						return FUnrealMcpToolResult::Success(TEXT("marshalled"));
					},
					FTimespan::FromSeconds(10));

				const FUnrealMcpToolResult Result = Future.Get();
				TestTrue(TEXT("dispatched off the game thread"), bCalledOffGameThread);
				TestTrue(TEXT("body ran on the game thread"), bBodyOnGameThread);
				TestTrue(TEXT("success"), Result.bSuccess);
				Done.Execute();
			});
		});

		LatentIt("completes with a timeout error when the body never finishes", FTimespan::FromSeconds(15), [this](const FDoneDelegate& Done)
		{
			Async(EAsyncExecution::Thread, [this, Done]()
			{
				FUnrealMcpGameThreadDispatcher Dispatcher;
				// A body that blocks far longer than the timeout — dispatched off the game thread so it
				// would occupy the game thread, but the timeout watcher completes the future first.
				TFuture<FUnrealMcpToolResult> Future = Dispatcher.Dispatch(
					FUnrealMcpToolCall(),
					[](const FUnrealMcpToolCall&)
					{
						FPlatformProcess::Sleep(2.0f);
						return FUnrealMcpToolResult::Success(TEXT("late"));
					},
					FTimespan::FromMilliseconds(200));

				const FUnrealMcpToolResult Result = Future.Get();
				TestFalse(TEXT("timed out -> error"), Result.bSuccess);
				Done.Execute();
			});
		});

		// The single-completion guard (§4) must hold even when the slow body finishes AFTER the timeout has
		// already delivered the error — the late body value must never overwrite the timeout result, and the
		// second Complete() must be a no-op (no crash on the shared promise/event).
		LatentIt("keeps the timeout result when the body completes late (single-completion guard)", FTimespan::FromSeconds(15), [this](const FDoneDelegate& Done)
		{
			Async(EAsyncExecution::Thread, [this, Done]()
			{
				FUnrealMcpGameThreadDispatcher Dispatcher;
				TFuture<FUnrealMcpToolResult> Future = Dispatcher.Dispatch(
					FUnrealMcpToolCall(),
					[](const FUnrealMcpToolCall&)
					{
						FPlatformProcess::Sleep(1.0f); // finishes well after the 100 ms timeout
						return FUnrealMcpToolResult::Success(TEXT("late-body-value"));
					},
					FTimespan::FromMilliseconds(100));

				const FUnrealMcpToolResult Result = Future.Get();
				TestFalse(TEXT("timed out -> error"), Result.bSuccess);
				TestNotEqual(TEXT("late body did not overwrite the timeout result"), Result.Message, FString(TEXT("late-body-value")));

				// Give the late body time to run and (attempt to) complete the promise a second time; the
				// guard must absorb it without altering the already-delivered result or faulting.
				FPlatformProcess::Sleep(1.5f);
				Done.Execute();
			});
		});

		// HandleToolCancel flips the shared cancel flag that the bridge stores; assert FUnrealMcpToolCall
		// observes that flip (and stays false with no flag / a clear flag).
		It("reflects the shared cancel flag through IsCancelled()", [this]()
		{
			FUnrealMcpToolCall NoFlagCall;
			TestFalse(TEXT("no flag -> not cancelled"), NoFlagCall.IsCancelled());

			TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe> Flag = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);
			FUnrealMcpToolCall Call;
			Call.CancelRequested = Flag;
			TestFalse(TEXT("flag clear -> not cancelled"), Call.IsCancelled());

			Flag.Get() = true;
			TestTrue(TEXT("flag set -> cancelled"), Call.IsCancelled());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
