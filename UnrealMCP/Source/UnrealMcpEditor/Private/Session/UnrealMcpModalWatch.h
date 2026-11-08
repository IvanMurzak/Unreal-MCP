// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class SWindow;

/**
 * Ends tool calls that a modal dialog holds, and says so (docs/ARCHITECTURE.md §4). While a Slate modal window is open,
 * FSlateApplication::AddModalWindow ticks only Slate: the engine frame, and with it every tool call, waits until someone
 * answers it, while Windows still reports the editor responding. Tool bodies run as unattended scripts, so a dialog they
 * open is cancelled; this covers every other one.
 *
 * Once modal dialogs have held the game thread BlockedAfterSeconds, the calls waiting for it end with the top dialog's title
 * as the reason, and the first time any did it logs `[Unreal-MCP] Editor blocked by modal dialog "<title>": ...` (Error); when the dialog
 * stack closes (the last modal window), `[Unreal-MCP] Modal dialog "<title>" closed; tool calls run again.` (Display). The App's editor diagnosis reads
 * these two lines: keep their wording. Game thread only.
 */
class UNREALMCPEDITOR_API FUnrealMcpModalWatch
{
public:
	static constexpr double BlockedAfterSeconds = 2.0;

	~FUnrealMcpModalWatch();

	/** @p InEndWaitingCalls: ends the waiting calls with a reason and returns how many (default: the dispatcher's, for all). */
	explicit FUnrealMcpModalWatch(TFunction<int32(const FString&)> InEndWaitingCalls = nullptr);

	/** Watch Slate's modal loop. */
	void Start();

	/** Stop watching. Idempotent. */
	void Shutdown();

	/** One tick of the modal loop with @p Active the modal window on top (public for specs). */
	void HandleModalTick(const TSharedPtr<SWindow>& Active, double Now);

	/** A modal window closed (FCoreDelegates::PostSlateModal); @p bAnyModalLeft: another one is still open (public for specs). */
	void HandleModalClosed(bool bAnyModalLeft);

	/** Whether the open modal dialog has been reported as blocking tool calls. */
	bool IsReportingBlocked() const { return bReported; }

	/** The text a waiting call ends with (public for specs). */
	static FString BlockedReason(const FString& Title, double OpenSeconds);

private:
	TFunction<int32(const FString&)> EndWaitingCalls;
	FDelegateHandle ModalTickHandle;
	FDelegateHandle ModalClosedHandle;
	TWeakPtr<SWindow> Window;
	FString Title;
	double OpenedAt = 0.0;
	double NextCheckAt = 0.0;
	bool bHeld = false; // a modal stack holds the game thread (since OpenedAt)
	bool bReported = false;
};
