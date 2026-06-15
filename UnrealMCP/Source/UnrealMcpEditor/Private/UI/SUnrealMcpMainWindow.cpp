// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpMainWindow.h"
#include "UI/SUnrealMcpAgentConfigurators.h"
#include "UI/SUnrealMcpAgentWidgets.h"
#include "UI/FUnrealMcpStyle.h"
#include "UnrealMcpLog.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Misc/Attribute.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

using UnrealMcpStyleWidgets::EDot;

namespace
{
	// Footer links (mirrors the Unity reference footer: Discord Help/Talk, GitHub bug report, GitHub star).
	const FString DiscordUrl = TEXT("https://discord.gg/");
	const FString IssuesUrl  = TEXT("https://github.com/IvanMurzak/Unreal-MCP/issues");
	const FString StarUrl    = TEXT("https://github.com/IvanMurzak/Unreal-MCP");

	// Segment value-tags for the segmented controls (the int the control reports; mapped to the enums here).
	constexpr int32 TagCustom = 0;
	constexpr int32 TagCloud  = 1;
	constexpr int32 TagStdio  = 0;
	constexpr int32 TagHttp   = 1;
	constexpr int32 TagNone   = 0;
	constexpr int32 TagRequired = 1;
}

void SUnrealMcpMainWindow::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;
	PluginVersion = InArgs._PluginVersion;
	OnRestartBridge = InArgs._OnRestartBridge;
	BridgeStatusProvider = InArgs._BridgeStatusProvider;
	ConnectionInfoProvider = InArgs._ConnectionInfoProvider;

	// The AI-cube logo brush comes from the style set (registered at module startup; lazily-inited fallback).
	LogoBrush = FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Logo");

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildHeaderSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildConnectionSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildAgentConfiguratorsSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildExtensionsSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[ BuildFooterSection() ]
		]
	];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::UnderlinedLabel(const TAttribute<FText>& Text)
{
	// A 13px-bold label with a thin 1px underline (Slate has no text-underline run; we draw a separator).
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.TimelineLabel"))
			.Text(Text)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 1, 0, 0)
		[
			SNew(SSeparator).Thickness(1.0f).SeparatorImage(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.ConnectingLine"))
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildHeaderSection()
{
	const FString Version = PluginVersion.IsEmpty() ? TEXT("0.1.0") : PluginVersion;

	// Left column: base config (Log Level, Timeout (ms), Version) styled rows. Right column: the AI-cube logo.
	auto MakeConfigRow = [](const FText& Label, const TSharedRef<SWidget>& Field)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.40f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
				.Text(Label)
			]
			+ SHorizontalBox::Slot().FillWidth(0.60f).VAlign(VAlign_Center)
			[
				Field
			];
	};

	const TSharedRef<SWidget> LogLevelField = SNew(SBorder)
		.BorderImage(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Input"))
		.Padding(FMargin(8, 3))
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				const FString Lvl = IsViewModelValid() ? ViewModel->GetConfig().LogLevel : FString();
				return FText::FromString(Lvl.IsEmpty() ? TEXT("Info") : Lvl);
			})
		];

	const TSharedRef<SWidget> VersionField = SNew(STextBlock)
		.Text(FText::FromString(Version));

	return UnrealMcpStyleWidgets::StyledCard(
		SNew(SHorizontalBox)
		// Base-config column.
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)[ MakeConfigRow(LOCTEXT("LogLevel", "Log Level"), LogLevelField) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				MakeConfigRow(LOCTEXT("OpenLogFile", "Log file"),
					UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary", LOCTEXT("OpenLog", "Open log file"),
						FOnClicked::CreateLambda([]()
						{
							const FString ProjectLogPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
							FPlatformProcess::ExploreFolder(*ProjectLogPath);
							return FReply::Handled();
						})))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)[ MakeConfigRow(LOCTEXT("VersionLbl", "Version"), VersionField) ]
		]
		// AI-cube logo (top-right; replaces the old banner image).
		+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(8, 0, 0, 0)
		[
			SNew(SBox).WidthOverride(48.0f).HeightOverride(48.0f)
			[
				SNew(SImage).Image(LogoBrush)
			]
		]);
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildConnectionSection()
{
	// The unified Connection card: a header row ("Connection" + Custom/Cloud segmented), then the mode-specific
	// body. Cloud shows the masked token + Revoke/Authorize; Custom shows Server URL + the MCP-server sub-card.
	// The status timeline (dots + connecting line + underlined labels) sits below, shared across both modes.
	return UnrealMcpStyleWidgets::StyledCard(
		SNew(SVerticalBox)
		// Header row: title + Custom/Cloud segmented (top-right).
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ UnrealMcpStyleWidgets::SectionHeader(LOCTEXT("ConnHeader", "Connection")) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				UnrealMcpStyleWidgets::SegmentedControl(
					{ { LOCTEXT("ModeCustom", "Custom"), TagCustom }, { LOCTEXT("ModeCloud", "Cloud"), TagCloud } },
					[this]() { return (IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Cloud) ? TagCloud : TagCustom; },
					[this](int32 SegTag)
					{
						if (IsViewModelValid())
							ViewModel->SetConnectionMode(SegTag == TagCloud ? EUnrealMcpConnectionMode::Cloud : EUnrealMcpConnectionMode::Custom);
					})
			]
		]
		// Cloud auth row (masked token + Revoke / Authorize) — visible only in Cloud mode.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[ BuildCloudAuthRow() ]
		// Custom Server URL + MCP-server sub-card — visible only in Custom mode.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[ BuildCustomServerSection() ]
		// Shared connection-status timeline (Unreal: <status> dot + connecting line + Connect/Disconnect/Stop).
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
		[
			SNew(SHorizontalBox)
			// The status dot.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				UnrealMcpStyleWidgets::StatusDot(TAttribute<EDot>::Create([this]()
				{
					if (!IsViewModelValid())
						return EDot::Offline;
					switch (ViewModel->GetConnectionState())
					{
						case EUnrealMcpConnectionState::Connected:  return EDot::Online;
						case EUnrealMcpConnectionState::Connecting:  return EDot::Ring;
						case EUnrealMcpConnectionState::Degraded:    return EDot::Ring;
						default:                                     return EDot::Offline;
					}
				}))
			]
			// The underlined "Unreal: <status>" label.
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				UnderlinedLabel(TAttribute<FText>::Create([this]()
				{
					return IsViewModelValid()
						? FUnrealMcpEditorViewModel::GetStatusLabel(ViewModel->GetConnectionState())
						: FText::GetEmpty();
				}))
			]
			// Connect / Disconnect / Stop button (secondary styling).
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				UnrealMcpStyleWidgets::StyledButton("UnrealMcp.Button.Secondary",
					SNew(STextBlock).Text_Lambda([this]()
					{
						return IsViewModelValid()
							? FUnrealMcpEditorViewModel::GetButtonText(ViewModel->GetConnectionState())
							: FText::GetEmpty();
					}),
					FOnClicked::CreateSP(this, &SUnrealMcpMainWindow::OnConnectClicked))
			]
		]);
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildCloudAuthRow()
{
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		// Masked cloud token (read-only display).
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 0, 6, 0)
		[
			SNew(SBorder)
			.BorderImage(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Input"))
			.Padding(FMargin(8, 4))
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					if (!IsViewModelValid() || !ViewModel->HasCloudToken())
						return LOCTEXT("NoCloudToken", "Not authorized");
					return FText::FromString(FUnrealMcpEditorViewModel::MaskTokenForDisplay(ViewModel->GetConfig().CloudToken, /*bReveal*/ false));
				})
			]
		]
		// Revoke (red) — only when a token is stored.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return (IsViewModelValid() && ViewModel->HasCloudToken()) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Alert", LOCTEXT("Revoke", "Revoke"),
					FOnClicked::CreateLambda([this]()
					{
						if (IsViewModelValid()) ViewModel->Revoke();
						return FReply::Handled();
					}))
			]
		]
		// Authorize / Cancel.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary",
				TAttribute<FText>::Create([this]()
				{
					return (IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Pending)
						? LOCTEXT("CancelAuth", "Cancel") : LOCTEXT("Authorize", "Authorize");
				}).Get(),
				FOnClicked::CreateLambda([this]()
				{
					if (!IsViewModelValid())
						return FReply::Handled();
					if (ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Pending)
						ViewModel->CancelAuth();
					else
						ViewModel->Authorize();
					return FReply::Handled();
				}))
		];

	// Device-code instructions (verification URL + user code) shown while pending; an authorized/failed line below.
	TSharedRef<SVerticalBox> CloudBody = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ Row ]
		// Pending: code + open-verification.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this]()
			{
				return (IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Pending)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.Font(FUnrealMcpStyle::TimelineLabelFont())
				.Text_Lambda([this]() { return IsViewModelValid() ? FText::FromString(ViewModel->GetDeviceUserCode()) : FText::GetEmpty(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary", LOCTEXT("CopyCode", "Copy code"),
					FOnClicked::CreateLambda([this]()
					{
						if (IsViewModelValid())
							FPlatformApplicationMisc::ClipboardCopy(*ViewModel->GetDeviceUserCode());
						return FReply::Handled();
					}))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary", LOCTEXT("OpenVerify", "Open verification page"),
					FOnClicked::CreateLambda([this]()
					{
						if (IsViewModelValid())
						{
							const FString Url = ViewModel->GetDeviceVerificationUrl();
							if (!Url.IsEmpty())
								FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
						}
						return FReply::Handled();
					}))
			]
		]
		// Authorized indicator.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor(FUnrealMcpStyle::StatusOnline()))
			.Visibility_Lambda([this]()
			{
				return (IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Authorized)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Text(LOCTEXT("Authorized", "Authorized — cloud token stored."))
		]
		// Failure reason.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FUnrealMcpStyle::StatusOffline()))
			.Visibility_Lambda([this]()
			{
				return (IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Failed)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Text_Lambda([this]() { return IsViewModelValid() ? FText::FromString(ViewModel->GetDeviceAuthError()) : FText::GetEmpty(); })
		];

	// Whole Cloud body is visible only in Cloud mode.
	CloudBody->SetVisibility(MakeAttributeLambda([this]() -> EVisibility
	{
		return (IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Cloud)
			? EVisibility::Visible : EVisibility::Collapsed;
	}));
	return CloudBody;
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildCustomServerSection()
{
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		// Server URL row.
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
				.Text(LOCTEXT("ServerUrl", "Server URL"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SEditableTextBox)
				.Text_Lambda([this]() { return IsViewModelValid() ? FText::FromString(ViewModel->GetCustomHost()) : FText::GetEmpty(); })
				.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
				{
					if (IsViewModelValid())
						ViewModel->SetCustomHost(NewText.ToString());
				})
			]
		]
		// URL validation error.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor(FUnrealMcpStyle::StatusOffline()))
			.Text_Lambda([this]()
			{
				if (!IsViewModelValid())
					return FText::GetEmpty();
				FString Error;
				return FUnrealMcpEditorViewModel::ValidateServerUrl(ViewModel->GetCustomHost(), Error)
					? FText::GetEmpty() : FText::FromString(Error);
			})
		]
		// The "MCP server" sub-card: Start + Transport + Authorization (mirrors the reference's framed sub-section).
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			UnrealMcpStyleWidgets::StyledCard(
				SNew(SVerticalBox)
				// MCP server header row (dot + underlined label + teal Start).
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
					[ UnrealMcpStyleWidgets::StatusDot(EDot::Offline) ]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[ UnderlinedLabel(LOCTEXT("McpServer", "MCP server")) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Primary", LOCTEXT("Start", "Start"),
							FOnClicked::CreateLambda([this]()
							{
								// Start mirrors the reference's local-server Start: (re)connect through the view-model.
								if (IsViewModelValid()) ViewModel->Connect();
								return FReply::Handled();
							}))
					]
				]
				// Transport (stdio / http) segmented.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[ BuildTransportSelector() ]
				// Authorization (none / required) + masked token + New.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[ BuildCustomAuthSelector() ])
		];

	Body->SetVisibility(MakeAttributeLambda([this]() -> EVisibility
	{
		return (IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom)
			? EVisibility::Visible : EVisibility::Collapsed;
	}));
	return Body;
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildTransportSelector()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock)
			.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
			.Text(LOCTEXT("TransportLabel", "Transport"))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			UnrealMcpStyleWidgets::SegmentedControl(
				{ { LOCTEXT("TransportStdio", "stdio"), TagStdio }, { LOCTEXT("TransportHttp", "http"), TagHttp } },
				[this]() { return (IsViewModelValid() && ViewModel->GetEffectiveTransport() == EUnrealMcpTransportMethod::Http) ? TagHttp : TagStdio; },
				[this](int32 SegTag)
				{
					if (IsViewModelValid())
						ViewModel->SetTransportMethod(SegTag == TagHttp ? EUnrealMcpTransportMethod::Http : EUnrealMcpTransportMethod::Stdio);
				},
				TAttribute<bool>::Create([this]() { return IsViewModelValid() && ViewModel->IsTransportSelectable(); }))
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildCustomAuthSelector()
{
	return SNew(SVerticalBox)
		// Authorization Token (none / required) row.
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
				.Text(LOCTEXT("AuthTokenLabel", "Authorization Token"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				UnrealMcpStyleWidgets::SegmentedControl(
					{ { LOCTEXT("AuthNone", "none"), TagNone }, { LOCTEXT("AuthRequired", "required"), TagRequired } },
					[this]() { return (IsViewModelValid() && ViewModel->GetAuthOption() == EUnrealMcpAuthOption::Required) ? TagRequired : TagNone; },
					[this](int32 SegTag)
					{
						if (IsViewModelValid())
							ViewModel->SetAuthOption(SegTag == TagRequired ? EUnrealMcpAuthOption::Required : EUnrealMcpAuthOption::None);
					})
			]
		]
		// Masked token field + New — only when auth is required.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this]()
			{
				return (IsViewModelValid() && ViewModel->GetAuthOption() == EUnrealMcpAuthOption::Required)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SEditableTextBox)
				// IsPassword renders the token as dots natively; revealed only while the Reveal button is held (§8).
				.IsPassword_Lambda([this]() { return !bRevealToken; })
				.Text_Lambda([this]() { return IsViewModelValid() ? FText::FromString(ViewModel->GetCustomToken()) : FText::GetEmpty(); })
				.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
				{
					if (IsViewModelValid())
						ViewModel->SetCustomToken(NewText.ToString());
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
			[
				// Reveal-on-HOLD (not a sticky toggle): the raw bearer is drawn only while the button is
				// physically held, then immediately re-masked — the AC's "never rendered unmasked" default
				// always holds (§8). OnPressed/OnReleased drive bRevealToken, which gates IsPassword above.
				SNew(SButton)
				.ButtonStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FButtonStyle>("UnrealMcp.Button.Secondary"))
				.HAlign(HAlign_Center).VAlign(VAlign_Center)
				.ToolTipText(LOCTEXT("RevealHint", "Press and hold to reveal the token; it re-masks on release."))
				.OnPressed_Lambda([this]() { bRevealToken = true; })
				.OnReleased_Lambda([this]() { bRevealToken = false; })
				[
					SNew(STextBlock).Text(LOCTEXT("Reveal", "Reveal"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
			[
				UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary", LOCTEXT("New", "New"),
					FOnClicked::CreateLambda([this]()
					{
						if (IsViewModelValid()) ViewModel->GenerateCustomToken();
						return FReply::Handled();
					}))
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildAgentConfiguratorsSection()
{
	// The AI Agent Configurators panel (§7/§8) — out of scope for visual edits here; it inherits the new style
	// set via FUnrealMcpStyle::Get(). Bound to the same view-model + the runtime connection-info provider.
	return SNew(SUnrealMcpAgentConfigurators)
		.ViewModel(ViewModel)
		.ConnectionInfoProvider(ConnectionInfoProvider);
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildExtensionsSection()
{
	// The Extensions section (issue #78): a styled header + a placeholder card when no registry data exists,
	// matching the populated-row design (Name bold + Install button right + description below). Unreal-MCP has
	// no extensions registry surfaced to the UI yet, so render the styled placeholder per the issue's constraint.
	return UnrealMcpStyleWidgets::StyledCard(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ UnrealMcpStyleWidgets::SectionHeader(LOCTEXT("ExtHeader", "Extensions")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			// One placeholder row in the populated-row shape (bold name + Install + description below).
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FUnrealMcpStyle::SubHeaderFont())
					.Text(LOCTEXT("ExtPlaceholderName", "No extensions available"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).IsEnabled(false)
					[
						UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary", LOCTEXT("Install", "Install"),
							FOnClicked::CreateLambda([]() { return FReply::Handled(); }))
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				UnrealMcpStyleWidgets::Description(
					LOCTEXT("ExtPlaceholderDesc", "Engine extensions will appear here when a registry is available. Each lists a name, a short description, and an Install action."))
			]
		]);
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildFooterSection()
{
	return UnrealMcpStyleWidgets::StyledCard(
		SNew(SVerticalBox)
		// "Found an issue?"
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Font(FUnrealMcpStyle::SubHeaderFont())
			.Text(LOCTEXT("FoundIssue", "Found an issue?"))
		]
		// Help/Talk (Discord) · Bug Report (GitHub) · Check.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				UnrealMcpStyleWidgets::IconButton("UnrealMcp.Button.Secondary", "UnrealMcp.Discord", LOCTEXT("HelpTalk", "Help / Talk"),
					FOnClicked::CreateLambda([]() { FPlatformProcess::LaunchURL(*DiscordUrl, nullptr, nullptr); return FReply::Handled(); }))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				UnrealMcpStyleWidgets::IconButton("UnrealMcp.Button.Secondary", "UnrealMcp.GitHub", LOCTEXT("BugReport", "Bug Report"),
					FOnClicked::CreateLambda([]() { FPlatformProcess::LaunchURL(*IssuesUrl, nullptr, nullptr); return FReply::Handled(); }))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary", LOCTEXT("RestartBridge", "Restart bridge"),
					FOnClicked::CreateLambda([this]() { OnRestartBridge.ExecuteIfBound(); return FReply::Handled(); }))
			]
		]
		// Divider.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)[ SNew(SSeparator).Thickness(1.0f) ]
		// Thanks text + Sincerely + gold GitHub Star.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("ThanksText", "Thank you for using AI Game Developer. If you like it, please give the project a star on GitHub."))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
				[
					SNew(STextBlock)
					.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
					.Text(LOCTEXT("Sincerely", "Sincerely,\nIvan Murzak"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				UnrealMcpStyleWidgets::IconButton("UnrealMcp.Button.Golden", "UnrealMcp.Star", LOCTEXT("GitHubStar", "GitHub Star"),
					FOnClicked::CreateLambda([]() { FPlatformProcess::LaunchURL(*StarUrl, nullptr, nullptr); return FReply::Handled(); }))
			]
		]);
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
