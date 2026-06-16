// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "HAL/PlatformApplicationMisc.h"
#include "UI/FUnrealMcpStyle.h"

/**
 * Reusable Slate equivalents of Unity's AI-agent-configurator UI templates (docs/ARCHITECTURE.md §7) — the
 * Slate analog of Unity's UI/AiAgentConfigurators/AiAgentConfigurator.cs `Template*` helpers + the
 * UI/Components/AlertPanel.cs widget + the UI/AiAgentConfigurators/ConfigurationElements.cs status row.
 *
 * Unity uses UXML+USS templates loaded by name; Unreal has no UMG/asset dependency in this pure-Slate window, so
 * these are plain static factory functions (and one compound widget) that build the equivalent Slate trees with the
 * matching styling intent:
 *   - TemplateLabelDescription  → a wrapped, slightly-dimmed description label.
 *   - TemplateWarningLabel       → an ORANGE wrapped label (Unity's .warning-label).
 *   - TemplateAlertLabel         → a RED wrapped label (Unity's .alert-label).
 *   - TemplateTextFieldReadOnly  → a read-only, selectable command field WITH a Copy button (Unity's read-only
 *                                  TextField the user copies a command out of).
 *   - TemplateFoldout / First    → an SExpandableArea (Unity's Foldout); "First" starts expanded.
 *   - SUnrealMcpAlertPanel       → title + message + bullet items + optional action button (Unity's AlertPanel).
 *   - MakeConfigurationStatusRow → "Configured (transport)" / "Not configured" + Configure/Reconfigure + Remove
 *                                  (Unity's ConfigurationElements).
 *
 * These are header-only (factory functions + a small SCompoundWidget) so any §7 widget can reuse them without a
 * link-time dependency. Colours match the existing window palette (green/orange/red constants already used in
 * SUnrealMcpMainWindow / SUnrealMcpAgentConfigurators) so the rich content blends with the rest of the UI.
 */
namespace UnrealMcpAgentWidgets
{
	/** Shared palette (kept in lockstep with the inline constants already used by the §7 window widgets). */
	inline FLinearColor DescriptionColor() { return FLinearColor(0.70f, 0.70f, 0.70f); }   // dimmed grey
	inline FLinearColor WarningColor()     { return FLinearColor(0.95f, 0.60f, 0.13f); }   // orange
	inline FLinearColor AlertColor()       { return FLinearColor(0.95f, 0.40f, 0.40f); }   // red
	inline FLinearColor ConfiguredColor()  { return FLinearColor(0.16f, 0.74f, 0.30f); }   // green
	inline FLinearColor PendingColor()     { return FLinearColor(0.85f, 0.65f, 0.20f); }   // amber

	/** Unity's TemplateLabelDescription — a wrapped, dimmed description line. */
	inline TSharedRef<SWidget> TemplateLabelDescription(const FText& Text)
	{
		return SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(DescriptionColor()))
			.Text(Text);
	}

	/** Unity's TemplateWarningLabel — an ORANGE wrapped warning line. */
	inline TSharedRef<SWidget> TemplateWarningLabel(const FText& Text)
	{
		return SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(WarningColor()))
			.Text(Text);
	}

	/** Unity's TemplateAlertLabel — a RED wrapped alert line. */
	inline TSharedRef<SWidget> TemplateAlertLabel(const FText& Text)
	{
		return SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(AlertColor()))
			.Text(Text);
	}

	/**
	 * Unity's read-only TextField the user copies a command out of. Slate's SEditableText/SMultiLineEditableTextBox
	 * is read-only + selectable; we pair it with a Copy button (Unreal has no implicit Ctrl+C affordance hint), so
	 * the user can both select-copy and one-click-copy. The value is shown verbatim (these are command snippets, not
	 * secrets — token masking happens at the snippet-assembly layer, never here).
	 */
	inline TSharedRef<SWidget> TemplateTextFieldReadOnly(const FString& Value)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AllowMultiLine(false)
				.AlwaysShowScrollbars(false)
				.Text(FText::FromString(Value))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0).VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("UnrealMcp", "CopyField", "Copy"))
				.ToolTipText(NSLOCTEXT("UnrealMcp", "CopyFieldHint", "Copy this value to the clipboard."))
				.OnClicked_Lambda([Value]()
				{
					FPlatformApplicationMisc::ClipboardCopy(*Value);
					return FReply::Handled();
				})
			];
	}

	/** Unity's TemplateFoldout — a collapsible section (collapsed by default). */
	inline TSharedRef<SExpandableArea> TemplateFoldout(const FText& Heading, const TSharedRef<SWidget>& Body, bool bInitiallyExpanded = false)
	{
		return SNew(SExpandableArea)
			.InitiallyCollapsed(!bInitiallyExpanded)
			.BorderImage(FAppStyle::GetBrush("NoBorder"))
			.HeaderContent()
			[
				SNew(STextBlock)
				.Text(Heading)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			.BodyContent()
			[
				Body
			];
	}

	/** Unity's TemplateFoldoutFirst — the first foldout, which starts EXPANDED. */
	inline TSharedRef<SExpandableArea> TemplateFoldoutFirst(const FText& Heading, const TSharedRef<SWidget>& Body)
	{
		return TemplateFoldout(Heading, Body, /*bInitiallyExpanded*/ true);
	}
}

/**
 * Style-set-backed reusable widgets (docs/ARCHITECTURE.md §7) — the Slate building blocks the main window
 * composes to reach visual parity with the Unity reference. Each reads brushes/colours from FUnrealMcpStyle
 * (NOT inline literals), so the whole window shares one palette source of truth. Header-only factory
 * functions (no link-time dependency) so any §7 widget can reuse them.
 *
 * Provided:
 *   - StyledCard       → the rounded rgba(20,40,69,0.2) frame-group container (Unity .frame-group).
 *   - SectionHeader    → a 20px-bold section title (Unity .header).
 *   - SegmentedControl → the tab-like Custom/Cloud · stdio/http · none/required toggle (Unity .segmented-control).
 *   - StatusDot        → a 14px online/offline/ring connection dot (Unity .status-indicator-circle*).
 *   - PrimaryButton / AlertButton / SecondaryButton / GoldenButton → the styled buttons (Unity .btn-*).
 *   - IconButton       → a button with a leading icon brush + label (Unity .btn-with-icon).
 */
namespace UnrealMcpStyleWidgets
{
	/** Which status dot to draw — mirrors Unity's .status-indicator-circle-{online,disconnected,external}. */
	enum class EDot : uint8 { Online, Offline, Ring };

	/** The rounded card/frame container (Unity .frame-group). Wraps @p Content with 8px padding. */
	inline TSharedRef<SWidget> StyledCard(const TSharedRef<SWidget>& Content)
	{
		return SNew(SBorder)
			.BorderImage(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Card"))
			.Padding(8.0f)
			[
				Content
			];
	}

	/** A 20px-bold section header (Unity .header). */
	inline TSharedRef<SWidget> SectionHeader(const FText& Title)
	{
		return SNew(STextBlock)
			.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Header"))
			.Text(Title);
	}

	/**
	 * A full-width 1px horizontal rule (Unity .divider — rgb(26,26,26), 10px margin top/bottom). Used between
	 * major sections instead of wrapping each in a card; mirrors the reference's flat section separators.
	 */
	inline TSharedRef<SWidget> Divider()
	{
		return SNew(SBox)
			.HeightOverride(1.0f)
			[
				SNew(SImage).Image(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Divider"))
			];
	}

	/** A dimmed description/hint line (Unity .section-desc). */
	inline TSharedRef<SWidget> Description(const TAttribute<FText>& Text)
	{
		return SNew(STextBlock)
			.AutoWrapText(true)
			.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
			.Text(Text);
	}

	/** A connection-status dot (14x14) bound to a live EDot provider. */
	inline TSharedRef<SWidget> StatusDot(const TAttribute<EDot>& DotState)
	{
		return SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
		[
			SNew(SImage)
			.Image_Lambda([DotState]() -> const FSlateBrush*
			{
				switch (DotState.Get())
				{
					case EDot::Online:  return FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Dot.Online");
					case EDot::Offline: return FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Dot.Offline");
					case EDot::Ring:    return FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Dot.Ring");
					default:            return FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Dot.Offline");
				}
			})
		];
	}

	/** A fixed 2px-wide vertical connecting-line segment (Unity's .timeline-line, rgb(80,80,80)), centred in
	 *  the rail's 20px column. Used as a FillHeight rail segment BETWEEN two dots so it absorbs the full
	 *  inter-dot slack and reads as one continuous rule joining them; @p bVisible collapses it when the
	 *  upper dot does not exist (e.g. the MCP-server point only exists in Custom mode). */
	inline TSharedRef<SWidget> TimelineLineSegment(const TAttribute<EVisibility>& Visibility)
	{
		return SNew(SBox).WidthOverride(2.0f)
			.Visibility(Visibility)
			[
				SNew(SImage).Image(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.ConnectingLine"))
			];
	}

	/**
	 * A connection-timeline RAIL (Unity's .connection-timeline left column): a fixed 20px-wide vertical column
	 * that owns ALL the cluster's status dots and the connecting-line segments BETWEEN them, so the line is a
	 * continuous rule spanning from one dot to the next regardless of the content rows beside it. This is the
	 * structural fix for the per-row approach (which trapped each line inside an AutoHeight row with no slack):
	 * here the line slots are FillHeight at the CLUSTER level, between the dots, with the full inter-dot height
	 * as slack to fill.
	 *
	 * Slots are supplied as alternating dot / line entries. A dot slot is AutoHeight (top-aligned so it pins to
	 * the top of its content row beside it); a line slot is FillHeight(1) so it stretches to fill the gap down to
	 * the next dot. Each line carries its own visibility attribute so a segment collapses when its upper dot is
	 * absent (e.g. in Cloud mode the MCP-server dot + its line vanish, leaving only the Unreal dot).
	 *
	 * Width 20px + the dots/line centred horizontally mirror Unity's .timeline-indicator (20px column, line at
	 * x=9px). The whole rail is VAlign_Fill in its parent SHorizontalBox so it matches the content column's height.
	 */
	struct FTimelineRailDot
	{
		// Default Offline (red) rather than the enum's value-0 Online (green) so a future rail point that forgets to
		// set DotState reads as not-connected, not falsely healthy. Both current call sites set this explicitly.
		TAttribute<EDot> DotState = EDot::Offline;
		TAttribute<EVisibility> DotVisibility = EVisibility::Visible;
		// Visibility of the connecting-line segment BELOW this dot; Collapsed on the last dot (no line below it).
		TAttribute<EVisibility> LineBelowVisibility = EVisibility::Collapsed;
		// Top padding to vertically centre the 14px dot on its sibling content row's first line (the content row
		// may begin below the rail's y=0, e.g. a card's 8px top padding) — purely cosmetic alignment.
		float DotTopPadding = 0.0f;
	};

	inline TSharedRef<SWidget> TimelineRail(const TArray<FTimelineRailDot>& Dots)
	{
		TSharedRef<SVerticalBox> Rail = SNew(SVerticalBox);
		for (int32 DotIndex = 0; DotIndex < Dots.Num(); ++DotIndex)
		{
			const FTimelineRailDot& Point = Dots[DotIndex];
			// The dot — AutoHeight, top-aligned so it pins to the top of its sibling content row.
			Rail->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0, Point.DotTopPadding, 0, 0)
			[
				SNew(SBox).Visibility(Point.DotVisibility)
				[
					StatusDot(Point.DotState)
				]
			];
			// The connecting line BELOW it — FillHeight so it absorbs the slack down to the next dot. Only emit it
			// BETWEEN consecutive dots: a trailing FillHeight slot after the last dot (even Collapsed) would still
			// claim its STRETCH share of the rail's slack, so the inter-dot line would visibly absorb only part of it.
			if (DotIndex < Dots.Num() - 1)
			{
				Rail->AddSlot().FillHeight(1.0f).HAlign(HAlign_Center).Padding(0, 2, 0, 2)
				[
					TimelineLineSegment(Point.LineBelowVisibility)
				];
			}
		}
		return SNew(SBox).WidthOverride(20.0f)
		[
			Rail
		];
	}

	/**
	 * A single connection-timeline ROW: a self-contained 20px rail cell on the LEFT (its status dot pinned to the
	 * TOP, then a FillHeight connecting-line segment that fills the rest of THIS row's height) beside the row's
	 * CONTENT on the right. The two cells share the row's AutoHeight, so the dot sits on the content's FIRST line
	 * (its label) and the line spans from just below the dot to the row's bottom edge.
	 *
	 * Stacking these rows in an SVerticalBox yields BOTH properties the single-column TimelineRail could not give
	 * at once: (1) each dot is anchored to the TOP of its OWN content row — i.e. centred on its label — instead of
	 * floating at the even-split midpoint of the whole cluster (which dragged the 2nd/3rd dots down into the middle
	 * of tall rows like the MCP card); and (2) the line is still continuous, because each row's line runs from its
	 * dot down to the row bottom, which is exactly where the next row's dot begins. @p bIsLast collapses the line
	 * (the last point has none — Unity's .timeline-point-last). @p DotTopPadding is a small per-row nudge to centre
	 * the 14px dot on its label's text line (labels differ slightly in cap-height / leading).
	 *
	 * @p bLineAbove draws a short connecting-line segment ABOVE the dot, filling the DotTopPadding gap between the
	 * row's top edge and the (possibly nudged-down) dot — so the line arrives flush at the dot regardless of how
	 * far DotTopPadding pushes it down (the MCP row nudges its dot ~14px to reach the card-header label; without
	 * this the previous row's line would stop at the row boundary and leave a visible gap above the MCP dot). Pass
	 * false for the FIRST row (nothing is above it). The above-segment uses the row's own DotState visibility-free
	 * connecting brush; it is collapsed when the dot itself is collapsed.
	 *
	 * Width 20px + dot/line centred horizontally mirror Unity's .timeline-indicator (20px column, line at x=9px).
	 */
	inline TSharedRef<SWidget> TimelineRow(
		const FTimelineRailDot& Point,
		const TSharedRef<SWidget>& Content,
		bool bIsLast,
		bool bLineAbove)
	{
		// LEFT: the rail cell. When bLineAbove, a fixed-height connecting segment fills the DotTopPadding gap so the
		// line meets the dot flush (uniform spacing into every dot); otherwise the dot just carries its top padding.
		TSharedRef<SVerticalBox> RailCell = SNew(SVerticalBox);
		if (bLineAbove && Point.DotTopPadding > 0.0f)
		{
			// The 2px-gap below mirrors the inter-dot line's 2px top gap, so the visible spacing dot↔line is uniform
			// at every dot. The segment height = DotTopPadding - 2 (clamped >= 1) so the dot still lands at its label.
			const float AboveHeight = FMath::Max(1.0f, Point.DotTopPadding - 2.0f);
			RailCell->AddSlot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SBox).WidthOverride(2.0f).HeightOverride(AboveHeight).Visibility(Point.DotVisibility)
				[
					SNew(SImage).Image(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.ConnectingLine"))
				]
			];
			RailCell->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0, 2, 0, 0)
			[
				SNew(SBox).Visibility(Point.DotVisibility)
				[
					StatusDot(Point.DotState)
				]
			];
		}
		else
		{
			RailCell->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0, Point.DotTopPadding, 0, 0)
			[
				SNew(SBox).Visibility(Point.DotVisibility)
				[
					StatusDot(Point.DotState)
				]
			];
		}
		if (!bIsLast)
		{
			RailCell->AddSlot().FillHeight(1.0f).HAlign(HAlign_Center).Padding(0, 2, 0, 0)
			[
				TimelineLineSegment(Point.LineBelowVisibility)
			];
		}

		return SNew(SHorizontalBox)
			// The rail cell — VAlign_Fill so its FillHeight line stretches to the full row height (down to the next
			// row's dot). Fixed 20px column, 8px gutter to the content (matches the old cluster's rail->content gap).
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Fill).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(20.0f)
				[
					RailCell
				]
			]
			// The content — its first line (the row's label) is what the top-pinned dot aligns to.
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Fill)
			[
				Content
			];
	}

	/**
	 * A tab-like segmented control (Unity .segmented-control): a rounded track holding N toggle segments;
	 * the selected segment renders its text teal. Each segment is a toggle-style SCheckBox bound to the
	 * shared IsSelected/OnSelect lambdas so it stays a thin view over the caller's state.
	 *
	 * @param Segments  ordered (label, value-tag) pairs. The value-tag is an int the caller maps to its enum.
	 * @param SelectedProvider  returns the currently-selected value-tag.
	 * @param OnSelect  invoked with the chosen value-tag when a segment is clicked.
	 * @param IsEnabled  whole-control enable state (e.g. transport selector locked in Cloud mode).
	 */
	inline TSharedRef<SWidget> SegmentedControl(
		const TArray<TPair<FText, int32>>& Segments,
		TFunction<int32()> SelectedProvider,
		TFunction<void(int32)> OnSelect,
		TAttribute<bool> IsEnabled = true)
	{
		TSharedRef<SHorizontalBox> Track = SNew(SHorizontalBox);
		for (const TPair<FText, int32>& Seg : Segments)
		{
			const FText Label = Seg.Key;
			const int32 Tag = Seg.Value;
			Track->AddSlot().AutoWidth()
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.IsEnabled(IsEnabled)
				.IsChecked_Lambda([SelectedProvider, Tag]()
				{
					return SelectedProvider() == Tag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([OnSelect, Tag](ECheckBoxState NewState)
				{
					if (NewState == ECheckBoxState::Checked)
						OnSelect(Tag);
				})
				.Padding(FMargin(10.0f, 3.0f))
				[
					SNew(STextBlock)
					.Justification(ETextJustify::Center)
					.MinDesiredWidth(40.0f)
					.Text(Label)
					.ColorAndOpacity_Lambda([SelectedProvider, Tag]() -> FSlateColor
					{
						return SelectedProvider() == Tag
							? FSlateColor(FUnrealMcpStyle::AccentTeal())
							: FSlateColor(FUnrealMcpStyle::DescriptionText());
					})
				]
			];
		}

		return SNew(SBorder)
			.BorderImage(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Segmented.Track"))
			.Padding(2.0f)
			[
				Track
			];
	}

	/** A button using one of the style set's FButtonStyle entries; @p Content is the button body. */
	inline TSharedRef<SButton> StyledButton(const FName& StyleKey, const TSharedRef<SWidget>& Content, FOnClicked OnClicked)
	{
		return SNew(SButton)
			.ButtonStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FButtonStyle>(StyleKey))
			.OnClicked(OnClicked)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				Content
			];
	}

	/** A text-only styled button (teal/alert/secondary/golden depending on @p StyleKey). */
	inline TSharedRef<SButton> StyledTextButton(const FName& StyleKey, const FText& Label, FOnClicked OnClicked)
	{
		return StyledButton(StyleKey, SNew(STextBlock).Text(Label), OnClicked);
	}

	/** The compact-button definition (declared above SectionHeader; needs StyledButton + the 11px font). */
	inline TSharedRef<SButton> CompactTextButton(const FText& Label, FOnClicked OnClicked)
	{
		return StyledButton("UnrealMcp.Button.Compact",
			SNew(STextBlock).Font(FUnrealMcpStyle::CompactButtonFont()).Text(Label),
			OnClicked);
	}

	/** A button with a leading icon brush + label (Unity .btn-with-icon) — Discord/GitHub footer buttons. */
	inline TSharedRef<SButton> IconButton(const FName& StyleKey, const FName& IconBrush, const FText& Label, FOnClicked OnClicked)
	{
		return StyledButton(StyleKey,
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(SBox).WidthOverride(18.0f).HeightOverride(18.0f)
				[ SNew(SImage).Image(FUnrealMcpStyle::Get().GetBrush(IconBrush)) ]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(Label) ],
			OnClicked);
	}
}

/**
 * Unity's AlertPanel.cs as a Slate compound widget (docs/ARCHITECTURE.md §7): a bordered notice with a bold title,
 * a wrapped message, an optional bulleted item list, and an optional action button. SetVisible toggles the whole
 * panel. Used for the §7 "Setup Required" / "Reconfiguration Required" / Cloud-auth alerts. Fluent AddItem/SetButton
 * mirror the Unity API 1:1 so callers read the same.
 */
class SUnrealMcpAlertPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpAlertPanel) {}
		SLATE_ARGUMENT(FText, Title)
		SLATE_ARGUMENT(FText, Message)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		ItemsBox = SNew(SVerticalBox);

		ButtonBox = SNew(SHorizontalBox);

		ChildSlot
		[
			SAssignNew(RootBorder, SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(InArgs._Title)
					.ColorAndOpacity(FSlateColor(UnrealMcpAgentWidgets::WarningColor()))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(InArgs._Message)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					ItemsBox.ToSharedRef()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
				[
					ButtonBox.ToSharedRef()
				]
			]
		];
	}

	/** Add a bulleted item line; bRecommended renders it green (Unity's "alert-frame-item-recommended"). */
	SUnrealMcpAlertPanel& AddItem(const FText& Text, bool bRecommended = false)
	{
		ItemsBox->AddSlot().AutoHeight().Padding(0, 1, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(bRecommended ? UnrealMcpAgentWidgets::ConfiguredColor() : UnrealMcpAgentWidgets::DescriptionColor()))
			.Text(Text)
		];
		return *this;
	}

	/** Configure the (single) action button. Calling again replaces the previous button. */
	SUnrealMcpAlertPanel& SetButton(const FText& Text, FSimpleDelegate OnClick)
	{
		ButtonBox->ClearChildren();
		ButtonBox->AddSlot().AutoWidth()
		[
			SNew(SButton)
			.Text(Text)
			.OnClicked_Lambda([OnClick]()
			{
				OnClick.ExecuteIfBound();
				return FReply::Handled();
			})
		];
		return *this;
	}

	/** Show / hide the whole panel (Unity's SetVisible). */
	void SetVisible(bool bVisible)
	{
		if (RootBorder.IsValid())
			RootBorder->SetVisibility(bVisible ? EVisibility::Visible : EVisibility::Collapsed);
	}

private:
	TSharedPtr<SBorder> RootBorder;
	TSharedPtr<SVerticalBox> ItemsBox;
	TSharedPtr<SHorizontalBox> ButtonBox;
};
