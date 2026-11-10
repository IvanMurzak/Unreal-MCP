// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Session/UnrealMcpModalWatch.h"
#include "Session/UnrealMcpDialogControl.h"
#include "UnrealMcpLog.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Widgets/SWindow.h"

FUnrealMcpModalWatch::FUnrealMcpModalWatch(TFunction<int32(const FString&)> InEndWaitingCalls, FString InDialogDirectory)
	: EndWaitingCalls(InEndWaitingCalls ? MoveTemp(InEndWaitingCalls) : TFunction<int32(const FString&)>(&FUnrealMcpGameThreadDispatcher::FailWaitingCalls))
	, Dialogs(MakeUnique<FUnrealMcpDialogControl>(MoveTemp(InDialogDirectory)))
{
}

FUnrealMcpModalWatch::~FUnrealMcpModalWatch()
{
	Shutdown();
}

void FUnrealMcpModalWatch::Start()
{
	if (!FSlateApplication::IsInitialized() || ModalTickHandle.IsValid())
		return;
	Dialogs->Clear(); // a file a crashed editor with the same process id left behind
	ModalTickHandle = FSlateApplication::Get().GetOnModalLoopTickEvent().AddLambda([this](float)
	{
		HandleModalTick(FSlateApplication::Get().GetActiveModalWindow(), FPlatformTime::Seconds());
	});
	// Broadcast after the closing window left the modal stack (FSlateApplication::DestroyWindowsImmediately).
	ModalClosedHandle = FCoreDelegates::PostSlateModal.AddLambda([this]()
	{
		HandleModalClosed(FSlateApplication::Get().GetActiveModalWindow().IsValid());
	});
}

void FUnrealMcpModalWatch::Shutdown()
{
	if (ModalTickHandle.IsValid() && FSlateApplication::IsInitialized())
		FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(ModalTickHandle);
	ModalTickHandle.Reset();
	if (ModalClosedHandle.IsValid())
		FCoreDelegates::PostSlateModal.Remove(ModalClosedHandle);
	ModalClosedHandle.Reset();
	Dialogs->Clear();
}

FString FUnrealMcpModalWatch::BlockedReason(const FString& DialogTitle, double OpenSeconds, const TArray<FString>& DialogButtons)
{
	// "The editor is showing the modal dialog \"<title>\" (open for" is how the CLI recognizes it (cli/src/lib/editor-modal.ts).
	const FString Answer = DialogButtons.IsEmpty() ? FString()
		: FString::Printf(TEXT(", answer it with unreal-mcp-cli dialog --press \"<button>\" (buttons: %s)"), *FString::Join(DialogButtons, TEXT(", ")));
	return FString::Printf(TEXT("The editor is showing the modal dialog \"%s\" (open for %.0f s): its game thread waits for the")
		TEXT(" dialog to close, so no tool call can run. Ask the user to answer or close the dialog in the editor%s, or restart the")
		TEXT(" editor (unsaved changes are lost)."), *DialogTitle, OpenSeconds, *Answer);
}

void FUnrealMcpModalWatch::HandleModalTick(const TSharedPtr<SWindow>& Active, double Now)
{
	if (!Active.IsValid())
		return;
	if (Window.Pin() != Active)
	{
		// A dialog over another keeps the time the game thread has been held since the first; report the top one's title.
		if (!bHeld)
		{
			bHeld = true;
			OpenedAt = Now;
			NextCheckAt = 0.0;
		}
		Window = Active;
		++DialogId;
		bPublished = false;
		NextPollAt = 0.0;
	}
	if (Now >= NextPollAt)
	{
		NextPollAt = Now + PressPollSeconds;
		// Described on every poll: the first modal tick comes before Slate ticked and drew the dialog, so lists fill in
		// and bound visibility settles later. Published again (same id) whenever what it shows changed.
		const FUnrealMcpDialogControl::FDescription Dialog = FUnrealMcpDialogControl::Describe(Active.ToSharedRef());
		if (!bPublished || Dialog.Title != Title || Dialog.Message != Message || Dialog.Buttons != Buttons)
		{
			Title = Dialog.Title;
			Message = Dialog.Message;
			Buttons = Dialog.Buttons;
			Dialogs->Publish(Dialog, DialogId);
			bPublished = true;
		}
		Dialogs->PollPress(Active, DialogId);
	}
	// A few times a second rather than every modal tick; calls that arrive while the dialog stays open end too.
	if (Now - OpenedAt < BlockedAfterSeconds || Now < NextCheckAt)
		return;
	NextCheckAt = Now + 0.25;
	if (EndWaitingCalls(BlockedReason(Title, Now - OpenedAt, Buttons)) > 0 && !bReported)
	{
		bReported = true;
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] Editor blocked by modal dialog \"%s\": its game thread waits for the dialog")
			TEXT(" to close, so MCP tool calls cannot run. Answer or close the dialog in the editor, or restart the editor."), *Title);
	}
}

void FUnrealMcpModalWatch::HandleModalClosed(bool bAnyModalLeft)
{
	if (bAnyModalLeft)
	{
		Window.Reset(); // the next modal tick takes the dialog now on top, keeping the time and the report
		return;
	}
	if (bReported)
		UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Modal dialog \"%s\" closed; tool calls run again."), *Title);
	bReported = false;
	bHeld = false;
	Window.Reset();
	Title.Reset();
	Message.Reset();
	Buttons.Reset();
	Dialogs->Clear();
}
