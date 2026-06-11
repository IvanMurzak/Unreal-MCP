// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpSettingsWindow.h"
#include "UI/UnrealMcpEditorViewModel.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

void SUnrealMcpSettingsWindow::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;
	PortStatusProvider = InArgs._PortStatusProvider;

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SettingsHeading", "AI Game Developer — Connection Settings"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildConnectionModeRow() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildHostRow() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildAuthRow() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildPortsRow() ]
		]
	];
}

TSharedRef<SWidget> SUnrealMcpSettingsWindow::BuildConnectionModeRow()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(LOCTEXT("ModeLabel", "Connection mode")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([this]()
					{
						return IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Cloud
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						if (IsViewModelValid() && NewState == ECheckBoxState::Checked)
							ViewModel->SetConnectionMode(EUnrealMcpConnectionMode::Cloud);
					})
					[ SNew(STextBlock).Text(LOCTEXT("ModeCloud", "Cloud (ai-game.dev)")) ]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([this]()
					{
						return IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						if (IsViewModelValid() && NewState == ECheckBoxState::Checked)
							ViewModel->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
					})
					[ SNew(STextBlock).Text(LOCTEXT("ModeCustom", "Custom (self-hosted)")) ]
				]
			]
		];
}

TSharedRef<SWidget> SUnrealMcpSettingsWindow::BuildHostRow()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(LOCTEXT("HostLabel", "Custom server URL")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("HostHint", "http://localhost:8080"))
				.IsEnabled_Lambda([this]()
				{
					return IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom;
				})
				.Text_Lambda([this]()
				{
					return IsViewModelValid() ? FText::FromString(ViewModel->GetCustomHost()) : FText::GetEmpty();
				})
				.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
				{
					if (IsViewModelValid())
						ViewModel->SetCustomHost(NewText.ToString());
				})
			]
		];
}

TSharedRef<SWidget> SUnrealMcpSettingsWindow::BuildAuthRow()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(LOCTEXT("AuthLabel", "Custom-mode authorization")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]()
				{
					return IsViewModelValid() && ViewModel->GetAuthOption() == EUnrealMcpAuthOption::Required
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
				{
					if (IsViewModelValid())
						ViewModel->SetAuthOption(NewState == ECheckBoxState::Checked ? EUnrealMcpAuthOption::Required : EUnrealMcpAuthOption::None);
				})
				[ SNew(STextBlock).Text(LOCTEXT("AuthRequired", "Require a bearer token")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SEditableTextBox)
					// Mask natively via IsPassword (revealed only while Reveal is held) — committing a fixed-width
					// display mask back would overwrite the real token (the §8 masking contract, mirrored from the
					// main window's Custom-token field).
					.IsPassword_Lambda([this]() { return !bRevealToken; })
					.IsEnabled_Lambda([this]()
					{
						return IsViewModelValid() && ViewModel->GetAuthOption() == EUnrealMcpAuthOption::Required;
					})
					.Text_Lambda([this]()
					{
						return IsViewModelValid() ? FText::FromString(ViewModel->GetCustomToken()) : FText::GetEmpty();
					})
					.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
					{
						if (IsViewModelValid())
							ViewModel->SetCustomToken(NewText.ToString());
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Reveal", "Reveal"))
					.OnPressed_Lambda([this]() { bRevealToken = true; })
					.OnReleased_Lambda([this]() { bRevealToken = false; })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Generate", "Generate"))
					.OnClicked_Lambda([this]()
					{
						if (IsViewModelValid())
							ViewModel->GenerateCustomToken();
						return FReply::Handled();
					})
				]
			]
		];
}

TSharedRef<SWidget> SUnrealMcpSettingsWindow::BuildPortsRow()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(LOCTEXT("PortsLabel", "Ports")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					const FString Line = PortStatusProvider ? PortStatusProvider() : FString();
					return Line.IsEmpty()
						? LOCTEXT("PortsUnknown", "IPC bridge port: not bound")
						: FText::FromString(Line);
				})
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

#undef LOCTEXT_NAMESPACE
