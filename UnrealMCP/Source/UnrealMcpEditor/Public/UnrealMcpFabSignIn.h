// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

/** Epic's sign-in service in this editor: the EOS platform the engine's Fab plugin signs in on. */
enum class EUnrealMcpFabSignInService : uint8
{
	/** Not known (the UnrealMcpFab module is not loaded, or it cannot tell which EOS platform is Fab's). */
	Unknown,
	/** It never started (no EOS SDK, e.g. an editor run with -NoEOS): no sign-in can finish in this editor. */
	NotStarted,
	/** It runs and is restoring the sign-in Epic remembers on this computer. */
	Restoring,
	/** It runs. */
	Running,
};

/**
 * What the Fab tools know of the user's Epic sign-in beyond the Fab plugin's own UFUNCTIONs. Filled by the optional
 * UnrealMcpFab module (loaded only when the Fab plugin is enabled; it links EOSShared, which this module never does) and
 * reset when it unloads, so every member may be unset.
 */
struct FUnrealMcpFabSignIn
{
	TFunction<EUnrealMcpFabSignInService()> Service;
	/** The signed-in user's tokens on Fab's EOS platform; false when nobody is signed in there. */
	TFunction<bool(FString& OutAccessToken, FString& OutRefreshToken)> Tokens;
};

UNREALMCPEDITOR_API FUnrealMcpFabSignIn& UnrealMcpFabSignIn();
