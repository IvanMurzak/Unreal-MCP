// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/** One MCP prompt / resource entry rendered by SUnrealMcpFeatureListWindow (name + one-line description). */
struct FUnrealMcpFeatureEntry
{
	FString Name;
	FString Description;
};

/**
 * A read-only list window shared by the §7 "MCP Prompts" and "MCP Resources" windows (docs/ARCHITECTURE.md §7,
 * Unity's McpPromptsWindow / McpResourcesWindow analogs), pure-Slate (NO UMG). The plugin (C++) side declares
 * no prompts/resources of its own — those features live in the .NET sidecar's McpPlugin feature managers (§2),
 * which §2 did not surface back to the plugin. Rather than invent a fake registry, this window renders whatever
 * its provider returns and shows an honest empty state ("none registered yet") when the list is empty — the
 * minimal read surface the aux-windows task mandates, ready to light up when a prompts/resources feed lands.
 */
class SUnrealMcpFeatureListWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpFeatureListWindow) {}
		/** The message shown when the list is empty (e.g. "No MCP prompts are registered yet."). Required. */
		SLATE_ARGUMENT(FText, EmptyMessage)
		/** The bold one-line heading above the list (e.g. "MCP Prompts"). Required. */
		SLATE_ARGUMENT(FText, Heading)
		/** Snapshots the current feature set. Optional; an unset provider renders the empty state. */
		SLATE_ARGUMENT(TFunction<TArray<FUnrealMcpFeatureEntry>()>, FeatureProvider)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	static TSharedRef<SWidget> BuildEntryRow(const FUnrealMcpFeatureEntry& Entry);
};
