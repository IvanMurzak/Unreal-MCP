// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class SWindow;

/**
 * Tells MCP clients, fast, that a modal dialog holds the editor (docs/ARCHITECTURE.md §4, §11 risk 2). While a Slate modal
 * window is open, FSlateApplication::AddModalWindow ticks only Slate: the engine frame, and with it every tool call (all
 * run on the game thread), waits until someone answers the dialog. Windows still reports the editor "responding" (the
 * modal loop pumps messages), so a client saw only timeouts and "plugin not connected" (seen live: 15 minutes behind a
 * landscape "Insert New Landscape Edit Layer" dialog). Tool bodies themselves run as unattended scripts, so a dialog they
 * would open is cancelled (FUnrealMcpGameThreadDispatcher); this covers every other modal one.
 *
 * Once a modal dialog has been open BlockedAfterSeconds while tool calls wait, it logs once
 *   `[Unreal-MCP] Editor blocked by modal dialog "<title>": ...` (Error)
 * and ends the waiting calls with that reason instead of letting them time out; when the dialog closes it logs
 *   `[Unreal-MCP] Modal dialog "<title>" closed; tool calls run again.` (Display).
 * The App's editor diagnosis reads these two lines (keep their wording). Game thread only.
 */
class UNREALMCPEDITOR_API FUnrealMcpModalWatch
{
public:
	static constexpr double BlockedAfterSeconds = 2.0;

	~FUnrealMcpModalWatch();

	/** Watch Slate's modal loop. @p InWaitingCalls: tool calls in flight (FUnrealMcpBridgeServer::GetInFlightCallCount). */
	void Start(TFunction<int32()> InWaitingCalls);

	/** Stop watching. Idempotent. */
	void Shutdown();

	/** One tick of the modal loop with @p Active the modal window on top (public for specs). */
	void HandleModalTick(const TSharedPtr<SWindow>& Active, double Now);

	/** A modal window closed (FCoreDelegates::PostSlateModal; public for specs). */
	void HandleModalClosed();

	/** Whether the open modal dialog has been reported as blocking tool calls. */
	bool IsReportingBlocked() const { return bReported; }

	/** The text a waiting call ends with (public for specs). */
	static FString BlockedReason(const FString& Title, double OpenSeconds);

private:
	TFunction<int32()> WaitingCalls;
	FDelegateHandle ModalTickHandle;
	FDelegateHandle ModalClosedHandle;
	TWeakPtr<SWindow> Window;
	FString Title;
	double OpenedAt = 0.0;
	double NextFailAt = 0.0;
	bool bReported = false;
};
