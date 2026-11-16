// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "LandscapeEditTypes.h"

class ALandscapeProxy;
struct FUnrealAILandscapeEdit;
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
	 * False after Fail when the regions could not be loaded (an edit must then not run on part of the ground).
	 */
	bool LoadLandscape(ALandscapeProxy* Landscape, const FBox2D* Area = nullptr);

	/**
	 * Load every World Partition actor of @p Class (and its subclasses) in the editor world that is not loaded yet, as the
	 * editor's region loading does: the sources (landscapes, waterBodies, waterZones) then list the whole map, not only the
	 * region a reopened map loads around the camera. Loading changes and dirties nothing. Nothing to do without World Partition.
	 */
	void LoadAll(UClass* Class);

	/** How many records a landscape's edit log keeps (the oldest go first). */
	constexpr int32 MaxEdits = 1000;

	/**
	 * Append a record to the edit log of @p Landscape's landscape actor (creating the editor-only log component the first
	 * time), inside the current operation's undo step; the oldest records go beyond MaxEdits.
	 */
	void RecordEdit(ALandscapeProxy* Landscape, const TCHAR* Op, const FString& Label, const TArray<FVector>& Points, double Radius,
		double Width, double Height, const FString& Layer = FString());

	/** The edit log of @p Landscape's landscape actor, oldest first (empty when it has none). */
	const TArray<FUnrealAILandscapeEdit>& EditLog(const ALandscapeProxy* Landscape);

	/** Carve: a channel Depth cm below the ground along Points (world), Width cm of bottom then 1:1 banks; changed vertices. */
	int32 CarveAlong(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Depth);

	/**
	 * RoadBetween's route: a path from Start to End (world) whose ground climbs no more than MaxSlope (with a margin), found by
	 * an A* search on a ~10 m grid, then resampled every few metres with each point's Z the ground limited to MaxSlope from
	 * the start. False after Fail; bReached false when the search could not get to End (the path then ends nearest to it).
	 */
	bool PlanRoad(ALandscapeProxy* Landscape, const FVector& Start, const FVector& End, double MaxSlope, TArray<FVector>& OutPoints, bool& bReached);

	/**
	 * Ramp: the ground along Points (world) set to their Z, straight from point to point, Width cm wide, blended back to the
	 * ground over Falloff cm (a cosine) on both sides; raises or lowers; changed vertices (0 after Fail).
	 */
	int32 RampAlong(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Falloff);
}
