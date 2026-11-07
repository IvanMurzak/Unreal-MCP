// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Session/UnrealMcpBackgroundRendering.h"
#include "UnrealMcpLog.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorPerformanceSettings.h"
#include "HAL/PlatformTime.h"
#include "UObject/UnrealType.h"

namespace
{
	/** The bitfield cannot go through GET_MEMBER_NAME_CHECKED (no member pointer to a bitfield). */
	const FName BackgroundRenderingThrottleProperty(TEXT("bThrottleCPUWhenNotForeground"));
}

// --- FUnrealMcpKeepAwakePolicy --------------------------------------------------------------------

bool FUnrealMcpKeepAwakePolicy::ShouldHold(double Now, bool bClientConnected, uint64 InToolActivity, int32 InFlight)
{
	if (InFlight > 0 || InToolActivity != LastToolActivity)
	{
		LastToolActivity = InToolActivity;
		LastActivityTime = Now;
	}
	// An in-flight call has just stamped LastActivityTime, so the grace covers it.
	return bClientConnected || (LastActivityTime.IsSet() && Now - *LastActivityTime < ActivityGraceSeconds);
}

// --- FUnrealMcpBackgroundRendering ----------------------------------------------------------------

FUnrealMcpBackgroundRendering::~FUnrealMcpBackgroundRendering()
{
	Shutdown();
}

void FUnrealMcpBackgroundRendering::Start(TFunction<bool()> InIsClientConnected, TFunction<uint64()> InToolActivity,
	TFunction<int32()> InInFlightCalls)
{
	IsClientConnected = MoveTemp(InIsClientConnected);
	ToolActivity = MoveTemp(InToolActivity);
	InFlightCalls = MoveTemp(InInFlightCalls);
	if (!TickHandle.IsValid())
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FUnrealMcpBackgroundRendering::Tick), CheckIntervalSeconds);
}

void FUnrealMcpBackgroundRendering::Shutdown()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	SetHeld(false);
	IsClientConnected = nullptr;
	ToolActivity = nullptr;
	InFlightCalls = nullptr;
}

bool FUnrealMcpBackgroundRendering::Tick(float /*DeltaTime*/)
{
	const bool bClient = IsClientConnected && IsClientConnected();
	SetHeld(Policy.ShouldHold(FPlatformTime::Seconds(), bClient, ToolActivity ? ToolActivity() : 0, InFlightCalls ? InFlightCalls() : 0));
	Reconcile();
	return true;
}

void FUnrealMcpBackgroundRendering::SetHeld(bool bHold)
{
	if (bHold == bHeld)
		return;
	UEditorEngine* Editor = GEditor;
	if (bHold && !Editor)
		return; // no editor engine yet: the next check retries
	UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
	bHeld = bHold;
	if (bHold)
	{
		bUserThrottle = Settings->bThrottleCPUWhenNotForeground != 0;
		Settings->bThrottleCPUWhenNotForeground = false;
		SettingChangedHandle = Settings->OnSettingChanged().AddRaw(this, &FUnrealMcpBackgroundRendering::HandleSettingChanged);
		UEditorEngine::FShouldDisableCPUThrottling Delegate = UEditorEngine::FShouldDisableCPUThrottling::CreateLambda([]() { return true; });
		ThrottleDelegateHandle = Delegate.GetHandle();
		Editor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Delegate));
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] An MCP client is working with the editor: it keeps rendering at full rate in the "
			"background (\"Use Less CPU when in Background\" is overridden for this session only; the saved preference is unchanged)."));
		return;
	}

	Settings->OnSettingChanged().Remove(SettingChangedHandle);
	SettingChangedHandle.Reset();
	Settings->bThrottleCPUWhenNotForeground = bUserThrottle;
	if (Editor)
	{
		const FDelegateHandle Handle = ThrottleDelegateHandle;
		Editor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
			[Handle](const UEditorEngine::FShouldDisableCPUThrottling& Entry) { return Entry.GetHandle() == Handle; });
	}
	ThrottleDelegateHandle.Reset();
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] No MCP client is working with the editor: background rendering is back to the "
		"\"Use Less CPU when in Background\" preference (%s)."), bUserThrottle ? TEXT("on") : TEXT("off"));
}

void FUnrealMcpBackgroundRendering::Reconcile()
{
	if (!bHeld)
		return;
	UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
	if (Settings->bThrottleCPUWhenNotForeground)
	{
		bUserThrottle = true;
		Settings->bThrottleCPUWhenNotForeground = false;
	}
}

void FUnrealMcpBackgroundRendering::HandleSettingChanged(UObject* /*Settings*/, FPropertyChangedEvent& Event)
{
	if (!bHeld)
		return;
	UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
	// A value of true here was set by someone else (we only ever write false); an explicit edit of this property is
	// the user's choice either way. Any other edit keeps the value captured when the hold began.
	const bool bCurrent = Settings->bThrottleCPUWhenNotForeground != 0;
	if (bCurrent || (Event.Property && Event.Property->GetFName() == BackgroundRenderingThrottleProperty))
		bUserThrottle = bCurrent;
	// The settings editor saves right after this notification: let it write the user's value. Reconcile (next check)
	// puts the override back.
	Settings->bThrottleCPUWhenNotForeground = bUserThrottle;
}
