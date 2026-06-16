// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

/**
 * The canonical external-link URLs the "AI Game Developer" window footer launches (docs/ARCHITECTURE.md §7,
 * mirroring the Unity reference footer): Discord Help/Talk, the GitHub issues bug-report page, and the
 * GitHub repo "Star" page. Lifted out of SUnrealMcpMainWindow.cpp into one header so the dev-control bridge
 * can report + validate the SAME url a footer button would launch (`assert intent, don't open` — the
 * headless smoke test reads these without opening a browser), keeping a single source of truth for both.
 */
struct FUnrealMcpExternalLinks
{
	/** Discord invite — the footer "Help / Talk" button. */
	static const TCHAR* Discord() { return TEXT("https://discord.gg/cfbdMZX99G"); }
	/** GitHub issues — the footer "Bug Report" button. */
	static const TCHAR* Issues()  { return TEXT("https://github.com/IvanMurzak/Unreal-MCP/issues"); }
	/** GitHub repo — the footer golden "GitHub Star" button. */
	static const TCHAR* Star()    { return TEXT("https://github.com/IvanMurzak/Unreal-MCP"); }

	/**
	 * Resolve a dev-control external-link name ("help"|"discord", "bug"|"issues", "star"|"github") to the URL
	 * the matching footer button would launch. Returns true + the URL on a match; false for an unknown name.
	 * Pure — never opens a browser (the dev-control `external-link` action asserts intent only).
	 */
	static bool Resolve(const FString& Name, FString& OutUrl)
	{
		if (Name.Equals(TEXT("help"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("discord"), ESearchCase::IgnoreCase))
		{
			OutUrl = Discord();
			return true;
		}
		if (Name.Equals(TEXT("bug"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("issues"), ESearchCase::IgnoreCase))
		{
			OutUrl = Issues();
			return true;
		}
		if (Name.Equals(TEXT("star"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("github"), ESearchCase::IgnoreCase))
		{
			OutUrl = Star();
			return true;
		}
		return false;
	}
};
