// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Session/UnrealMcpModalWatch.h"
#include "UnrealMcpLog.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Widgets/SWindow.h"

FUnrealMcpModalWatch::FUnrealMcpModalWatch(TFunction<int32(const FString&)> InEndWaitingCalls)
	: EndWaitingCalls(InEndWaitingCalls ? MoveTemp(InEndWaitingCalls) : TFunction<int32(const FString&)>(&FUnrealMcpGameThreadDispatcher::FailWaitingCalls))
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
}

FString FUnrealMcpModalWatch::BlockedReason(const FString& DialogTitle, double OpenSeconds)
{
	return FString::Printf(TEXT("The editor is showing the modal dialog \"%s\" (open for %.0f s): its game thread waits for the")
		TEXT(" dialog to close, so no tool call can run. Ask the user to answer or close the dialog in the editor, or restart the")
		TEXT(" editor (unsaved changes are lost)."), *DialogTitle, OpenSeconds);
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
		Title = Active->GetTitle().ToString();
	}
	// A few times a second rather than every modal tick; calls that arrive while the dialog stays open end too.
	if (Now - OpenedAt < BlockedAfterSeconds || Now < NextCheckAt)
		return;
	NextCheckAt = Now + 0.25;
	if (EndWaitingCalls(BlockedReason(Title, Now - OpenedAt)) > 0 && !bReported)
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
}
