// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpFeatureListWindow.h"

#include "Widgets/SNullWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

void SUnrealMcpFeatureListWindow::Construct(const FArguments& InArgs)
{
	TArray<FUnrealMcpFeatureEntry> Entries;
	if (InArgs._FeatureProvider)
		Entries = InArgs._FeatureProvider();

	TSharedRef<SWidget> Body = SNullWidget::NullWidget;
	if (Entries.Num() == 0)
	{
		Body = SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(12.0f)
			[
				SNew(STextBlock)
				.Text(InArgs._EmptyMessage)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
	}
	else
	{
		TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
		for (const FUnrealMcpFeatureEntry& Entry : Entries)
			List->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ BuildEntryRow(Entry) ];
		Body = List;
	}

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
		[
			SNew(STextBlock).Text(InArgs._Heading).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
		]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SSeparator) ]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(8.0f)[ Body ]
		]
	];
}

TSharedRef<SWidget> SUnrealMcpFeatureListWindow::BuildEntryRow(const FUnrealMcpFeatureEntry& Entry)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(FText::FromString(Entry.Name)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Entry.Description))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

#undef LOCTEXT_NAMESPACE
