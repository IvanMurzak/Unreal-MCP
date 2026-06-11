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
	// Snapshot the registered-tool set once — the set is static after boot (§2.2: core families register before
	// the bridge accepts), so a re-snapshot would not change. The per-row checkbox binds LIVE to the view-model,
	// so enable/disable edits render immediately without rebuilding the list.
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
		// Summary line: "N / M tools enabled".
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
		[
			SNew(STextBlock)
			.Text(this, &SUnrealMcpToolsWindow::GetSummaryText)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
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
	int32 Enabled = 0;
	if (ViewModel.IsValid())
	{
		for (const FUnrealMcpToolListEntry& Entry : Tools)
		{
			if (!ViewModel->IsToolDisabled(Entry.Name))
				++Enabled;
		}
	}
	return FText::Format(LOCTEXT("ToolsSummary", "{0} / {1} tools enabled"),
		FText::AsNumber(Enabled), FText::AsNumber(Tools.Num()));
}

TSharedRef<SWidget> SUnrealMcpToolsWindow::BuildToolRow(const FUnrealMcpToolListEntry& Entry)
{
	const FString ToolName = Entry.Name;
	const FString Family = Entry.ExtensionId.IsEmpty() ? TEXT("core") : Entry.ExtensionId;
	const FText Heading = Entry.Title.IsEmpty()
		? FText::FromString(Entry.Name)
		: FText::Format(LOCTEXT("ToolHeading", "{0}  ({1})"), FText::FromString(Entry.Title), FText::FromString(Entry.Name));

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SHorizontalBox)
			// Per-tool enable/disable checkbox.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 0, 8, 0)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this, ToolName]()
				{
					const bool bEnabled = ViewModel.IsValid() ? !ViewModel->IsToolDisabled(ToolName) : true;
					return bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, ToolName](ECheckBoxState NewState)
				{
					if (ViewModel.IsValid())
						ViewModel->SetToolEnabled(ToolName, NewState == ECheckBoxState::Checked);
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
