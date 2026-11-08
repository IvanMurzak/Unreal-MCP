// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "LandscapeEditTypes.h"

class ALandscapeProxy;
enum ELandscapeLayerUpdateMode : uint32;

/** Landscape edit helpers the landscape and water operations share (UnrealAILandscapeRqlApi.cpp). */
namespace UnrealAILandscapeRqlApiLocal
{
	/**
	 * The edit layer heights or weights (@p Target) are written to: the first one the standard tools can edit (not locked,
	 * not procedural such as splines); an invalid guid after Fail when there is none.
	 */
	FGuid EditLayerGuid(ALandscapeProxy* Landscape, ELandscapeToolTargetType Target);

	/**
	 * Merge the edits now instead of over the next editor frames, then queue @p FollowUp (a full pass: collision, navigation,
	 * foliage) for the next tick. Without rendering (-nullrhi, commandlets) nothing merges: the edit stays in its layer.
	 */
	void MergeNow(ALandscapeProxy* Landscape, ELandscapeLayerUpdateMode FollowUp);
}
