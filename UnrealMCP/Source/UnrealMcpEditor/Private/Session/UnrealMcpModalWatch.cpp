// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Session/UnrealMcpModalWatch.h"
#include "UnrealMcpLog.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Widgets/SWindow.h"

FUnrealMcpModalWatch::~FUnrealMcpModalWatch()
{
	Shutdown();
}

void FUnrealMcpModalWatch::Start(TFunction<int32()> InWaitingCalls)
{
	WaitingCalls = MoveTemp(InWaitingCalls);
	if (!FSlateApplication::IsInitialized() || ModalTickHandle.IsValid())
		return;
	ModalTickHandle = FSlateApplication::Get().GetOnModalLoopTickEvent().AddLambda([this](float)
	{
		HandleModalTick(FSlateApplication::Get().GetActiveModalWindow(), FPlatformTime::Seconds());
	});
	ModalClosedHandle = FCoreDelegates::PostSlateModal.AddRaw(this, &FUnrealMcpModalWatch::HandleModalClosed);
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
		HandleModalClosed(); // a different dialog: the previous one is gone
		Window = Active;
		Title = Active->GetTitle().ToString();
		OpenedAt = Now;
		NextFailAt = 0.0;
	}
	if (Now - OpenedAt < BlockedAfterSeconds || !WaitingCalls || WaitingCalls() <= 0 || Now < NextFailAt)
		return;
	if (!bReported)
	{
		bReported = true;
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] Editor blocked by modal dialog \"%s\": its game thread waits for the dialog")
			TEXT(" to close, so MCP tool calls cannot run. Answer or close the dialog in the editor, or restart the editor."), *Title);
	}
	// Calls that arrive while the dialog stays open end here too, a few times a second rather than every modal tick.
	NextFailAt = Now + 0.25;
	FUnrealMcpGameThreadDispatcher::FailWaitingCalls(BlockedReason(Title, Now - OpenedAt));
}

void FUnrealMcpModalWatch::HandleModalClosed()
{
	if (bReported)
		UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Modal dialog \"%s\" closed; tool calls run again."), *Title);
	bReported = false;
	Window.Reset();
	Title.Reset();
}
