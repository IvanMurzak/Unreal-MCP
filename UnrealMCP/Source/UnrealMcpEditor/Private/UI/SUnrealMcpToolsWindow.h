// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class FUnrealMcpEditorViewModel;

/** One row in the §7 MCP Tools window — a registry snapshot the widget renders (kept free of the registry type). */
struct FUnrealMcpToolListEntry
{
	FString Name;
	FString Title;
	FString Description;
	FString ExtensionId; // "core" or an extension id (§5) — surfaced as the family label.
};

/**
 * The "MCP Tools" window (docs/ARCHITECTURE.md §7, Unity's McpToolsWindow analog), a pure-Slate compound widget
 * — NO UMG / editor-utility dependency. Lists every registered tool (across all families, enabled or not) with a
 * per-tool enable/disable checkbox. Toggling routes through the shared FUnrealMcpEditorViewModel, which persists
 * the choice to the §8 config store and asks the runtime to exclude/restore the tool in the served manifest
 * (so the sidecar's tools/list drops/restores it over the wire). The tool set is snapshotted at Construct (it is
 * static after boot); each row's checkbox binds live to the view-model so the rendered state always reflects the
 * persisted blocklist.
 */
class SUnrealMcpToolsWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpToolsWindow) {}
		/** The shared view-model (owned by the runtime). Required — drives enable state + persistence. */
		SLATE_ARGUMENT(TSharedPtr<FUnrealMcpEditorViewModel>, ViewModel)
		/** Snapshots the full registered-tool set (sorted). Required — the runtime closes over the registry. */
		SLATE_ARGUMENT(TFunction<TArray<FUnrealMcpToolListEntry>()>, ToolListProvider)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	TArray<FUnrealMcpToolListEntry> Tools;

	TSharedRef<SWidget> BuildToolRow(const FUnrealMcpToolListEntry& Entry);
	FText GetSummaryText() const;
};
