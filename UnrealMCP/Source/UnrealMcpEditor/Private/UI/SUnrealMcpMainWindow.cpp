// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpMainWindow.h"
#include "UI/SUnrealMcpAgentConfigurators.h"
#include "UI/SUnrealMcpAgentWidgets.h"
#include "UI/UnrealMcpAuxWindows.h"
#include "UI/UnrealMcpExternalLinks.h"
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
#include "HAL/PlatformTime.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

using UnrealMcpStyleWidgets::EDot;

namespace
{
	// Footer links (mirrors the Unity reference footer: Discord Help/Talk, GitHub bug report, GitHub star).
	// Single source of truth in UnrealMcpExternalLinks.h so the dev-control bridge reports the SAME urls.
	const FString DiscordUrl = FUnrealMcpExternalLinks::Discord();
	const FString IssuesUrl  = FUnrealMcpExternalLinks::Issues();
	const FString StarUrl    = FUnrealMcpExternalLinks::Star();

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
	SendAgentConfigRequest = InArgs._SendAgentConfigRequest;

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
			// The Connection section is the 3-item status timeline (Unreal → MCP server → AI agents). Issue #97:
			// the AI-agents element inside the cluster is now a READ-ONLY status row (BuildAiAgentsStatusRow) listing
			// the connected agents, NOT the configurator — mirroring Unity's TimelinePointAiAgent. The configurator
			// dropdown is a separate section below (next slot).
			+ SVerticalBox::Slot().AutoHeight()[ BuildConnectionSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
			// The AI Agent Configurators panel — its own standalone top-level section BELOW the Connection section
			// (issue #97 restored the pre-#93 placement). It is the agent-picker/configurator dropdown, distinct
			// from the AI-agents STATUS row inside the Connection timeline above; the connection dots do not anchor
			// to it. Mirrors Unity's separate "AI agent" dropdown section below the connection timeline.
			+ SVerticalBox::Slot().AutoHeight()[ BuildAgentConfiguratorsSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
			+ SVerticalBox::Slot().AutoHeight()[ BuildExtensionsSection() ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
			+ SVerticalBox::Slot().AutoHeight()[ BuildFooterSection() ]
		]
	];

	// Issue #99: a low-frequency active timer drives the bounded Cloud-auth Connecting→Failed timeout so the UI never
	// wedges in the transient "Starting MCP bridge…" state if the sidecar never handshakes. TickAuthTimeout is a no-op
	// in every state but Connecting, so this is effectively idle (and cheap) the rest of the time. The widget owns the
	// timer's lifetime — it is unregistered automatically when the widget is destroyed, so no dangling view-model touch.
	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SUnrealMcpMainWindow::TickCloudAuthTimeout));
}

EActiveTimerReturnType SUnrealMcpMainWindow::TickCloudAuthTimeout(double InCurrentTime, float /*InDeltaTime*/)
{
	if (IsViewModelValid())
		ViewModel->TickAuthTimeout(FPlatformTime::Seconds());
	return EActiveTimerReturnType::Continue;
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
	// In Cloud mode the rail collapses to the Unreal dot + the AI-agents dot, with no dangling MCP-server segment.
	TAttribute<EVisibility> CustomOnly = MakeAttributeLambda([this]() -> EVisibility
	{
		return (IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Custom)
			? EVisibility::Visible : EVisibility::Collapsed;
	});

	// Issue #93: the connection timeline must read top-to-bottom EXACTLY as ARCHITECTURE §7 specifies —
	// "Unreal point → MCP server point → AI-agent point" (the pre-#93 cluster had MCP-server ABOVE Unreal and
	// omitted the AI-agent point entirely).
	//
	// Issue #97 (operator alignment pass): the timeline is now built ROW-BY-ROW via TimelineRow rather than as a
	// single rail column beside a single content column. The old two-column split positioned every dot via two
	// even-weight FillHeight line segments, so the 2nd/3rd dots floated to the cluster's vertical midpoint and
	// landed in the MIDDLE of tall rows (the MCP dot sat at "Authorization"; the AI-agents dot sat at "Copilot")
	// instead of on their header labels. TimelineRow keeps each dot pinned to the TOP of its OWN row's content
	// (its label) while still drawing a continuous line (each row's line runs from its dot to the row bottom =
	// the next row's dot). DotTopPadding is now only a small per-label centring nudge, not a coarse positioner.

	// Point 1 — Unreal status (always present; first row).
	UnrealMcpStyleWidgets::FTimelineRailDot UnrealPoint;
	UnrealPoint.DotState = UnrealDot;
	UnrealPoint.DotVisibility = EVisibility::Visible;
	// The line runs DOWN from the Unreal dot to the next row (the MCP row in Custom mode, or — when the MCP row is
	// collapsed in Cloud mode — straight on to the AI-agents row).
	UnrealPoint.LineBelowVisibility = EVisibility::Visible;
	// Centre the 14px dot on the "Unreal: <status>" underlined label's text line.
	UnrealPoint.DotTopPadding = 6.0f;

	// Point 2 — MCP server (Custom-only; second row).
	// Issue #97 (operator follow-up): drive the dot from the LIVE local-server run-state instead of a hardcoded
	// EDot::Offline — Online (green) when the local gamedev-mcp-server (#95/#96 FUnrealMcpServerManager, surfaced
	// via ViewModel->IsLocalServerRunning()) is running, else Offline. Mirrors how UnrealPoint reads
	// GetConnectionState() and AgentsPoint reads GetAiAgents(). There is no separate "starting/launching" transient
	// in the view-model (IsLocalServerRunning is a single bool sink), so no EDot::Ring state — Online/Offline only,
	// no new plumbing. This dot only renders in Custom mode (the whole MCP row collapses in Cloud — see below),
	// which matches the local-server feature's Custom+http-only gating.
	UnrealMcpStyleWidgets::FTimelineRailDot McpPoint;
	McpPoint.DotState = TAttribute<EDot>::Create([this]()
	{
		return (IsViewModelValid() && ViewModel->IsLocalServerRunning()) ? EDot::Online : EDot::Offline;
	});
	McpPoint.DotVisibility = EVisibility::Visible; // the whole row collapses in Cloud mode (see below), so the dot is always visible WITHIN the row.
	// The line below the MCP-server dot runs DOWN to the AI-agents row.
	McpPoint.LineBelowVisibility = EVisibility::Visible;
	// The MCP content is a card whose header label sits below the card's own 8px border padding; nudge the dot down
	// to centre it on that "MCP server" header text (was a coarse 8px positioner in the old rail; here it only
	// accounts for the card border + label leading).
	McpPoint.DotTopPadding = 14.0f;

	// Point 3 — AI agents (always present; last row, so no line below it — Unity's .timeline-point-last).
	// Issue #97: the dot is driven by live data — Online when any agent is connected (GetAiAgents().Num() > 0),
	// else Offline — mirroring how UnrealPoint's dot reflects GetConnectionState() and Unity's aiAgentStatusCircle
	// flips connected/disconnected from the agent list. Replaces the previous hardcoded EDot::Offline.
	UnrealMcpStyleWidgets::FTimelineRailDot AgentsPoint;
	AgentsPoint.DotState = TAttribute<EDot>::Create([this]()
	{
		return (IsViewModelValid() && ViewModel->GetAiAgents().Num() > 0) ? EDot::Online : EDot::Offline;
	});
	AgentsPoint.DotVisibility = EVisibility::Visible;
	AgentsPoint.LineBelowVisibility = EVisibility::Collapsed;
	// Centre the dot on the "AI agents" underlined label's text line (same label shape as the Unreal-status row).
	AgentsPoint.DotTopPadding = 6.0f;

	// The MCP-server row is Custom-only: in Cloud mode the WHOLE row (its rail cell + the card) collapses, so the
	// Unreal row's line connects straight to the AI-agents row with no dangling MCP segment.
	TSharedRef<SWidget> McpRow = UnrealMcpStyleWidgets::TimelineRow(McpPoint, BuildMcpServerCard(), /*bIsLast*/ false, /*bLineAbove*/ true);
	McpRow->SetVisibility(CustomOnly);

	return SNew(SVerticalBox)
		// (1) Unreal status row — dot pinned to the "Unreal: <status>" label. First row, so no line above it.
		+ SVerticalBox::Slot().AutoHeight()
		[
			UnrealMcpStyleWidgets::TimelineRow(UnrealPoint, BuildUnrealStatusRow(), /*bIsLast*/ false, /*bLineAbove*/ false)
		]
		// (2) MCP-server card row (Custom-only) — dot pinned to the "MCP server" card header. bLineAbove fills the
		// dot's ~14px top nudge so the connector meets the dot flush (uniform spacing, issue #97 alignment pass).
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			McpRow
		]
		// (3) AI agents STATUS row — dot pinned to the "AI agents" label. Issue #97: this is a READ-ONLY readout of
		// the connected-agents list (BuildAiAgentsStatusRow), mirroring Unity's TimelinePointAiAgent, NOT the
		// configurator. The configurator dropdown moved back out to its own standalone section below Connection.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			UnrealMcpStyleWidgets::TimelineRow(AgentsPoint, BuildAiAgentsStatusRow(), /*bIsLast*/ true, /*bLineAbove*/ true)
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

TSharedRef<SWidget> SUnrealMcpMainWindow::BuildAiAgentsStatusRow()
{
	// Issue #97: the "AI agents" connection-timeline content row — the read-only STATUS readout of the agents
	// currently connected (mirrors Unity's TimelinePointAiAgent / aiAgentLabelsContainer). NOT the configurator
	// (which is now its own standalone section below the Connection section). The dot for this row lives in the
	// shared rail (BuildConnectionCluster) and is driven Online when GetAiAgents().Num() > 0; this row carries
	// only the underlined label + the agent-name list (or an empty-state line).
	//
	// The label uses UnderlinedLabel (same style as the "Unreal: <status>" and "MCP server" rows) so the rail dot
	// pins to the top of its text line. Below the label, one Description line per connected agent name, or a single
	// "No agents connected" Description line when the list is empty (Unity shows the placeholder "AI agent" label;
	// we use an explicit empty-state line per the task brief). The list is rebuilt every frame from the live
	// view-model via a Description bound to a lambda — there is no per-agent interactive widget, so a single joined
	// text block (one name per line) is the simplest faithful readout and stays a thin view over GetAiAgents().
	return SNew(SVerticalBox)
		// Underlined "AI agents" label — spans only the text (matches the other timeline labels).
		+ SVerticalBox::Slot().AutoHeight()
		[
			UnderlinedLabel(LOCTEXT("AiAgentsLabel", "AI agents"))
		]
		// The connected-agent list (one name per line) or the empty state, as a dimmed Description block.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			UnrealMcpStyleWidgets::Description(TAttribute<FText>::Create([this]()
			{
				if (!IsViewModelValid())
					return LOCTEXT("NoAgents", "No agents connected");
				const TArray<FString>& Agents = ViewModel->GetAiAgents();
				if (Agents.Num() == 0)
					return LOCTEXT("NoAgents", "No agents connected");
				return FText::FromString(FString::Join(Agents, TEXT("\n")));
			}))
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
					// Both Connecting (awaiting the sidecar handshake to flush a queued auth-start, issue #99) and
					// Pending (device-code in the browser) are in-flight states the user can Cancel.
					if (!IsViewModelValid())
						return LOCTEXT("Authorize", "Authorize");
					const EUnrealMcpDeviceAuthState State = ViewModel->GetDeviceAuthState();
					return (State == EUnrealMcpDeviceAuthState::Pending || State == EUnrealMcpDeviceAuthState::Connecting)
						? LOCTEXT("CancelAuth", "Cancel") : LOCTEXT("Authorize", "Authorize");
				}),
				FOnClicked::CreateLambda([this]()
				{
					if (!IsViewModelValid())
						return FReply::Handled();
					const EUnrealMcpDeviceAuthState State = ViewModel->GetDeviceAuthState();
					if (State == EUnrealMcpDeviceAuthState::Pending || State == EUnrealMcpDeviceAuthState::Connecting)
						ViewModel->CancelAuth();
					else
						ViewModel->Authorize(FPlatformTime::Seconds()); // issue #99: arm the bounded connect timeout
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
		// Connecting (issue #99): transient "starting/awaiting the MCP bridge" line shown while a queued auth-start
		// waits for the sidecar handshake to flush. Amber, mirrors the Connecting connection-status colour.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FUnrealMcpEditorViewModel::GetStatusColor(EUnrealMcpConnectionState::Connecting)))
			.Visibility_Lambda([this]()
			{
				return (IsViewModelValid() && ViewModel->GetDeviceAuthState() == EUnrealMcpDeviceAuthState::Connecting)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Text(LOCTEXT("AuthConnecting", "Starting MCP bridge… authorization will continue once it connects."))
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
					// Issue #95: the MCP-server card's Start/Stop button launches + supervises the LOCAL
					// gamedev-mcp-server (FUnrealMcpServerManager) via the view-model — REPLACING the interim #94
					// wiring that (re)started the bridge sidecar. The label toggles Start↔Stop with the live server
					// state; the action is gated to Custom mode + http transport (ToggleLocalServer no-ops otherwise,
					// and the button is disabled outside that mode so a stray click can never spawn a server). The
					// card itself is already Custom-only (visibility predicate below); the IsEnabled guard further
					// disables it for Custom+stdio. NOT ViewModel->Connect() (the Unreal-side SignalR connect, owned
					// by the "Unreal: <status>" row) and NOT OnRestartBridge (the footer's bridge restart).
					SNew(SButton)
					.ButtonStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FButtonStyle>("UnrealMcp.Button.Primary"))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					// Disabled (so a stray click cannot spawn a server) outside Custom+http; ToggleLocalServer also
					// no-ops in that case as a second guard.
					.IsEnabled_Lambda([this]() { return IsViewModelValid() && ViewModel->IsLocalServerLaunchable(); })
					.OnClicked_Lambda([this]()
					{
						if (IsViewModelValid())
							ViewModel->ToggleLocalServer();
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return IsViewModelValid() ? ViewModel->GetLocalServerButtonText() : LOCTEXT("Start", "Start");
						})
					]
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
	// The AI Agent Configurators panel (§7/§8) — the agent-picker dropdown + per-agent configuration. Issue #97
	// moved it back OUT of the Connection cluster to its OWN standalone top-level section below the Connection
	// section (its pre-#93 placement); the connection status dots no longer anchor to it. It keeps its own framed
	// card (Unity reserves the blue frame for the AI-agent block) — one of the few elements that stays carded per
	// issue #80 item 6. Bound to the shared view-model + the runtime connection-info provider. This is distinct
	// from BuildAiAgentsStatusRow (the read-only connected-agents readout inside the Connection timeline).
	return SAssignNew(AgentConfiguratorsPanel, SUnrealMcpAgentConfigurators)
		.ViewModel(ViewModel)
		.ConnectionInfoProvider(ConnectionInfoProvider)
		.SendRequest(SendAgentConfigRequest);
}

void SUnrealMcpMainWindow::DeliverAgentConfigResult(const TSharedPtr<FJsonObject>& Result)
{
	if (AgentConfiguratorsPanel.IsValid())
		AgentConfiguratorsPanel->OnAgentConfigResult(Result);
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
			// "Check" — opens the Serialization Check window (Unity-MCP parity). Secondary style, beside the
			// support links. Delegates to the static aux-window invoker so it focuses an already-open tab.
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary", LOCTEXT("Check", "Check"),
					FOnClicked::CreateLambda([]() { FUnrealMcpAuxWindows::TryInvokeSerializationCheckTab(); return FReply::Handled(); }))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Secondary", LOCTEXT("RestartBridge", "Restart bridge"),
					FOnClicked::CreateLambda([this]() { OnRestartBridge.ExecuteIfBound(); return FReply::Handled(); }))
			]
		]
		// Divider before the thanks block (Unity .divider).
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ UnrealMcpStyleWidgets::Divider() ]
		// Issue #93: the "Thank you …" line takes the FULL WIDTH on its own row (auto-wrap), with NO button beside it.
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT("ThanksText", "Thank you for using AI Game Developer. If you like it, please give the project a star on GitHub."))
		]
		// Issue #93: the NEXT row splits horizontally — "Sincerely,\nIvan Murzak" (two lines, left-aligned) on the
		// LEFT, and the gold GitHub Star button on the RIGHT, vertically centered on the same row. The signature uses
		// the SAME normal body-text style as the "Thank you …" line above (it previously used the dimmed 12px
		// "UnrealMcp.Text.Description" style — dropped here so it visually matches normal body text).
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Justification(ETextJustify::Left)
				.Text(LOCTEXT("Sincerely", "Sincerely,\nIvan Murzak"))
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
