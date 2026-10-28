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
	// Whether the tool passes the §8 EnabledTools whitelist gate (static, env-driven). A non-whitelisted tool is
	// not served regardless of the §7 Visible toggle, so the window renders its row gated rather than
	// pretending the per-tool checkbox controls it. Defaults true (the common "no whitelist" case).
	bool bWhitelisted = true;
};

/**
 * The "MCP Tools" window (docs/ARCHITECTURE.md §7, Unity's McpToolsWindow analog), a pure-Slate compound widget
 * — NO UMG / editor-utility dependency. Lists every registered tool (across all families, visible or not) with a
 * per-tool Visible checkbox. Visible only decides whether the tool is listed in tools/list: a hidden tool still runs
 * when an MCP client calls it by name. Toggling routes through the shared FUnrealMcpEditorViewModel, which persists
 * the choice to the §8 config store (`hiddenTools`) and asks the runtime to re-flag the tool in the served manifest
 * (so the sidecar's tools/list hides/shows it over the wire). The tool set is snapshotted at Construct; each row's
 * checkbox binds live to the view-model so an edit renders immediately. A tool that fails the §8 EnabledTools
 * whitelist gate is rendered gated (row disabled + annotated) and excluded from the "N / M visible" count, so the
 * window matches what tools/list effectively shows rather than the hidden list alone.
 */
class UNREALMCPEDITOR_API SUnrealMcpToolsWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpToolsWindow) {}
		/** The shared view-model (owned by the runtime). Required — drives the Visible state + persistence. */
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
