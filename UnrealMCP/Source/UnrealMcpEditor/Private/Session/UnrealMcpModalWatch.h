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
 * Once a dialog has been open BlockedAfterSeconds, the calls waiting for the game thread end with its title as the reason,
 * and the first time any did it logs `[Unreal-MCP] Editor blocked by modal dialog "<title>": ...` (Error); when the dialog
 * closes, `[Unreal-MCP] Modal dialog "<title>" closed; tool calls run again.` (Display). The App's editor diagnosis reads
 * these two lines: keep their wording. Game thread only.
 */
class UNREALMCPEDITOR_API FUnrealMcpModalWatch
{
public:
	static constexpr double BlockedAfterSeconds = 2.0;

	~FUnrealMcpModalWatch();

	/** Watch Slate's modal loop. */
	void Start();

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
	FDelegateHandle ModalTickHandle;
	FDelegateHandle ModalClosedHandle;
	TWeakPtr<SWindow> Window;
	FString Title;
	double OpenedAt = 0.0;
	double NextCheckAt = 0.0;
	bool bReported = false;
};
