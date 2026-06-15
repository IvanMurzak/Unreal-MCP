// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/FUnrealMcpStyle.h"

#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateStyleMacros.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

TSharedPtr<FSlateStyleSet> FUnrealMcpStyle::StyleInstance = nullptr;

// IMAGE_BRUSH_SVG / IMAGE_BRUSH from SlateStyleMacros.h resolve paths against the style set's content root
// (set below to the plugin's Resources dir). We use raw PNGs (the icons copied from the Unity Gizmos set),
// so the plain IMAGE_BRUSH macro applies.
#define UNREALMCP_IMAGE_BRUSH(RelativePath, ...) FSlateImageBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)

FName FUnrealMcpStyle::GetStyleSetName()
{
	static const FName StyleSetName(TEXT("UnrealMcpStyle"));
	return StyleSetName;
}

void FUnrealMcpStyle::Initialize()
{
	if (StyleInstance.IsValid())
		return;

	StyleInstance = Create();
	FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
}

void FUnrealMcpStyle::Shutdown()
{
	if (!StyleInstance.IsValid())
		return;

	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

const ISlateStyle& FUnrealMcpStyle::Get()
{
	// Initialize lazily so a widget constructed before the module's StartupModule ran (e.g. in a spec that
	// news the widget directly) still gets a valid style set.
	if (!StyleInstance.IsValid())
		Initialize();
	return *StyleInstance;
}

TSharedRef<FSlateStyleSet> FUnrealMcpStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(GetStyleSetName());

	// Content root = the plugin's Resources dir (ships the icon PNGs). FindPlugin can return null in odd
	// hosting setups; fall back to the engine content dir so brush registration never crashes (the icons
	// then just resolve to a missing-texture brush, which Slate renders harmlessly).
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
	if (Plugin.IsValid())
		Style->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	else
		Style->SetContentRoot(FPaths::EngineContentDir() / TEXT("Editor/Slate"));

	const FVector2D Icon20(20.0f, 20.0f);
	const FVector2D Icon24(24.0f, 24.0f);
	const FVector2D Logo48(48.0f, 48.0f);

	// --- Icon brushes (read from Resources/*.png). ---
	Style->Set("UnrealMcp.Logo",    new UNREALMCP_IMAGE_BRUSH("ai-cube-logo", Logo48));
	Style->Set("UnrealMcp.Discord", new UNREALMCP_IMAGE_BRUSH("discord_icon", Icon20));
	Style->Set("UnrealMcp.GitHub",  new UNREALMCP_IMAGE_BRUSH("github_icon",  Icon20));
	Style->Set("UnrealMcp.Star",    new UNREALMCP_IMAGE_BRUSH("star_icon",    Icon20));

	// --- Card / frame background: rounded box, rgba(20,40,69,0.2), 16px radius (Unity .frame-group). ---
	Style->Set("UnrealMcp.Card", new FSlateRoundedBoxBrush(CardBg(), 16.0f));

	// --- Input field background: rounded box, rgba(0,0,0,0.25), 6px radius, thin border. ---
	Style->Set("UnrealMcp.Input", new FSlateRoundedBoxBrush(InputBg(), 6.0f, InputBorder(), 1.0f));

	// --- Segmented-control track: rounded box, rgba(255,255,255,0.05), 6px radius. ---
	Style->Set("UnrealMcp.Segmented.Track", new FSlateRoundedBoxBrush(SegmentedTrack(), 6.0f));
	// The selected-segment highlight (the sliding pill behind the active segment).
	Style->Set("UnrealMcp.Segmented.Highlight", new FSlateRoundedBoxBrush(SegmentedHighlight(), 4.0f));

	// --- Status dots (filled circle + ring). A rounded box with radius = half-size renders a circle. ---
	Style->Set("UnrealMcp.Dot.Online",   new FSlateRoundedBoxBrush(StatusOnline(),  7.0f, FVector2D(14.0f, 14.0f)));
	Style->Set("UnrealMcp.Dot.Offline",  new FSlateRoundedBoxBrush(StatusOffline(), 7.0f, FVector2D(14.0f, 14.0f)));
	// External / connecting ring — transparent fill + a 2px coloured border (Unity's *-external/*-connecting).
	Style->Set("UnrealMcp.Dot.Ring",     new FSlateRoundedBoxBrush(FLinearColor::Transparent, 7.0f, StatusOnline(), 2.0f, FVector2D(14.0f, 14.0f)));

	// --- The 2px connecting line that joins the connection-timeline dots vertically. ---
	Style->Set("UnrealMcp.ConnectingLine", new FSlateColorBrush(ConnectingLine()));

	// --- Section header text style (20px bold). ---
	Style->Set("UnrealMcp.Text.Header", FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
		.SetFont(HeaderFont())
		.SetColorAndOpacity(FSlateColor(DefaultText())));

	// --- Timeline label text style (13px bold + underline) — "Unreal: Connected" etc. ---
	{
		FTextBlockStyle TimelineLabel = FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
			.SetFont(TimelineLabelFont())
			.SetColorAndOpacity(FSlateColor(DefaultText()));
		// Slate has no first-class text-underline; the reference look is reproduced with an underlined
		// font where available, else a thin SSeparator under the label (the widget adds that). We expose
		// the bold 13px style here; the underline is drawn by the widget so this stays a plain text style.
		Style->Set("UnrealMcp.Text.TimelineLabel", TimelineLabel);
	}

	// --- Section description text style (12px dimmed). ---
	Style->Set("UnrealMcp.Text.Description", FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
		.SetFont(DescriptionFont())
		.SetColorAndOpacity(FSlateColor(DescriptionText())));

	// --- Teal "primary" button (Start / Authorize-primary). ---
	{
		FButtonStyle Primary = FButtonStyle()
			.SetNormal(FSlateRoundedBoxBrush(AccentTeal(), 8.0f))
			.SetHovered(FSlateRoundedBoxBrush(AccentTealHover(), 8.0f))
			.SetPressed(FSlateRoundedBoxBrush(AccentTealActive(), 8.0f))
			.SetNormalForeground(FSlateColor(FLinearColor::Black))
			.SetHoveredForeground(FSlateColor(FLinearColor::Black))
			.SetPressedForeground(FSlateColor(FLinearColor::Black))
			.SetNormalPadding(FMargin(10.0f, 5.0f))
			.SetPressedPadding(FMargin(10.0f, 5.0f));
		Style->Set("UnrealMcp.Button.Primary", Primary);
	}

	// --- Alert (red) button — Revoke / Stop. ---
	{
		FButtonStyle Alert = FButtonStyle()
			.SetNormal(FSlateRoundedBoxBrush(AlertButtonBg(), 6.0f))
			.SetHovered(FSlateRoundedBoxBrush(FromBytes(255, 110, 110), 6.0f))
			.SetPressed(FSlateRoundedBoxBrush(FromBytes(255, 80, 80), 6.0f))
			.SetNormalForeground(FSlateColor(FromBytes(255, 200, 200)))
			.SetHoveredForeground(FSlateColor(FLinearColor::Black))
			.SetPressedForeground(FSlateColor(FLinearColor::Black))
			.SetNormalPadding(FMargin(10.0f, 5.0f))
			.SetPressedPadding(FMargin(10.0f, 5.0f));
		Style->Set("UnrealMcp.Button.Alert", Alert);
	}

	// --- Golden button (GitHub Star). ---
	{
		FButtonStyle Golden = FButtonStyle()
			.SetNormal(FSlateRoundedBoxBrush(GoldenBg(), 6.0f, GoldenBorder(), 1.0f))
			.SetHovered(FSlateRoundedBoxBrush(FromBytes(60, 52, 30), 6.0f, FromBytes(220, 180, 80), 1.0f))
			.SetPressed(FSlateRoundedBoxBrush(FromBytes(80, 68, 35), 6.0f, FromBytes(255, 200, 100), 1.0f))
			.SetNormalForeground(FSlateColor(GoldenText()))
			.SetHoveredForeground(FSlateColor(FromBytes(255, 225, 130)))
			.SetPressedForeground(FSlateColor(FromBytes(255, 235, 150)))
			.SetNormalPadding(FMargin(10.0f, 5.0f))
			.SetPressedPadding(FMargin(10.0f, 5.0f));
		Style->Set("UnrealMcp.Button.Golden", Golden);
	}

	// --- Secondary button (Disconnect / New / Authorize-secondary / footer link buttons). ---
	{
		FButtonStyle Secondary = FButtonStyle()
			.SetNormal(FSlateRoundedBoxBrush(FromBytes(70, 70, 70), 6.0f, FromBytes(100, 100, 100), 1.0f))
			.SetHovered(FSlateRoundedBoxBrush(FromBytes(90, 90, 90), 6.0f, FromBytes(120, 120, 120), 1.0f))
			.SetPressed(FSlateRoundedBoxBrush(FromBytes(110, 110, 110), 6.0f, FromBytes(120, 120, 120), 1.0f))
			.SetNormalForeground(FSlateColor(FromBytes(200, 200, 200)))
			.SetHoveredForeground(FSlateColor(FLinearColor::White))
			.SetPressedForeground(FSlateColor(FLinearColor::White))
			.SetNormalPadding(FMargin(10.0f, 5.0f))
			.SetPressedPadding(FMargin(10.0f, 5.0f));
		Style->Set("UnrealMcp.Button.Secondary", Secondary);
	}

	return Style;
}

#undef UNREALMCP_IMAGE_BRUSH
