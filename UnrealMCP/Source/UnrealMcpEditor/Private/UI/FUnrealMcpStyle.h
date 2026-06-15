// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateTypes.h"

/**
 * The "AI Game Developer" Slate style set (docs/ARCHITECTURE.md §7) — the C++/Slate analog of Unity-MCP's
 * shared USS (Editor/UI/uss/common/*.uss). It centralizes the palette, card/frame/input brushes, status-dot
 * brushes, the segmented-control + toggle-switch button styles, and the icon brushes (AI-cube logo, Discord,
 * GitHub, Star) so the main window stops hard-coding inline colours / default FAppStyle brushes and instead
 * reads one source of truth that matches the Unity reference 1:1.
 *
 * Lifetime: a single shared FSlateStyleSet registered with FSlateStyleRegistry at module startup and
 * unregistered at shutdown. Initialize()/Shutdown() are idempotent (safe to call twice). Every colour
 * constant below is the EXACT value lifted from the Unity USS (see the issue's palette table); keep them in
 * lockstep with `Editor/UI/uss/common/*.uss` if the reference ever changes.
 *
 * Usage: widgets read brushes via `FUnrealMcpStyle::Get().GetBrush("UnrealMcp.<Name>")` or the typed
 * accessors, and colours via the inline `static constexpr`/`static` palette helpers (no style-set lookup
 * needed for a raw FLinearColor — they are compile-time constants).
 */
class FUnrealMcpStyle
{
public:
	/** Register the style set (idempotent). Called from the editor module's StartupModule. */
	static void Initialize();
	/** Unregister + destroy the style set (idempotent). Called from ShutdownModule. */
	static void Shutdown();

	/** The shared style set; valid between Initialize() and Shutdown(). */
	static const ISlateStyle& Get();

	/** The style-set name brushes are namespaced under ("UnrealMcpStyle"). */
	static FName GetStyleSetName();

	// --- Palette (EXACT Unity USS values — the issue's "Exact palette" table). ------------------------

	/** Accent teal rgb(175,232,230) — primary-button bg, selected-segment text, Start button. */
	static FLinearColor AccentTeal()        { return FromBytes(175, 232, 230); }
	static FLinearColor AccentTealHover()   { return FromBytes(118, 231, 227); }
	static FLinearColor AccentTealActive()  { return FromBytes(250, 255, 234); }

	/** Status online green rgb(111,226,101). */
	static FLinearColor StatusOnline()      { return FromBytes(111, 226, 101); }
	/** Disconnected / offline orange rgb(220,76,9). */
	static FLinearColor StatusOffline()     { return FromBytes(220, 76, 9); }

	/** Golden (GitHub Star): text rgb(255,215,100), bg rgb(45,40,25), border rgb(180,150,60). */
	static FLinearColor GoldenText()        { return FromBytes(255, 215, 100); }
	static FLinearColor GoldenBg()          { return FromBytes(45, 40, 25); }
	static FLinearColor GoldenBorder()      { return FromBytes(180, 150, 60); }

	/** Alert (red) button bg rgb(88,44,44) — the Revoke button. */
	static FLinearColor AlertButtonBg()     { return FromBytes(88, 44, 44); }

	/** Toggle off #707070 / on #51C663. */
	static FLinearColor ToggleOff()         { return FromBytes(0x70, 0x70, 0x70); }
	static FLinearColor ToggleOn()          { return FromBytes(0x51, 0xC6, 0x63); }

	/** Card / frame background rgba(20,40,69,0.2). */
	static FLinearColor CardBg()            { return FromBytes(20, 40, 69, 51); }   // 0.2 * 255 ≈ 51
	/** Input field bg rgba(0,0,0,0.25) / border rgba(255,255,255,0.08). */
	static FLinearColor InputBg()           { return FLinearColor(0.0f, 0.0f, 0.0f, 0.25f); }
	static FLinearColor InputBorder()       { return FLinearColor(1.0f, 1.0f, 1.0f, 0.08f); }

	/** Segmented track rgba(255,255,255,0.05) / selected highlight rgba(0,0,0,0.4). */
	static FLinearColor SegmentedTrack()    { return FLinearColor(1.0f, 1.0f, 1.0f, 0.05f); }
	static FLinearColor SegmentedHighlight(){ return FLinearColor(0.0f, 0.0f, 0.0f, 0.4f); }

	/** Dimmed description text (Unity's helpbox text) — section descriptions / hints. */
	static FLinearColor DescriptionText()   { return FromBytes(160, 160, 160); }
	/** Default body text. */
	static FLinearColor DefaultText()       { return FromBytes(210, 210, 210); }

	/** The connecting-line colour (2px rgb(80,80,80)) joining the connection-timeline dots. */
	static FLinearColor ConnectingLine()    { return FromBytes(80, 80, 80); }

	// --- Fonts (issue: headers 20px bold; section desc 12px; timeline label 13px bold + underline). ---

	static FSlateFontInfo HeaderFont()      { return FCoreStyle::GetDefaultFontStyle("Bold", 20); }
	static FSlateFontInfo TimelineLabelFont(){ return FCoreStyle::GetDefaultFontStyle("Bold", 13); }
	static FSlateFontInfo SubHeaderFont()   { return FCoreStyle::GetDefaultFontStyle("Bold", 11); }
	static FSlateFontInfo DescriptionFont() { return FCoreStyle::GetDefaultFontStyle("Regular", 9); }

private:
	static TSharedRef<class FSlateStyleSet> Create();
	static TSharedPtr<class FSlateStyleSet> StyleInstance;

	/** Build an FLinearColor from 0-255 sRGB bytes (the USS authoring space) with alpha. */
	static FLinearColor FromBytes(uint8 R, uint8 G, uint8 B, uint8 A = 255)
	{
		return FLinearColor(FColor(R, G, B, A));
	}
};
