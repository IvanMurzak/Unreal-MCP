// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "ScopedTransaction.h"
#include "MeshPartitionMeshData.h" // FMeshABBTree3

class UClass;
class UWorld;

namespace UE::MeshPartition
{
	class AMeshPartition;
	class APreviewSection;
	class UMeshPartitionDefinition;
	class UMeshPartitionEditorComponent;
	class UModifierComponent;
}

/**
 * The helpers every Mesh Terrain operation shares (UnrealAIMeshTerrain*Api.cpp, the module's members, the specs). A Mesh
 * Terrain is a UE::MeshPartition::AMeshPartition actor ("MeshPartition" to RQL and Python); its shape is the stack of
 * modifier components (UModifierComponent) that target it, built asynchronously into preview sections (APreviewSection,
 * editor-only actors with the rendered mesh and its collision).
 *
 * Functions that can fail report it with UnrealMcpRql::Fail (the RQL call fails with that message) and return false /
 * nullptr: callers return at once.
 */
namespace UnrealAIMeshTerrain
{
	using AMeshPartition = UE::MeshPartition::AMeshPartition;
	using APreviewSection = UE::MeshPartition::APreviewSection;
	using UMeshPartitionDefinition = UE::MeshPartition::UMeshPartitionDefinition;
	using UMeshPartitionEditorComponent = UE::MeshPartition::UMeshPartitionEditorComponent;
	using UModifierComponent = UE::MeshPartition::UModifierComponent;

	/** The default time an operation waits for the terrain to build before it fails (seconds). */
	constexpr double DefaultBuildTimeoutSeconds = 120.0;
	/** The TimeoutSeconds range the operations accept. */
	constexpr double MinTimeoutSeconds = 1.0;
	constexpr double MaxTimeoutSeconds = 600.0;

	/** Fail ("TimeoutSeconds .. is outside 1 .. 600") and return false unless @p TimeoutSeconds is in that range. */
	bool CheckTimeout(double TimeoutSeconds);

	/** The seconds since @p Start (an FPlatformTime::Seconds()), rounded to 0.01: the operations' Seconds fields. */
	double SecondsSince(double Start);

	// ---- Guard -------------------------------------------------------------------------------------------------------

	/** Why Mesh Terrain cannot be edited in this editor (plugin disabled, no editor world, PIE running), empty when it can. */
	FString UnavailableReason();

	/** Fail with UnavailableReason() and return false when Mesh Terrain cannot be edited: the first line of every operation. */
	bool CheckAvailable();

	// ---- Find --------------------------------------------------------------------------------------------------------

	/** Every Mesh Terrain actor of the editor world (loaded ones). */
	TArray<AMeshPartition*> Terrains();

	/** The Mesh Terrain labelled (or named) @p LabelOrName; nullptr after Fail listing the terrains there are. */
	AMeshPartition* FindTerrain(const FString& LabelOrName);

	/**
	 * Fail and return false when Mesh Terrain cannot be edited (CheckAvailable), or @p Terrain is null (with the "pass unreal.find(...)" hint) or not in the editor world; else
	 * load its base / modifier actors (LoadTerrainActors) so the operation sees the whole terrain.
	 */
	bool CheckTerrain(const AMeshPartition* Terrain);

	/**
	 * Load (in the editor) every actor that carries a base mesh or a modifier of @p Terrain: on a World Partition map they are
	 * spatially loaded actors, unloaded until a region over them is loaded, and a terrain builds only from loaded ones. They
	 * stay loaded like actors of a loaded region (UnrealMcpRql::LoadWorldPartitionActors: no unsaved change, no undo step;
	 * actors deleted in this session stay deleted). Nothing to do on a map without World Partition.
	 */
	void LoadTerrainActors(const AMeshPartition* Terrain);

	/** The editor component that builds @p Terrain (null outside the editor or before the actor registered). */
	UMeshPartitionEditorComponent* EditorComponent(const AMeshPartition* Terrain);

	/** The modifiers that shape @p Terrain, in build order (priority layer, then sub-priority); @p bBases adds the base meshes. */
	TArray<UModifierComponent*> Modifiers(const AMeshPartition* Terrain, bool bBases = false);

	/** The paint channel names of @p Terrain's definition, joined for messages ("none" when it has none). */
	FString ChannelList(const AMeshPartition* Terrain);

	/** @p Base, or @p Base2, @p Base3, ...: the first actor label @p World does not use yet. */
	FString UniqueLabel(UWorld* World, const FString& Base);

	/**
	 * Place a modifier of @p Class the way the Modifiers palette does (UModifierActorFactory: an AModifierActor named after
	 * the class, with a billboard) at @p Location, labelled @p Label when set, then bind it to @p Terrain: the factory binds
	 * the nearest terrain by its own rule, not always the one under it. @p OutPrevious is the terrain it bound first (to
	 * rebuild it too). nullptr after Fail (a half-made actor is deleted).
	 */
	UModifierComponent* SpawnModifier(AMeshPartition* Terrain, UClass* Class, const FVector& Location, const FRotator& Rotation, const FString& Label,
		AMeshPartition*& OutPrevious);

	/** The terrain @p Modifier shapes (null when it targets none). */
	AMeshPartition* TerrainOf(const UModifierComponent* Modifier);

	/** The modifier "kind" agents use: the class name without the "Modifier" suffix (Noise, Spline, Boolean, MeshProvider...). */
	FString ModifierKind(const UModifierComponent* Modifier);

	/** The preview sections (the built, rendered mesh) of @p Terrain. */
	TArray<APreviewSection*> PreviewSections(const AMeshPartition* Terrain);

	/** The triangles @p Section renders now: its intermediate preview mesh, or its final static mesh(es) once compiled. */
	int32 RenderedTriangles(const APreviewSection* Section);

	/**
	 * The world box of @p Terrain's built mesh (the preview sections), invalid when nothing is built. Larger than BaseBounds
	 * where a Boolean Union shape reaches past the terrain's edge.
	 */
	FBox BuiltBounds(const AMeshPartition* Terrain);

	/** The world box of @p Terrain's base meshes (the terrain as Create / ImportHeightmap made it), invalid without one. */
	FBox BaseBounds(const AMeshPartition* Terrain);

	/**
	 * Where @p Terrain's built ground is highest and lowest within its own area @p Base (BaseBounds): a vertex of the rendered sections'
	 * collision that a vertical trace from above lands on (what traces and physics hit; not the underside of an overhang).
	 * Each is unset when nothing is built (or no section has cooked collision yet) or no candidate vertex is reached.
	 */
	void GroundExtremes(const AMeshPartition* Terrain, const FBox& Base, TOptional<FVector>& OutHighest, TOptional<FVector>& OutLowest);

	// ---- Build -------------------------------------------------------------------------------------------------------

	/**
	 * Where the asynchronous build of a terrain stands. A section is built in three steps: the modifier stack is evaluated on
	 * worker threads (bBuilding), the result shows at once on the section's preview mesh component (full resolution, with
	 * collision once cooked: Cooking), then the definition's preview transformer pipeline turns it into a (Nanite) static mesh
	 * that replaces it, with that mesh's collision (Finishing until it exists, CompilingMeshes while it compiles).
	 */
	struct FBuildState
	{
		int32 Sections = 0;            // preview sections
		int32 Triangles = 0;           // rendered by the sections now
		bool bBuilding = false;        // modifier stack evaluations in flight (this terrain)
		int32 Cooking = 0;             // sections whose rendered mesh has no collision yet
		int32 Finishing = 0;           // sections still showing the intermediate preview mesh (static mesh not made yet)
		int32 CompilingMeshes = 0;     // the sections' static meshes still compiling
		int32 Unregistered = 0;        // loaded modifiers of the terrain its editor component does not list yet (just loaded / spawned)
		int32 Actors = 0;              // the terrain's base / modifier actors on disk (World Partition actor descriptors)
		int32 LoadedActors = 0;        // ... and how many of them are loaded
		bool bNoBase = false;          // the terrain has no base mesh: nothing to build
		bool IsReady() const { return Unregistered == 0 && (bNoBase || (!bBuilding && Cooking == 0 && Finishing == 0 && CompilingMeshes == 0 && Sections > 0)); }
		FString Describe() const;
	};

	/** The build state of @p Terrain now (no waiting). */
	FBuildState BuildState(const AMeshPartition* Terrain);

	/**
	 * Drive @p Terrain's build on this (game) thread until every section shows its final mesh with its collision, for at
	 * most @p TimeoutSeconds: true when built; false after Fail naming what is still pending. Pumps the editor component
	 * (UMeshPartitionEditorComponent::Update: queued modifier changes start, finished builds are applied), the game thread's
	 * task queue (collision cooking and the transformer pipeline complete there) and the asset compiler (the static
	 * meshes), so the build advances inside one RQL call without editor frames. Safe because Unreal-MCP runs calls at the end
	 * of a frame, outside the task graph's game-thread processing (UnrealMcpGameThreadDispatcher).
	 */
	bool WaitForBuild(AMeshPartition* Terrain, double TimeoutSeconds = DefaultBuildTimeoutSeconds);

	/** Invalidate and rebuild every section of @p Terrain (ForceRebuildAllSections), then WaitForBuild. */
	bool Rebuild(AMeshPartition* Terrain, double TimeoutSeconds = DefaultBuildTimeoutSeconds);

	// ---- Edit --------------------------------------------------------------------------------------------------------

	/**
	 * One undo step for a terrain edit: opens a transaction named @p Description and marks @p Objects (actors, components,
	 * the definition) modified, so their packages are dirty and Ctrl+Z restores them. Keep it on the stack for the edit.
	 */
	class FTerrainTransaction
	{
	public:
		explicit FTerrainTransaction(const FText& Description, TArrayView<UObject* const> Objects = {});
		void Modify(UObject* Object);

	private:
		FScopedTransaction Transaction;
	};

	// ---- Surface -----------------------------------------------------------------------------------------------------

	/** The built terrain surface under a world X, Y. */
	struct FSurfaceSample
	{
		bool bInside = false;        // false: no built terrain under the point (the other fields are zero)
		double Height = 0.0;         // world Z (cm) of the rendered sections' collision: what traces and physics hit
		FVector Normal = FVector::UpVector;
		double ModelHeight = 0.0;    // with bChannels: world Z of the full-resolution modifier stack (the rendered mesh is simplified)
		TMap<FName, float> Channels; // with bChannels: the paint / weight channels the modifiers wrote (usually 0..1)
	};

	/** One evaluated piece of a terrain: a base-mesh group with every modifier applied, its AABB tree and its transform. */
	struct FRegionMesh
	{
		TSharedPtr<const UE::MeshPartition::FMeshData> Mesh;
		TSharedPtr<const UE::MeshPartition::FMeshABBTree3> Spatial;
		FTransform Transform;
	};

	/**
	 * The highest point of @p Terrain's built surface under @p X, @p Y: a trace against the rendered sections' collision.
	 * With @p bChannels it also evaluates the modifier stack there for the paint channels and ModelHeight: from
	 * @p Evaluated (BuildRegions evaluated for earlier points, for many points) or regions around the point, widened until
	 * one holds a whole triangle under it (Boolean shapes have triangles of 100+ m); the regions it evaluates are added to
	 * @p Evaluated. With bChannels, Normal is the evaluated face's (a trace on a collision edge, such as a section seam,
	 * reports the edge's normal). Read-only; call WaitForBuild first after an edit.
	 */
	FSurfaceSample SampleSurface(const AMeshPartition* Terrain, double X, double Y, bool bChannels = true, TArray<FRegionMesh>* Evaluated = nullptr);

	/**
	 * Evaluate @p Terrain's modifier stack (its bases and every modifier, in build order) for the base-mesh groups that
	 * intersect the world box @p Region, synchronously (UE::MeshPartition::Build::LaunchBuilds, BuildType Request, cached):
	 * the full-resolution mesh with its weight channels, independent of the preview sections. Empty on a timeout (after Fail
	 * with @p bFailOnTimeout).
	 */
	TArray<FRegionMesh> BuildRegion(const AMeshPartition* Terrain, const FBox& Region, double TimeoutSeconds = DefaultBuildTimeoutSeconds, bool bFailOnTimeout = true);

	// ---- Create (shared with the create operations and the specs) ------------------------------------------------------

	/**
	 * A flat rectangular Mesh Terrain the way Mesh Terrain Mode's Create tool makes one (FMegaMeshRectangleGenerator): an
	 * AMeshPartition with the project's default definition (MeshPartition Settings: /MeshPartition/DataAssets/MPD_Default)
	 * and one MeshProvider base modifier actor ("Section_X0-Y0_0", attached to it) per section of at most 500 x 500 quads,
	 * centred on @p Center, @p SizeX x @p SizeY cm with @p QuadsX x @p QuadsY quads, labelled @p Label. Called inside a transaction
	 * (meshTerrainCreate Create does) it is one undo step with its base meshes. Not built yet: WaitForBuild. nullptr after Fail.
	 */
	AMeshPartition* SpawnFlatTerrain(const FString& Label, const FVector& Center, double SizeX, double SizeY, int32 QuadsX, int32 QuadsY);

	/**
	 * Delete @p Terrain with its loaded base meshes and modifiers (one undo step): their actors when they belong to it (base
	 * actors, modifier actors whose modifiers all shape it), else only the modifier component.
	 */
	void DestroyTerrain(AMeshPartition* Terrain);
}
