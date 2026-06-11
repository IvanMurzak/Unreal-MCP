// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpMainWindow.h"
#include "UnrealMcpLog.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Misc/Attribute.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformApplicationMisc.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

namespace
{
	const FString DiscordUrl = TEXT("https://discord.gg/");
	const FString IssuesUrl = TEXT("https://github.com/IvanMurzak/Unreal-MCP/issues");
	const FString StarUrl = TEXT("https://github.com/IvanMurzak/Unreal-MCP");
}

void SUnrealMcpMainWindow::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;
	PluginVersion = InArgs._PluginVersion;
	OnRestartBridge = InArgs._OnRestartBridge;
	BridgeStatusProvider = InArgs._BridgeStatusProvider;

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildHeaderSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildConnectionSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildModeToggleSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildCloudAuthSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildCustomAuthSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildBridgeStatusSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildAiAgentsSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildFooterSection() ]
		]
	];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::SectionHeader(const FText& Title)
{
	return SNew(STextBlock)
		.Text(Title)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11));
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildHeaderSection()
{
	const FString Version = PluginVersion.IsEmpty() ? TEXT("0.1.0") : PluginVersion;
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("HeaderTitle", "AI Game Developer  —  v{0}"), FText::FromString(Version)))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenLog", "Open log file"))
					.OnClicked_Lambda([]()
					{
						const FString ProjectLogPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
						FPlatformProcess::ExploreFolder(*ProjectLogPath);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RestartBridge", "Restart bridge"))
					.OnClicked_Lambda([this]()
					{
						OnRestartBridge.ExecuteIfBound();
						return FReply::Handled();
					})
				]
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildConnectionSection()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ SectionHeader(LOCTEXT("ConnHeader", "Connection")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				// Status dot.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(SColorBlock)
					.Size(FVector2D(14.0f, 14.0f))
					.Color_Lambda([this]()
					{
						return IsViewModelValid()
							? FUnrealMcpEditorViewModel::GetStatusColor(ViewModel->GetConnectionState())
							: FLinearColor::Gray;
					})
				]
				// Status label.
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return IsViewModelValid()
							? FUnrealMcpEditorViewModel::GetStatusLabel(ViewModel->GetConnectionState())
							: FText::GetEmpty();
					})
				]
				// Connect / Disconnect / Stop button.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text_Lambda([this]()
					{
						return IsViewModelValid()
							? FUnrealMcpEditorViewModel::GetButtonText(ViewModel->GetConnectionState())
							: FText::GetEmpty();
					})
					.OnClicked(this, &SUnrealMcpMainWindow::OnConnectClicked)
				]
			]
			// Custom-mode server URL field (only shown in Custom mode).
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this]()
				{
					return IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("ServerUrl", "Server URL"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					SAssignNew(CustomHostBox, SEditableTextBox)
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
				// Inline validation error.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FLinearColor(0.95f, 0.4f, 0.4f))
					.Text_Lambda([this]()
					{
						if (!IsViewModelValid())
							return FText::GetEmpty();
						FString Error;
						return FUnrealMcpEditorViewModel::ValidateServerUrl(ViewModel->GetCustomHost(), Error)
							? FText::GetEmpty() : FText::FromString(Error);
					})
				]
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildModeToggleSection()
{
	auto MakeModeButton = [this](const FText& Label, EUnrealMcpConnectionMode Mode)
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.IsChecked_Lambda([this, Mode]()
			{
				return IsViewModelValid() && ViewModel->GetConnectionMode() == Mode
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, Mode](ECheckBoxState NewState)
			{
				if (NewState == ECheckBoxState::Checked && IsViewModelValid())
					ViewModel->SetConnectionMode(Mode);
			})
			.Padding(FMargin(12, 4))
			[
				SNew(STextBlock).Text(Label)
			];
	};

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ SectionHeader(LOCTEXT("ModeHeader", "Connection mode")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)[ MakeModeButton(LOCTEXT("ModeCloud", "Cloud"), EUnrealMcpConnectionMode::Cloud) ]
				+ SHorizontalBox::Slot().AutoWidth()[ MakeModeButton(LOCTEXT("ModeCustom", "Custom"), EUnrealMcpConnectionMode::Custom) ]
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildCloudAuthSection()
{
	TSharedRef<SBorder> Section = SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ SectionHeader(LOCTEXT("CloudAuthHeader", "Cloud authorization")) ]
			// Buttons row.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Authorize", "Authorize"))
					.IsEnabled_Lambda([this]()
					{
						return IsViewModelValid() && ViewModel->GetDeviceAuthState() != EUnrealMcpDeviceAuthState::Pending;
					})
					.OnClicked_Lambda([this]()
					{
						if (IsViewModelValid()) ViewModel->Authorize();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("CancelAuth", "Cancel"))
					.Visibility_Lambda([this]()
					{
						return IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Pending
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([this]()
					{
						if (IsViewModelValid()) ViewModel->CancelAuth();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Revoke", "Revoke"))
					.Visibility_Lambda([this]()
					{
						return IsViewModelValid() && ViewModel->HasCloudToken()
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([this]()
					{
						if (IsViewModelValid()) ViewModel->Revoke();
						return FReply::Handled();
					})
				]
			]
			// Device-code instructions (verification URL + user code), shown while pending.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this]()
				{
					return IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Pending
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).AutoWrapText(true)
					.Text(LOCTEXT("DeviceInstr", "Complete authorization in your browser, then enter this code:"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
						.Text_Lambda([this]()
						{
							return IsViewModelValid() ? FText::FromString(ViewModel->GetDeviceUserCode()) : FText::GetEmpty();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
					[
						SNew(SButton)
						.Text(LOCTEXT("CopyCode", "Copy code"))
						.OnClicked_Lambda([this]()
						{
							if (IsViewModelValid())
								FPlatformApplicationMisc::ClipboardCopy(*ViewModel->GetDeviceUserCode());
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
					[
						SNew(SButton)
						.Text(LOCTEXT("OpenVerify", "Open verification page"))
						.OnClicked_Lambda([this]()
						{
							if (IsViewModelValid())
							{
								const FString Url = ViewModel->GetDeviceVerificationUrl();
								if (!Url.IsEmpty())
									FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
							}
							return FReply::Handled();
						})
					]
				]
			]
			// Authorized indicator.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FLinearColor(0.16f, 0.74f, 0.30f))
				.Visibility_Lambda([this]()
				{
					return IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Authorized
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text(LOCTEXT("Authorized", "Authorized — cloud token stored."))
			]
		];

	Section->SetVisibility(MakeAttributeLambda([this]() -> EVisibility
	{
		return IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Cloud
			? EVisibility::Visible : EVisibility::Collapsed;
	}));
	return Section;
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildCustomAuthSection()
{
	TSharedRef<SBorder> Section = SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ SectionHeader(LOCTEXT("CustomAuthHeader", "Server authorization")) ]
			// none / required toggle.
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
				[
					SNew(STextBlock).Text(LOCTEXT("AuthRequired", "Require a bearer token"))
				]
			]
			// Masked token field + Generate, shown only when auth is required.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]()
				{
					return IsViewModelValid() && ViewModel->GetAuthOption() == EUnrealMcpAuthOption::Required
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SEditableTextBox)
					// IsPassword renders the token as dots natively — the raw value is never drawn unless revealed (§8).
					.IsPassword_Lambda([this]() { return !bRevealToken; })
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
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("Reveal", "Reveal"))
					.ToolTipText(LOCTEXT("RevealHint", "Press and hold to reveal the token; it re-masks on release."))
					// Reveal-on-HOLD (not a sticky toggle): the raw bearer is only ever drawn while the button is
					// physically held, then immediately re-masked — the AC's "never rendered unmasked" default
					// always holds (§8). OnPressed/OnReleased drive bRevealToken, which gates IsPassword above.
					.OnPressed_Lambda([this]() { bRevealToken = true; })
					.OnReleased_Lambda([this]() { bRevealToken = false; })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("Generate", "Generate"))
					.OnClicked_Lambda([this]()
					{
						if (IsViewModelValid()) ViewModel->GenerateCustomToken();
						return FReply::Handled();
					})
				]
			]
		];

	Section->SetVisibility(MakeAttributeLambda([this]() -> EVisibility
	{
		return IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom
			? EVisibility::Visible : EVisibility::Collapsed;
	}));
	return Section;
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildBridgeStatusSection()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ SectionHeader(LOCTEXT("BridgeHeader", "Bridge (sidecar)")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					if (BridgeStatusProvider)
						return FText::FromString(BridgeStatusProvider());
					return LOCTEXT("BridgeUnknown", "Sidecar status unavailable.");
				})
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildAiAgentsSection()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ SectionHeader(LOCTEXT("AgentsHeader", "AI agents")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]()
				{
					if (!IsViewModelValid() || ViewModel->GetAiAgents().Num() == 0)
						return LOCTEXT("NoAgents", "No AI agents connected.");
					return FText::FromString(FString::Join(ViewModel->GetAiAgents(), TEXT(", ")));
				})
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildFooterSection()
{
	auto MakeLinkButton = [](const FText& Label, const FString& Url)
	{
		return SNew(SButton)
			.Text(Label)
			.OnClicked_Lambda([Url]()
			{
				FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
				return FReply::Handled();
			});
	};

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SSeparator) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)[ MakeLinkButton(LOCTEXT("Discord", "Discord"), DiscordUrl) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)[ MakeLinkButton(LOCTEXT("Issues", "Issues"), IssuesUrl) ]
			+ SHorizontalBox::Slot().AutoWidth()[ MakeLinkButton(LOCTEXT("Star", "Star on GitHub"), StarUrl) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT("Thanks", "Thank you for using Unreal-MCP — built by Ivan Murzak and contributors."))
		];
}

FReply SUnrealMcpMainWindow::OnConnectClicked()
{
	if (!IsViewModelValid())
		return FReply::Handled();

	// Tri-state: Disconnected → Connect; anything else (Connecting/Connected/Degraded) → Disconnect/Stop.
	if (ViewModel->GetConnectionState() == EUnrealMcpConnectionState::Disconnected)
		ViewModel->Connect();
	else
		ViewModel->Disconnect();
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
