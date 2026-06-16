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
#include "Widgets/Input/SComboBox.h"
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

	// The Log Level options in Unity's LogLevel.cs order (Runtime/Utils/LogLevel.cs): Trace, Debug, Info,
	// Warning, Error, Exception, None. Rendered 1:1 in the §7 Log Level dropdown (issue #80 item 1).
	const TArray<FString> GLogLevelNames =
	{
		TEXT("Trace"), TEXT("Debug"), TEXT("Info"), TEXT("Warning"),
		TEXT("Error"), TEXT("Exception"), TEXT("None")
	};

	// One consistent right edge for every connection-row control (issue #80 item 4): the Custom/Cloud group,
	// the Server URL input, the Start button, and the transport / auth segmented groups all right-align to the
	// same x. A fixed-width right column makes that edge identical regardless of each control's natural width.
	constexpr float RightControlColumnWidth = 150.0f;
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

	// Build the Log Level dropdown's item source once (kept as a member so OptionsSource stays valid).
	LogLevelItems.Reset();
	for (const FString& Name : GLogLevelNames)
		LogLevelItems.Add(MakeShared<FString>(Name));

	// Sections are no longer each wrapped in a card (issue #80 item 6). Unity uses a flat layout with 1px
	// dividers between major sections and reserves the blue rounded frame for the few blocks that use it (the
	// AI-agent block + the MCP-server sub-card). We mirror that: plain section content separated by Divider().
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(16.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ BuildHeaderSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
			+ SVerticalBox::Slot().AutoHeight()[ BuildConnectionSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
			+ SVerticalBox::Slot().AutoHeight()[ BuildAgentConfiguratorsSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
			+ SVerticalBox::Slot().AutoHeight()[ BuildExtensionsSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
			+ SVerticalBox::Slot().AutoHeight()[ BuildFooterSection() ]
		]
	];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::UnderlinedLabel(const TAttribute<FText>& Text)
{
	// The underline must span ONLY the label text, never the full row width (issue #80 item 3). An SHorizontalBox
	// with a single AutoWidth slot shrinks to the text's width, so the 1px rule drawn beneath it (Slate has no
	// text-underline run) is exactly as wide as the letters and cannot collide with the row's right-hand buttons.
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.TimelineLabel"))
				.Text(Text)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1, 0, 0)
			[
				SNew(SBox).HeightOverride(1.0f)
				[
					SNew(SImage).Image(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.ConnectingLine"))
				]
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildHeaderSection()
{
	const FString Version = PluginVersion.IsEmpty() ? TEXT("0.2.0") : PluginVersion;

	// Base-config rows (Unity .content-item): a left label column + a right field column. The label column width
	// matches Unity's .content-item label (~85px); the field column fills the rest so every field's left edge lines up.
	auto MakeConfigRow = [](const FText& Label, const TSharedRef<SWidget>& Field)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(85.0f)
				[
					SNew(STextBlock)
					.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
					.Text(Label)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				Field
			];
	};

	// Issue #80 item 1: Log Level is now a real dropdown (SComboBox) listing all seven levels, writing the choice
	// back through the view-model (the same persisted LogLevel field the read-only display used to show).
	const TSharedRef<SWidget> LogLevelField = SNew(SComboBox<TSharedPtr<FString>>)
		.OptionsSource(&LogLevelItems)
		.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
		{
			return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
		})
		.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewItem, ESelectInfo::Type)
		{
			if (NewItem.IsValid() && IsViewModelValid())
				ViewModel->SetLogLevel(*NewItem);
		})
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return FText::FromString(IsViewModelValid() ? ViewModel->GetLogLevel() : TEXT("Info"));
			})
		];

	const TSharedRef<SWidget> VersionField = SNew(STextBlock)
		.Text(FText::FromString(Version));

	// The header is NOT carded anymore (issue #80 item 6) — plain content with the logo top-right.
	return SNew(SHorizontalBox)
		// Base-config column.
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)[ MakeConfigRow(LOCTEXT("LogLevel", "Log Level"), LogLevelField) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				// Issue #80 item 8: the Open-log button uses the compact (short) style so its row is vertically tight.
				MakeConfigRow(LOCTEXT("OpenLogFile", "Log file"),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						UnrealMcpStyleWidgets::CompactTextButton(LOCTEXT("OpenLog", "Open log file"),
							FOnClicked::CreateLambda([]()
							{
								const FString ProjectLogPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
								FPlatformProcess::ExploreFolder(*ProjectLogPath);
								return FReply::Handled();
							}))
					])
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)[ MakeConfigRow(LOCTEXT("VersionLbl", "Version"), VersionField) ]
		]
		// AI-cube logo (top-right).
		+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(15, 0, 0, 0)
		[
			SNew(SBox).WidthOverride(60.0f).HeightOverride(60.0f)
			[
				SNew(SImage).Image(LogoBrush)
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildConnectionSection()
{
	// The Connection section is NOT carded (issue #80 item 6) — flat content matching Unity: a header row
	// ("Connection" + Custom/Cloud segmented top-right), then the mode-specific body, then the status row. The
	// blue card is reserved for the MCP-server sub-block only (built in BuildCustomServerSection).
	return SNew(SVerticalBox)
		// Header row: title + Custom/Cloud segmented (right-aligned to the shared right edge).
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ UnrealMcpStyleWidgets::SectionHeader(LOCTEXT("ConnHeader", "Connection")) ]
			+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Center)
			[
				SNew(SBox).MinDesiredWidth(RightControlColumnWidth).HAlign(HAlign_Right)
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
		]
		// Cloud auth row (masked token + Revoke / Authorize) — visible only in Cloud mode.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[ BuildCloudAuthRow() ]
		// Custom Server URL + validation (NOT part of the dot cluster) — visible only in Custom mode.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[ BuildCustomServerSection() ]
		// The connection cluster (Unity's .connection-timeline): a 2-column layout where the LEFT column is one
		// continuous vertical rail [MCP-server dot] → [FillHeight line] → [Unreal dot], and the RIGHT column holds
		// the matching content rows. The per-row approach (round-2) trapped each line inside an AutoHeight row with
		// no vertical slack — structurally incapable of a continuous line. Here the line slot lives BETWEEN the two
		// dots at the CLUSTER level, so it has the full inter-dot height as slack to fill (Unity's .timeline-line).
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
		[
			BuildConnectionCluster()
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildConnectionCluster()
{
	// The Unreal status dot — driven by the live connection state.
	TAttribute<EDot> UnrealDot = TAttribute<EDot>::Create([this]()
	{
		if (!IsViewModelValid())
			return EDot::Offline;
		switch (ViewModel->GetConnectionState())
		{
			case EUnrealMcpConnectionState::Connected:  return EDot::Online;
			case EUnrealMcpConnectionState::Connecting:  return EDot::Ring;
			// Degraded must NOT use the (green) Ring brush — that reads as healthy. Show Offline.
			case EUnrealMcpConnectionState::Degraded:    return EDot::Offline;
			default:                                     return EDot::Offline;
		}
	});

	// The MCP-server dot + the line below it exist only in Custom mode (the MCP-server content row is Custom-only).
	// In Cloud mode the rail collapses to just the Unreal dot, leaving no dangling line above it.
	TAttribute<EVisibility> CustomOnly = MakeAttributeLambda([this]() -> EVisibility
	{
		return (IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom)
			? EVisibility::Visible : EVisibility::Collapsed;
	});

	UnrealMcpStyleWidgets::FTimelineRailDot McpPoint;
	McpPoint.DotState = EDot::Offline;
	McpPoint.DotVisibility = CustomOnly;
	// The connecting line runs DOWN from the MCP-server dot to the Unreal dot — present only in Custom mode.
	McpPoint.LineBelowVisibility = CustomOnly;
	// Nudge the dot down to centre it on the "MCP server" label inside the card (8px card padding + ~half a line).
	McpPoint.DotTopPadding = 8.0f;

	UnrealMcpStyleWidgets::FTimelineRailDot UnrealPoint;
	UnrealPoint.DotState = UnrealDot;
	UnrealPoint.DotVisibility = EVisibility::Visible;
	// Last point — no line below it (Unity's .timeline-point-last).
	UnrealPoint.LineBelowVisibility = EVisibility::Collapsed;
	// Small nudge to centre the dot on the "Unreal: <status>" label's text line.
	UnrealPoint.DotTopPadding = 2.0f;

	return SNew(SHorizontalBox)
		// LEFT column: the continuous timeline rail owning both dots + the line between them. VAlign_Fill so the
		// rail matches the content column's height and the FillHeight line slot can absorb the full inter-dot slack.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Fill).Padding(0, 0, 8, 0)
		[
			UnrealMcpStyleWidgets::TimelineRail({ McpPoint, UnrealPoint })
		]
		// RIGHT column: the stacked content rows, one per dot. Each row's top aligns with its dot in the rail.
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Fill)
		[
			SNew(SVerticalBox)
			// MCP-server card body (Custom-only) — aligns with the MCP-server dot.
			+ SVerticalBox::Slot().AutoHeight()[ BuildMcpServerCard() ]
			// Unreal status row — aligns with the Unreal dot.
			+ SVerticalBox::Slot().AutoHeight()[ BuildUnrealStatusRow() ]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildUnrealStatusRow()
{
	// The "Unreal: <status>" row — underlined label + Connect/Disconnect/Stop. The dot for this row lives in the
	// shared rail (BuildConnectionCluster), so this row carries only its content.
	return SNew(SHorizontalBox)
		// The underlined "Unreal: <status>" label — spans only the text (issue #80 item 3).
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			UnderlinedLabel(TAttribute<FText>::Create([this]()
			{
				return IsViewModelValid()
					? FUnrealMcpEditorViewModel::GetStatusLabel(ViewModel->GetConnectionState())
					: FText::GetEmpty();
			}))
		]
		// Connect / Disconnect / Stop button — right-aligned to the shared right edge.
		+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Center)
		[
			SNew(SBox).MinDesiredWidth(RightControlColumnWidth).HAlign(HAlign_Right)
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
		];
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
			UnrealMcpStyleWidgets::StyledButton("UnrealMcp.Button.Secondary",
				SNew(STextBlock).Text_Lambda([this]()
				{
					return (IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Pending)
						? LOCTEXT("CancelAuth", "Cancel") : LOCTEXT("Authorize", "Authorize");
				}),
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
	// The Server URL row + validation — Custom-only, and NOT part of the dot cluster (it has no timeline point in
	// the reference). The MCP-server sub-card moved into the cluster's content column (BuildMcpServerCard) so the
	// connecting line can run continuously from the MCP-server dot down to the Unreal dot.
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		// Server URL row — label left, input right-aligned to the shared right edge (issue #80 item 4).
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
		];

	Body->SetVisibility(MakeAttributeLambda([this]() -> EVisibility
	{
		return (IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom)
			? EVisibility::Visible : EVisibility::Collapsed;
	}));
	return Body;
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildMcpServerCard()
{
	// The "MCP server" sub-card: the ONE blue rounded card kept in the Connection section (Unity .frame-mcp-server).
	// The card's timeline DOT now lives in the shared rail (BuildConnectionCluster) so the connecting line is
	// continuous from this dot down to the Unreal dot; this card carries only its content. The whole card is
	// Custom-only — it shares the same visibility predicate as the rail's MCP-server dot + line above it.
	TSharedRef<SWidget> Card = UnrealMcpStyleWidgets::StyledCard(
		SNew(SVerticalBox)
		// MCP server header row (underlined label + teal Start, right-aligned to the shared edge).
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ UnderlinedLabel(LOCTEXT("McpServer", "MCP server")) ]
			+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Center)
			[
				SNew(SBox).MinDesiredWidth(RightControlColumnWidth).HAlign(HAlign_Right)
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
		]
		// Transport (stdio / http) segmented — indented to the section-title start, control right-aligned.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[ BuildTransportSelector() ]
		// Authorization (none / required) + masked token + New.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[ BuildCustomAuthSelector() ]);

	Card->SetVisibility(MakeAttributeLambda([this]() -> EVisibility
	{
		return (IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom)
			? EVisibility::Visible : EVisibility::Collapsed;
	}));
	return Card;
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildTransportSelector()
{
	// Issue #80 item 4: the transport line's label fills, and the segmented control right-aligns to the same edge
	// as the MCP-server Start button / the server URL input — one consistent right edge for the whole sub-card.
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock)
			.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
			.Text(LOCTEXT("TransportLabel", "Transport"))
		]
		+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Center)
		[
			SNew(SBox).MinDesiredWidth(RightControlColumnWidth).HAlign(HAlign_Right)
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
			]
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildCustomAuthSelector()
{
	return SNew(SVerticalBox)
		// Authorization Token (none / required) row — label fills, segmented right-aligned to the shared edge.
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
				.Text(LOCTEXT("AuthTokenLabel", "Authorization Token"))
			]
			+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Center)
			[
				SNew(SBox).MinDesiredWidth(RightControlColumnWidth).HAlign(HAlign_Right)
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
				.ButtonStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FButtonStyle>("UnrealMcp.Button.Compact"))
				.HAlign(HAlign_Center).VAlign(VAlign_Center)
				.ToolTipText(LOCTEXT("RevealHint", "Press and hold to reveal the token; it re-masks on release."))
				.OnPressed_Lambda([this]() { bRevealToken = true; })
				.OnReleased_Lambda([this]() { bRevealToken = false; })
				[
					SNew(STextBlock).Font(FUnrealMcpStyle::CompactButtonFont()).Text(LOCTEXT("Reveal", "Reveal"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
			[
				UnrealMcpStyleWidgets::CompactTextButton(LOCTEXT("New", "New"),
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
	// The AI Agent Configurators panel (§7/§8). It keeps its own framed card (Unity reserves the blue frame for the
	// AI-agent block) — one of the few elements that stays carded per issue #80 item 6. Bound to the shared
	// view-model + the runtime connection-info provider.
	return SNew(SUnrealMcpAgentConfigurators)
		.ViewModel(ViewModel)
		.ConnectionInfoProvider(ConnectionInfoProvider);
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildExtensionsSection()
{
	// The Extensions section (issue #78): a styled header + a placeholder row. NOT carded (issue #80 item 6) — Unity
	// renders the Extensions list flat under its header, with each row a bold name + Install button + description below.
	return SNew(SVerticalBox)
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
					.Font(FUnrealMcpStyle::SectionTitleFont())
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
		];
}

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildFooterSection()
{
	// The footer is NOT carded (issue #80 item 6) — flat content matching Unity: "Found an issue?" + a button row,
	// a divider, then the thanks text on the left with the GitHub Star vertically centered on the same line (item 7).
	return SNew(SVerticalBox)
		// "Found an issue?"
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Font(FUnrealMcpStyle::SubHeaderFont())
			.Text(LOCTEXT("FoundIssue", "Found an issue?"))
		]
		// Help/Talk (Discord) · Bug Report (GitHub) · Restart bridge.
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
		// Divider before the thanks block (Unity .divider).
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
		// Thanks text + Sincerely on the left; the gold GitHub Star vertically centered on the same row (issue #80 item 7).
		+ SVerticalBox::Slot().AutoHeight()
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
