// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class AActor;
class AWaterBody;
class AWaterZone;
class UClass;
class UMaterialInterface;
class UWorld;

namespace UE::MeshPartition
{
	class AMeshPartition;
	class APreviewSection;
	class UMeshPartitionEditorComponent;
	class UModifierComponent;
	class UWaterModifier;
}

/**
 * The helpers every Mesh Terrain Water operation shares (the operations, the module's members, the specs). Water on a Mesh
 * Terrain (UE 5.8 MeshPartitionWater, Experimental) is a Water plugin body whose actor carries a Mesh Partition water
 * modifier component of its kind (a Lake carries a LakeModifier, a River a RiverModifier) bound to the terrain: the
 * modifier shapes the terrain to the body (bed, shores) from the body's Terrain settings (WaterHeightmapSettings,
 * CurveSettings: ChannelDepth ...), only while the body's bAffectsLandscape is on. An Ocean needs no modifier: its spline
 * only crops the ocean water under the terrain (Epic's Mesh Terrain docs: no setup for oceans). The engine's experimental
 * OceanModifier can override the ground other modifiers shaped under it (seen undoing a lake): the operations never add
 * one and the Problem member reports one.
 *
 * This extension does not depend on the UnrealAIMeshTerrain extension (each builds alone): the few terrain helpers it needs
 * (find, World Partition loading, the build wait) are its own, kept to what the water operations need.
 *
 * Functions that can fail report it with UnrealMcpRql::Fail and return false / nullptr: callers return at once.
 */
namespace UnrealAIMeshTerrainWater
{
	using AMeshPartition = UE::MeshPartition::AMeshPartition;
	using APreviewSection = UE::MeshPartition::APreviewSection;
	using UMeshPartitionEditorComponent = UE::MeshPartition::UMeshPartitionEditorComponent;
	using UModifierComponent = UE::MeshPartition::UModifierComponent;
	using UWaterModifier = UE::MeshPartition::UWaterModifier;

	/** The default time an operation waits for the terrain to build (seconds): below the MCP server's 300 s tool timeout. */
	constexpr double DefaultBuildTimeoutSeconds = 240.0;
	constexpr double MinTimeoutSeconds = 1.0;
	constexpr double MaxTimeoutSeconds = 600.0;
	/** Specs only: when above 0, every build wait gives up after this many seconds (a forced timeout). */
	extern double BuildTimeoutForTests;

	/** Fail ("TimeoutSeconds .. is outside 1 .. 600") and return false unless @p TimeoutSeconds is in that range. */
	bool CheckTimeout(double TimeoutSeconds);

	/** The seconds since @p Start (an FPlatformTime::Seconds()), rounded to 0.01. */
	double SecondsSince(double Start);

	// ---- Guard -------------------------------------------------------------------------------------------------------

	/** Why water on Mesh Terrain cannot be edited in this editor (a plugin disabled, no editor world, PIE running), empty when it can. */
	FString UnavailableReason();

	/** Fail with UnavailableReason() and return false when it is not empty: the first line of every operation. */
	bool CheckAvailable();

	// ---- Terrains ----------------------------------------------------------------------------------------------------

	/** Every Mesh Terrain actor of the editor world (loaded ones). */
	TArray<AMeshPartition*> Terrains();

	/**
	 * Fail and return false when the operations cannot run (CheckAvailable), or @p Terrain is null or not a terrain of the
	 * editor world; else load its World Partition base / modifier actors (LoadTerrainActors).
	 */
	bool CheckTerrain(const AMeshPartition* Terrain);

	/**
	 * Load (in the editor) every actor that carries a base mesh or a modifier of @p Terrain (water bodies with their water
	 * modifier included): on a World Partition map they are spatially loaded and a terrain builds only from loaded ones. No
	 * unsaved change, no undo step. Nothing to do on a map without World Partition.
	 */
	void LoadTerrainActors(const AMeshPartition* Terrain);

	/** Load every water body actor of the editor world (World Partition); nothing to do without World Partition. */
	void LoadWaterBodies();

	/** The editor component that builds @p Terrain (null outside the editor or before the actor registered). */
	UMeshPartitionEditorComponent* EditorComponent(const AMeshPartition* Terrain);

	/** The world box of every loaded Mesh Terrain's built mesh (else its base meshes), invalid when there is none. */
	FBox TerrainsBounds(UWorld* World);

	/**
	 * The ground of @p Terrain's built sections under world @p X, @p Y (a vertical trace against their collision: what traces
	 * and physics hit); unset off the terrain or before it is built.
	 */
	TOptional<double> GroundHeight(const AMeshPartition* Terrain, double X, double Y);

	// ---- Build -------------------------------------------------------------------------------------------------------

	/** Where the asynchronous build of a terrain stands (what UnrealAIMeshTerrain's Build member reports, in short). */
	struct FBuildState
	{
		int32 Sections = 0;        // preview sections
		bool bBuilding = false;    // modifier stack evaluations in flight
		int32 Cooking = 0;         // sections whose rendered mesh has no collision yet
		int32 Finishing = 0;       // sections still showing the intermediate preview mesh (static mesh not made yet)
		int32 CompilingMeshes = 0; // the sections' static meshes still compiling
		bool bNoBase = false;      // the terrain has no base mesh: nothing to build
		bool IsReady() const { return bNoBase || (!bBuilding && Cooking == 0 && Finishing == 0 && CompilingMeshes == 0 && Sections > 0); }
		FString Describe() const;
	};

	/** The build state of @p Terrain now (no waiting). */
	FBuildState BuildState(const AMeshPartition* Terrain);

	/**
	 * Drive @p Terrain's build on the game thread until every section shows its final mesh with its collision, for at most
	 * @p TimeoutSeconds (at least one round): true when built; false with @p OutPending saying what is still pending. Pumps
	 * the editor component, the game thread's task queue and the asset compiler, so the build advances inside one RQL call
	 * (as UnrealAIMeshTerrain's WaitForBuild does).
	 */
	bool WaitForBuild(AMeshPartition* Terrain, double TimeoutSeconds, FString& OutPending);

	// ---- Water -------------------------------------------------------------------------------------------------------

	/** "Lake", "River", "Ocean" or "Custom" (Transition), "Unknown" without a water body component. */
	FString WaterType(const AWaterBody* Body);

	/** The water modifier class a body of @p Body's type carries (LakeModifier, RiverModifier); null for an ocean / custom body. */
	UClass* ModifierClassFor(const AWaterBody* Body);

	/** The Mesh Partition water modifiers (any kind) on @p Body's actor. */
	TArray<UWaterModifier*> WaterModifiers(const AWaterBody* Body);

	/** The modifier of @p Body's own kind (the first), null when it has none. */
	UWaterModifier* MatchingModifier(const AWaterBody* Body);

	/** The terrain the body's matching modifier shapes (null when none). */
	AMeshPartition* TerrainOf(const AWaterBody* Body);

	/** Every water body of the editor world whose matching modifier shapes @p Terrain (loaded ones). */
	TArray<AWaterBody*> BodiesOf(const AMeshPartition* Terrain);

	/** The first actor of class @p Class in @p World (the water zone and the ocean: one per world), or null. */
	AActor* FindFirst(UWorld* World, UClass* Class);

	/** Whether @p Material can render water (its base material has "Used with Water"); read-only. */
	bool IsWaterMaterial(const UMaterialInterface* Material);

	/**
	 * Why @p Body does not render or does not shape its terrain yet, with what to call (empty when it is ready): the
	 * operations' answer and the MeshTerrainWater member.
	 */
	FString Problem(const AWaterBody* Body);
}
