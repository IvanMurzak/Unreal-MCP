// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SButton;
class SCheckBox;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class STextBlock;

/**
 * The "Serialization Check" window (docs/ARCHITECTURE.md §7, Unity's SerializationCheckWindow analog), a
 * pure-Slate compound widget — NO UMG / editor-utility dependency. Lets a developer pick a UObject/Actor by
 * its path-or-label string ref, toggle Recursive, serialize it to pretty-printed JSON, see the elapsed time,
 * and Copy the output.
 *
 * Serialization path (the §22.5/§3.2 in-process JSON converter — NOT a sidecar round-trip): Unreal serializes
 * objects to JSON in-process via FUnrealMcpPropertyJson::SerializeObject (FJsonObjectConverter over the object's
 * reflected FProperties), the SAME path the `object-get-data` / `actor-get-data` tools use to return Unreal
 * object data over MCP. ReflectorNet lives in the .NET sidecar and only wraps the MCP wire layer — the editor
 * plugin never needs it to serialize a UObject — so this window calls SerializeObject directly on the game
 * thread. The Recursive toggle mirrors Unity's `recursive` flag: when OFF, nested UObject* references serialize
 * as their identity (path/name) only; when ON, the full reflected graph is emitted (FJsonObjectConverter walks
 * referenced sub-objects). Target resolution reuses FUnrealMcpObjectRef::ResolveObject (path or actor label),
 * matching the tool families' ref rule. "Use selection" fills the input from the current editor selection.
 *
 * Headless-safe: like the other §7 windows, construction guards Slate-only work and the window is registered
 * as a nomad tab by FUnrealMcpAuxWindows. All serialization runs on the game thread (FProperty reads require it).
 */
class SUnrealMcpSerializationCheckWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpSerializationCheckWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * The "Recursive OFF" shallow view: keep only the scalar (non-object, non-array) top-level fields of a full
	 * serialization — the object's identity + flat properties, dropping nested reflected sub-objects. Pure
	 * JSON→JSON so a spec can assert it without launching Slate; exported so the test module links it. When
	 * @p Source is null, returns an empty object.
	 */
	static UNREALMCPEDITOR_API TSharedRef<class FJsonObject> ShallowFilter(const TSharedPtr<class FJsonObject>& Source);

private:
	// Build the Information foldout (collapsed help text).
	TSharedRef<SWidget> BuildInformationFoldout();
	// Build the Target ref input row + "Use selection" + Recursive + Serialize.
	TSharedRef<SWidget> BuildInputRow();
	// Build the Output header + scrollable monospace JSON panel + Copy.
	TSharedRef<SWidget> BuildOutputSection();

	// Resolve the target ref, serialize via FUnrealMcpPropertyJson, pretty-print, and fill the output.
	FReply OnSerializeClicked();
	// Fill the target input from the current editor selection (first selected actor, else first selected object).
	FReply OnUseSelectionClicked();
	// Copy the full output text to the clipboard and flash "Copied!" for ~1.5s.
	FReply OnCopyClicked();
	// One-shot active-timer callback that restores the Copy label after the "Copied!" flash.
	EActiveTimerReturnType OnCopyResetTimer(double InCurrentTime, float InDeltaTime);

	// Replace the output panel text + remember the full text for Copy.
	void SetOutput(const FString& Text);

	// Widgets bound after Construct (read on the game thread only).
	TSharedPtr<SEditableTextBox> TargetField;
	TSharedPtr<SCheckBox> RecursiveToggle;
	TSharedPtr<SMultiLineEditableTextBox> OutputBox;
	TSharedPtr<SButton> CopyButton;

	// The output header text ("Output" / "Output (NN ms)"), bound live.
	FText OutputHeader;
	// The full serialized text the Copy button writes (the OutputBox is read-only but we keep the canonical copy).
	FString FullOutputText;
	// The Copy button's resting label; swapped to "Copied!" on click and restored by a timer.
	FText CopyLabel;
};
