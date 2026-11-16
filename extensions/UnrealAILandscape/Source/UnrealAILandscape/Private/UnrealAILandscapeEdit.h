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

	/** The lowest and highest world Z (cm) the landscape's heightmap can hold (its 16-bit range scaled by the actor's Z scale). */
	FVector2D HeightRange(const ALandscapeProxy* Landscape);

	/**
	 * Whether ground from @p LowCm to @p HighCm (world Z) fits the landscape's HeightRange; else false after Fail naming the
	 * range, the request and the fix (Create with a larger MaxHeightMeters). Never clamp silently.
	 */
	bool FitsHeightRange(const ALandscapeProxy* Landscape, double LowCm, double HighCm);

	/**
	 * Load the World Partition streaming proxies of the landscape that overlap @p Area (world X, Y; null: all of them), as
	 * the editor's region loading does, so an edit or read sees all of the ground and not only the region loaded around the
	 * camera after a map opens. Nothing to do on a map without World Partition. Loading changes and dirties nothing.
	 */
	void LoadLandscape(ALandscapeProxy* Landscape, const FBox2D* Area = nullptr);

	/** Carve: a channel Depth cm below the ground along Points (world), Width cm of bottom then 1:1 banks; changed vertices. */
	int32 CarveAlong(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Depth);
}
