// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpToolsWindow.h"
#include "UI/UnrealMcpEditorViewModel.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SCheckBox.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

void SUnrealMcpToolsWindow::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;
	// Snapshot the registered-tool set once at construction. Core families register before the bridge accepts
	// (§2.2), but a §5 extension hot-reload CAN add/remove tools after boot — an already-open window keeps its
	// construction snapshot, so a late-registered tool only appears on reopen. The per-row Visible checkbox binds LIVE
	// to the view-model, so show/hide edits render immediately without rebuilding the list.
	if (InArgs._ToolListProvider)
		Tools = InArgs._ToolListProvider();

	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	if (Tools.Num() == 0)
	{
		List->AddSlot().AutoHeight().Padding(4.0f)
		[
			SNew(STextBlock).Text(LOCTEXT("ToolsEmpty", "No tools registered."))
		];
	}
	else
	{
		for (const FUnrealMcpToolListEntry& Entry : Tools)
		{
			List->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ BuildToolRow(Entry) ];
		}
	}

	ChildSlot
	[
		SNew(SVerticalBox)
		// Summary line: "N / M tools visible", plus what Visible means.
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 0)
		[
			SNew(STextBlock)
			.Text(this, &SUnrealMcpToolsWindow::GetSummaryText)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 2, 8, 4)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ToolsVisibleHint", "Visible: listed in tools/list for AI agents. A hidden tool still runs when called by name."))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SSeparator) ]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(8.0f)[ List ]
		]
	];
}

FText SUnrealMcpToolsWindow::GetSummaryText() const
{
	int32 Visible = 0;
	if (ViewModel.IsValid())
	{
		for (const FUnrealMcpToolListEntry& Entry : Tools)
		{
			// Count what tools/list EFFECTIVELY shows: served (passes the §8 env whitelist gate) AND not hidden by the
			// user — matching the registry's manifest, not the §7 hidden list alone.
			if (Entry.bWhitelisted && !ViewModel->IsToolHidden(Entry.Name))
				++Visible;
		}
	}
	return FText::Format(LOCTEXT("ToolsSummaryVisible", "{0} / {1} tools visible"),
		FText::AsNumber(Visible), FText::AsNumber(Tools.Num()));
}

TSharedRef<SWidget> SUnrealMcpToolsWindow::BuildToolRow(const FUnrealMcpToolListEntry& Entry)
{
	const FString ToolName = Entry.Name;
	const bool bWhitelisted = Entry.bWhitelisted;
	const FString Family = Entry.ExtensionId.IsEmpty() ? TEXT("core") : Entry.ExtensionId;
	const FText Heading = Entry.Title.IsEmpty()
		? FText::FromString(Entry.Name)
		: FText::Format(LOCTEXT("ToolHeading", "{0}  ({1})"), FText::FromString(Entry.Title), FText::FromString(Entry.Name));

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SHorizontalBox)
			// Per-tool Visible checkbox (listed in tools/list or not; a hidden tool still runs when called by name). A
			// whitelist-gated tool (§8) is not served at all regardless of this toggle, so its checkbox is forced
			// unchecked + disabled — toggling it would have no wire effect.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 0, 8, 0)
			[
				SNew(SCheckBox)
				.IsEnabled(bWhitelisted)
				.ToolTipText(LOCTEXT("ToolVisibleTip", "Visible: list this tool in tools/list. A hidden tool still runs when an AI agent calls it by name."))
				.IsChecked_Lambda([this, ToolName, bWhitelisted]()
				{
					if (!bWhitelisted)
						return ECheckBoxState::Unchecked; // never served — the §8 whitelist excludes it
					const bool bVisible = ViewModel.IsValid() ? !ViewModel->IsToolHidden(ToolName) : true;
					return bVisible ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, ToolName](ECheckBoxState NewState)
				{
					if (ViewModel.IsValid())
						ViewModel->SetToolVisible(ToolName, NewState == ECheckBoxState::Checked);
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(Heading).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Family))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					// Whitelist-gate annotation (§8): the tool is excluded by the EnabledTools filter and cannot be served
					// from this window — make that explicit instead of showing an inert checked row.
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SNew(STextBlock)
						.Visibility(bWhitelisted ? EVisibility::Collapsed : EVisibility::Visible)
						.Text(LOCTEXT("ToolWhitelistGated", "disabled by the EnabledTools filter"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Entry.Description))
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
