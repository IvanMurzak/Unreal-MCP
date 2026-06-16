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

	/**
	 * A connection-timeline indicator column (Unity's .timeline-indicator): a fixed 20px-wide column holding
	 * the 14px status dot centred horizontally, with a 2px vertical connecting-line (rgb(80,80,80), Unity's
	 * .timeline-line) running DOWN from the dot toward the next timeline point when @p DrawLineBelow is true.
	 * The line FillHeight-fills the slot below the dot so it stretches to whatever height the row resolves to
	 * (Unity uses position:absolute bottom:-10px; Slate has no absolute positioning, so a fill slot that grows
	 * with the row is the structural equivalent). The last point in the cluster passes DrawLineBelow=false
	 * (Unity's .timeline-point-last hides the line) — supplied as a TAttribute so it can react to mode changes
	 * (e.g. the MCP-server point only exists in Custom mode, so the dot above it draws no dangling line in Cloud).
	 *
	 * Width 20px + the line centred at x=9px mirror the USS exactly; the dot is 14px so a centred line at the
	 * dot's horizontal midpoint reads as one continuous vertical rule joining the dots.
	 */
	inline TSharedRef<SWidget> TimelineIndicator(const TAttribute<EDot>& DotState, const TAttribute<bool>& DrawLineBelow)
	{
		return SNew(SBox).WidthOverride(20.0f)
		[
			SNew(SVerticalBox)
			// The status dot, centred horizontally in the 20px column.
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				StatusDot(DotState)
			]
			// The 2px connecting-line below the dot — fills the remaining row height, centred under the dot.
			+ SVerticalBox::Slot().FillHeight(1.0f).HAlign(HAlign_Center).Padding(0, 2, 0, 0)
			[
				SNew(SBox).WidthOverride(2.0f)
				.Visibility_Lambda([DrawLineBelow]()
				{
					return DrawLineBelow.Get() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
				})
				[
					SNew(SImage).Image(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.ConnectingLine"))
				]
			]
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
