// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class UObject;
struct FPropertyChangedEvent;

/**
 * When the editor should keep rendering in the background (docs/ARCHITECTURE.md §10 "Background rendering"), without
 * engine APIs. An MCP client counts as working with the editor while the sidecar reports one connected (the `status`
 * roster), while a tool call is in flight, and for ActivityGraceSeconds after the last finished call: the grace covers
 * a server that reports no roster and the pause while an agent thinks between calls.
 */
class UNREALMCPEDITOR_API FUnrealMcpKeepAwakePolicy
{
public:
	static constexpr double ActivityGraceSeconds = 120.0;

	/** @p ToolActivity: the finished-call counter (FUnrealMcpBridgeServer::GetToolActivityCount). Pure. */
	bool ShouldHold(double Now, bool bClientConnected, uint64 ToolActivity, int32 InFlightCalls);

private:
	uint64 LastToolActivity = 0;
	TOptional<double> LastActivityTime;
};

/**
 * Keeps the editor ticking and rendering at its normal rate while an MCP client works with it and the editor window is
 * in the background or minimized. UE's "Use Less CPU when in Background" (UEditorPerformanceSettings::
 * bThrottleCPUWhenNotForeground, on by default) does two things there: it caps the editor at a few frames per second
 * (UEditorEngine::ShouldThrottleCPUUsage) and it adds a "Background Process" realtime override that turns every
 * viewport non-realtime, so the editor world only advances time (LEVELTICK_TimeOnly): Niagara, animation and other
 * ticking content stop, and the agent's edits do not show. While held this
 *  - registers an UEditorEngine::ShouldDisableCPUThrottlingDelegates entry (the tick-rate cap, also when every editor
 *    window is minimized), and
 *  - clears bThrottleCPUWhenNotForeground IN MEMORY (the realtime override is read from it every editor tick), keeping
 *    the user's own value and putting it back on release and on shutdown.
 * The user's saved preference never changes: when the settings object is edited (OnSettingChanged fires before the
 * settings editor saves), the user's value is restored first so the save writes it, and the override returns on the
 * next check. It never focuses or raises an editor window. Game thread only.
 */
class UNREALMCPEDITOR_API FUnrealMcpBackgroundRendering
{
public:
	/** How often the hold is re-evaluated (seconds). */
	static constexpr float CheckIntervalSeconds = 0.25f;

	~FUnrealMcpBackgroundRendering();

	/**
	 * Start the periodic check. @p InIsClientConnected: whether the sidecar reports an MCP client (AI agent) connected;
	 * @p InToolActivity / @p InInFlightCalls: the bridge counters.
	 */
	void Start(TFunction<bool()> InIsClientConnected, TFunction<uint64()> InToolActivity, TFunction<int32()> InInFlightCalls);

	/** Stop the check and release the hold (restores the user's setting). Idempotent. */
	void Shutdown();

	/** Apply or release the hold now (the periodic check calls this; public for specs). */
	void SetHeld(bool bHold);
	bool IsHeld() const { return bHeld; }

	/** The user's own "Use Less CPU when in Background" value captured when the hold began (meaningful while held). */
	bool GetUserThrottleSetting() const { return bUserThrottle; }

	/**
	 * Re-apply the in-memory override when something set the setting back on while held: a value of true then is the
	 * user's (or another system's) new preference, kept for the restore. Runs on every check. Public for specs.
	 */
	void Reconcile();

	/** UDeveloperSettings::OnSettingChanged handler (public for specs): put the user's value back before the save. */
	void HandleSettingChanged(UObject* Settings, FPropertyChangedEvent& Event);

private:
	bool Tick(float DeltaTime);

	TFunction<bool()> IsClientConnected;
	TFunction<uint64()> ToolActivity;
	TFunction<int32()> InFlightCalls;
	FUnrealMcpKeepAwakePolicy Policy;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle ThrottleDelegateHandle;
	FDelegateHandle SettingChangedHandle;
	bool bHeld = false;
	bool bUserThrottle = true;
};
