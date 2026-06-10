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
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
