// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Session/UnrealMcpModalWatch.h"
#include "Widgets/SWindow.h"

/**
 * A modal dialog holds the game thread, and with it every tool call, until someone answers it (seen live: 15 minutes
 * behind "Insert New Landscape Edit Layer", reported to the agent only as timeouts and "plugin not connected"). The
 * watch names the dialog in the log and ends the waiting calls with that reason. Driven through its handlers (a real
 * modal loop would block the spec) with a stand-in for the dispatcher, so no live call of the editor is touched.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpModalWatchSpec, "UnrealMcp.Session.ModalWatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	/** Calls the stand-in dispatcher holds, and the reasons it ended them with. */
	int32 Waiting = 0;
	TArray<FString> Reasons;

	TFunction<int32(const FString&)> EndWaiting()
	{
		return [this](const FString& Reason)
		{
			const int32 Ended = Waiting;
			if (Ended > 0)
				Reasons.Add(Reason);
			Waiting = 0;
			return Ended;
		};
	}

	/** Where the watch publishes its dialogs here: not the editor's own Saved/UnrealMCP/Dialogs. */
	static FString WatchSpecDialogDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("UnrealMcpTests") / TEXT("ModalWatchDialogs"));
	}

	static TSharedRef<SWindow> Dialog(const TCHAR* Title)
	{
		return SNew(SWindow).Title(FText::FromString(Title));
	}

END_DEFINE_SPEC(FUnrealMcpModalWatchSpec)

void FUnrealMcpModalWatchSpec::Define()
{
	BeforeEach([this]() { Waiting = 0; Reasons.Reset(); });

	It("reports a modal dialog that holds waiting calls, ends them with its title, and logs when it closes", [this]()
	{
		AddExpectedErrorPlain(TEXT("Editor blocked by modal dialog \"Insert New Landscape Edit Layer\""), EAutomationExpectedErrorFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("Modal dialog \"Insert New Landscape Edit Layer\" closed; tool calls run again."), EAutomationExpectedMessageFlags::Contains, 1);
		FUnrealMcpModalWatch Watch(EndWaiting(), WatchSpecDialogDir());
		const TSharedRef<SWindow> Picker = Dialog(TEXT("Insert New Landscape Edit Layer"));

		Waiting = 1;
		Watch.HandleModalTick(Picker, 100.0);
		Watch.HandleModalTick(Picker, 101.0);
		TestFalse(TEXT("a dialog open for a moment is not reported"), Watch.IsReportingBlocked());
		TestEqual(TEXT("nor are calls ended"), Waiting, 1);

		Watch.HandleModalTick(Picker, 100.0 + FUnrealMcpModalWatch::BlockedAfterSeconds + 1.0);
		TestTrue(TEXT("reported"), Watch.IsReportingBlocked());
		TestEqual(TEXT("the waiting call ended"), Waiting, 0);
		if (!TestEqual(TEXT("once"), Reasons.Num(), 1))
			return;
		TestTrue(TEXT("naming the dialog"), Reasons[0].Contains(TEXT("\"Insert New Landscape Edit Layer\"")));
		TestTrue(TEXT("saying what to do"), Reasons[0].Contains(TEXT("close the dialog in the editor")));

		// A call that arrives while the dialog stays open ends too (paced from the last check).
		Waiting = 1;
		Watch.HandleModalTick(Picker, 104.0);
		TestEqual(TEXT("a later call ended"), Waiting, 0);

		Watch.HandleModalClosed(/*bAnyModalLeft*/ false);
		TestFalse(TEXT("cleared"), Watch.IsReportingBlocked());
	});

	It("reports nothing while no call waits", [this]()
	{
		FUnrealMcpModalWatch Watch(EndWaiting(), WatchSpecDialogDir());
		const TSharedRef<SWindow> First = Dialog(TEXT("First"));
		Watch.HandleModalTick(First, 0.0);
		Watch.HandleModalTick(First, 60.0);
		TestFalse(TEXT("nothing reported"), Watch.IsReportingBlocked());
	});

	It("keeps a report across a dialog opened over the first, and starts a new grace after the last one closed", [this]()
	{
		AddExpectedErrorPlain(TEXT("Editor blocked by modal dialog \"Parent\""), EAutomationExpectedErrorFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("Modal dialog \"Child\" closed; tool calls run again."), EAutomationExpectedMessageFlags::Contains, 1);
		FUnrealMcpModalWatch Watch(EndWaiting(), WatchSpecDialogDir());
		const TSharedRef<SWindow> Parent = Dialog(TEXT("Parent"));
		const TSharedRef<SWindow> Child = Dialog(TEXT("Child"));

		Waiting = 1;
		Watch.HandleModalTick(Parent, 0.0);
		Watch.HandleModalTick(Parent, 3.0);
		TestTrue(TEXT("parent reported"), Watch.IsReportingBlocked());

		Waiting = 1;
		Watch.HandleModalTick(Child, 4.0);
		TestTrue(TEXT("still reported over the child"), Watch.IsReportingBlocked());
		TestEqual(TEXT("a call waiting behind the child ends at once (held since the parent)"), Waiting, 0);
		TestTrue(TEXT("with the child's title"), Reasons.Last().Contains(TEXT("\"Child\"")));

		Watch.HandleModalClosed(/*bAnyModalLeft*/ true); // the child closed; the parent is still open
		TestTrue(TEXT("no 'closed' while the parent is open"), Watch.IsReportingBlocked());
		Watch.HandleModalClosed(/*bAnyModalLeft*/ false);
		TestFalse(TEXT("cleared after the last one"), Watch.IsReportingBlocked());

		Waiting = 1;
		const TSharedRef<SWindow> Later = Dialog(TEXT("Later"));
		Watch.HandleModalTick(Later, 10.0);
		TestEqual(TEXT("a dialog opened later has its own grace"), Waiting, 1);
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
