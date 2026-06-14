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
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "HAL/PlatformApplicationMisc.h"

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
