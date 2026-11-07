// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"
#include "Session/UnrealMcpModalWatch.h"
#include "Widgets/SWindow.h"

/**
 * A modal dialog holds the game thread, and with it every tool call, until someone answers it (seen live: 15 minutes
 * behind "Insert New Landscape Edit Layer", reported to the agent only as timeouts and "plugin not connected"). The
 * watch names the dialog in the log and ends the waiting calls with that reason. Driven through its handlers: a real
 * modal loop would block the spec.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpModalWatchSpec, "UnrealMcp.Session.ModalWatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	/** A call the game thread has not finished: it ran once and waits for a later frame. */
	TFuture<FUnrealMcpToolResult> WaitingCall(FUnrealMcpGameThreadDispatcher& Dispatcher)
	{
		return Dispatcher.Dispatch(FUnrealMcpToolCall(),
			[](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::RetryLater(TEXT("waiting")); },
			FTimespan::FromSeconds(30));
	}

END_DEFINE_SPEC(FUnrealMcpModalWatchSpec)

void FUnrealMcpModalWatchSpec::Define()
{
	It("reports a modal dialog that holds waiting calls, ends them with its title, and logs when it closes", [this]()
	{
		AddExpectedError(TEXT("Editor blocked by modal dialog \"Insert New Landscape Edit Layer\""), EAutomationExpectedErrorFlags::Contains, 1);
		FUnrealMcpGameThreadDispatcher Dispatcher;
		int32 Waiting = 1;
		FUnrealMcpModalWatch Watch;
		Watch.Start([&Waiting]() { return Waiting; });
		const TSharedRef<SWindow> Dialog = SNew(SWindow).Title(FText::FromString(TEXT("Insert New Landscape Edit Layer")));

		TFuture<FUnrealMcpToolResult> Call = WaitingCall(Dispatcher);
		Watch.HandleModalTick(Dialog, 100.0);
		Watch.HandleModalTick(Dialog, 101.0);
		TestFalse(TEXT("a dialog open for a moment is not reported"), Watch.IsReportingBlocked());
		TestFalse(TEXT("nor are calls ended"), Call.IsReady());

		Watch.HandleModalTick(Dialog, 100.0 + FUnrealMcpModalWatch::BlockedAfterSeconds + 1.0);
		TestTrue(TEXT("reported"), Watch.IsReportingBlocked());
		if (!TestTrue(TEXT("the waiting call ended"), Call.IsReady()))
			return;
		const FUnrealMcpToolResult Result = Call.Get();
		TestFalse(TEXT("as an error"), Result.bSuccess);
		TestTrue(TEXT("naming the dialog"), Result.Message.Contains(TEXT("\"Insert New Landscape Edit Layer\"")));
		TestTrue(TEXT("saying what to do"), Result.Message.Contains(TEXT("close the dialog in the editor")));

		// A call that arrives while the dialog stays open ends too (the 0.25 s pacing is measured from the last end).
		TFuture<FUnrealMcpToolResult> Later = WaitingCall(Dispatcher);
		Watch.HandleModalTick(Dialog, 104.0);
		TestTrue(TEXT("a later call ended"), Later.IsReady());

		AddExpectedMessage(TEXT("Modal dialog \"Insert New Landscape Edit Layer\" closed; tool calls run again."), EAutomationExpectedMessageFlags::Contains, 1);
		Watch.HandleModalClosed();
		TestFalse(TEXT("cleared"), Watch.IsReportingBlocked());
		Watch.Shutdown();
	});

	It("leaves calls alone while none waits, and a new dialog starts its own grace", [this]()
	{
		FUnrealMcpGameThreadDispatcher Dispatcher;
		int32 Waiting = 0;
		FUnrealMcpModalWatch Watch;
		Watch.Start([&Waiting]() { return Waiting; });
		const TSharedRef<SWindow> First = SNew(SWindow).Title(FText::FromString(TEXT("First")));
		const TSharedRef<SWindow> Second = SNew(SWindow).Title(FText::FromString(TEXT("Second")));

		Watch.HandleModalTick(First, 0.0);
		Watch.HandleModalTick(First, 60.0);
		TestFalse(TEXT("no waiting call: nothing reported"), Watch.IsReportingBlocked());

		Waiting = 1;
		TFuture<FUnrealMcpToolResult> Call = WaitingCall(Dispatcher);
		Watch.HandleModalTick(Second, 61.0);
		TestFalse(TEXT("another dialog: its own grace"), Call.IsReady());
		// End the call here so it does not wait out its timeout after the spec.
		FUnrealMcpGameThreadDispatcher::FailWaitingCalls(TEXT("spec cleanup"));
		Watch.Shutdown();
	});

	It("names the dialog and the remedy in the reason", [this]()
	{
		const FString Reason = FUnrealMcpModalWatch::BlockedReason(TEXT("Save Content"), 42.4);
		TestTrue(TEXT("title"), Reason.Contains(TEXT("\"Save Content\"")));
		TestTrue(TEXT("how long"), Reason.Contains(TEXT("42 s")));
		TestTrue(TEXT("restart"), Reason.Contains(TEXT("restart the")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
