// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class SWindow;

/**
 * Lets a process outside the editor read and answer its modal dialog (unreal-mcp-cli dialog, docs/ARCHITECTURE.md §4).
 * While a Slate modal window is open only Slate ticks, so no tool call can reach the dialog, and Windows UI Automation
 * does not see its buttons (Slate's accessibility is off by default and leaves a modal window without children). The
 * modal loop's tick still runs on the game thread, so FUnrealMcpModalWatch drives this from there, through files in
 * Saved/UnrealMCP/Dialogs/ (any connection mode, no server or sidecar needed):
 *
 * - `<pid>.json` (written here): the dialog on top, `{"pid", "id", "title", "message", "buttons": [...]}`; removed when
 *   no modal dialog is left.
 * - `<pid>.press.json` (written by the CLI): `{"request", "button", "id"}`, `id` optional; read and removed here.
 * - `<pid>.result.json` (written here): `{"request", "result": "pressed" | "not-found", "button", "message"}`.
 *
 * The file names and fields are a contract with the CLI (cli/src/lib/editor-dialogs.ts). Game thread only.
 */
class UNREALMCPEDITOR_API FUnrealMcpDialogControl
{
public:
	/** What a dialog shows: its title, its text and the labels of its buttons. */
	struct FDescription
	{
		FString Title;
		FString Message;
		TArray<FString> Buttons;
	};

	/** @p InDirectory: where the files go (default Saved/UnrealMCP/Dialogs); @p InPid: whose (default this process). */
	explicit FUnrealMcpDialogControl(FString InDirectory = FString(), uint32 InPid = 0);

	/** The visible texts (not those of buttons, check boxes and list headers, nor the title) and the labelled buttons of @p Window. */
	static FDescription Describe(const TSharedRef<SWindow>& Window);

	/**
	 * Click the visible button labelled @p Label (case-insensitive) in @p Window, when it and every widget around it are
	 * enabled; false when there is none. @p OutPressed: the label as the dialog shows it.
	 */
	static bool Press(const TSharedRef<SWindow>& Window, const FString& Label, FString* OutPressed = nullptr);

	/** The click Press would make, not made yet (empty when there is no such button). */
	static TFunction<void()> FindPress(const TSharedRef<SWindow>& Window, const FString& Label, FString* OutPressed = nullptr);

	/** Publish @p Dialog, the one on top, under @p Id. */
	void Publish(const FDescription& Dialog, int32 Id);

	/** Answer a pending press request against @p Window (published as @p Id); none or no window: nothing to do. */
	void PollPress(const TSharedPtr<SWindow>& Window, int32 Id);

	/** No modal dialog is left: remove the published one and any request that came too late. */
	void Clear();

	const FString& DialogFile() const { return DialogPath; }
	const FString& PressFile() const { return PressPath; }
	const FString& ResultFile() const { return ResultPath; }

private:
	FString File(const TCHAR* Suffix) const;
	void WriteJson(const FString& Path, const TSharedRef<class FJsonObject>& Object) const;

	FString Directory;
	uint32 Pid = 0;
	FString DialogPath;
	FString PressPath;
	FString ResultPath;
};
