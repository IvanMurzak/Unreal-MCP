// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIMeshTerrainWaterRqlApi.generated.h"

class AWaterBody;
class AWaterZone;
class UMaterialInterface;

namespace UE::MeshPartition
{
	class AMeshPartition;
	class UModifierComponent;
}

/** The result of the `meshTerrainWater` operations: the water body, the terrain it shapes and whether both are ready. */
USTRUCT(BlueprintType)
struct FUnrealAIMeshTerrainWaterResult
{
	GENERATED_BODY()

	/** the water body actor (null after Delete) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	TObjectPtr<AWaterBody> Body = nullptr;

	/** its label (what unreal.find takes) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	FString Label;

	/** Lake, River, Ocean or Custom */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	FString Type;

	/** the label of the Mesh Terrain its modifier shapes ("" for an ocean: it needs no modifier) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	FString Terrain;

	/** its Mesh Partition water modifier (LakeModifier, RiverModifier; null for an ocean): listed by meshTerrainModifiers too */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	TObjectPtr<UE::MeshPartition::UModifierComponent> Modifier = nullptr;

	/** where the water surface is: the body's location (world, cm); a lake's or river's spline points are at this Z or their own */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	FVector Location = FVector::ZeroVector;

	/** true when the terrain is rebuilt around the water and the water renders (Problem is empty) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	bool Ready = false;

	/** how long the call waited for the terrain to build (seconds) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	double Seconds = 0.0;

	/** "built", or what the terrain still builds */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	FString State;

	/** why it is not Ready, with what to call (empty when Ready) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain Water")
	FString Problem;
};

/**
 * The `meshTerrainWater` RQL source (type "MeshTerrainWaterOperations"): Water plugin bodies on a Mesh Terrain (UE 5.8
 * MeshPartitionWater, Experimental), e.g. `meshTerrainWater | call CreateLake(unreal.find("Island"), "Lake", [0, 0, 300], 3000)`.
 * A lake or river is placed the way the Place Actors panel places it (the Water defaults: material, waves) and carries the
 * Mesh Partition water modifier of its kind bound to the terrain, which shapes the terrain to it (its bed, its shores); each
 * call is one undo step and returns when the terrain is rebuilt. BlueprintCallable only so UHT records the C++ default
 * arguments; BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAIMeshTerrainWaterRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** create a round lake on a Mesh Terrain around Center: X, Y its middle, Z its water surface (read the ground with meshTerrain | call Surface; a surface above the ground raises shores around it), Radius in cm (100 .. 200000), Depth cm below the surface (0 keeps the Water default, else 10 .. 20000); its LakeModifier shapes the terrain; Material optional (the Water default); returns {Body, Label, Type, Terrain, Modifier, Location, Ready, Seconds, State, Problem} when the terrain is rebuilt */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainWaterResult CreateLake(UE::MeshPartition::AMeshPartition* Terrain, const FString& Label, FVector Center, double Radius,
		double Depth = 0.0, UMaterialInterface* Material = nullptr, double TimeoutSeconds = 240.0);

	/** create a river on a Mesh Terrain through Points (2 .. 200 world points, upstream first; Z is the water surface: keep it at or a little below the ground and flowing downhill) Width cm wide (100 .. 20000), Depth cm deep (0 keeps the Water default, else 10 .. 20000); its RiverModifier carves its bed into the terrain; Material optional; returns the result when the terrain is rebuilt */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainWaterResult CreateRiver(UE::MeshPartition::AMeshPartition* Terrain, const FString& Label, const TArray<FVector>& Points,
		double Width = 1500.0, double Depth = 0.0, UMaterialInterface* Material = nullptr, double TimeoutSeconds = 240.0);

	/** create the ocean (one per world) with its surface at world Z Height (cm), filling the water zone (made to cover every Mesh Terrain plus 25% when there is none); it needs no modifier: ground above Height hides it, so an island's land must rise above it (meshTerrainModifier | call AddPatch) and the seabed lie below; Material optional; returns the result */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainWaterResult CreateOcean(const FString& Label, double Height = 0.0, UMaterialInterface* Material = nullptr);

	/** make an existing lake or river (placed by hand, with editor | call SpawnActor, by the Landscape extension) shape a Mesh Terrain: gives it the water modifier of its kind bound to Terrain (keeps one, removes extra ones), enables it and turns its Affects Landscape on, then waits for the rebuild; the repair for a body whose MeshTerrainWater.Problem names Attach */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainWaterResult Attach(AWaterBody* Body, UE::MeshPartition::AMeshPartition* Terrain, double TimeoutSeconds = 240.0);

	/** remove a water body's Mesh Partition water modifiers (it stays, the terrain it shaped is rebuilt without it): for an ocean that carries an OceanModifier, or water that should no longer shape the ground */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainWaterResult Detach(AWaterBody* Body, double TimeoutSeconds = 240.0);

	/** delete a water body (one undo step); the terrain it shaped is rebuilt without it (its ground comes back) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainWaterResult Delete(AWaterBody* Body, double TimeoutSeconds = 240.0);

	/** wait (at most TimeoutSeconds, 1..600) until the terrain a water body shapes is rebuilt; read-only; returns the result (Ready, State, Problem) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true", RqlReadOnly))
	FUnrealAIMeshTerrainWaterResult WaitForBuild(AWaterBody* Body, double TimeoutSeconds = 240.0);

	/** create or resize the water zone (the area the water renders in, one per world): SizeMeters 0 covers every Mesh Terrain plus a 25% margin (Center is then ignored), else a square SizeMeters (10 .. 100000) wide around Center (X, Y); refits the ocean; returns the zone */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	AWaterZone* SetZone(double SizeMeters = 0.0, FVector Center = FVector::ZeroVector);

	/** give a water body its water material safely, building its water mesh first (null: the Place Actors defaults of its type, which also repairs a body with no material); the material must be a water material ("Used with Water"); returns the body */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	AWaterBody* SetMaterial(AWaterBody* Body, UMaterialInterface* Material = nullptr);

	/** enable MeshPartitionWater, Water and MeshPartition in the project file (.uproject), so the water modifiers load in packaged games and cooks; returns what changed; a restart is needed only when one was not loaded */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain Water", meta = (BlueprintInternalUseOnly = "true"))
	FString EnableMeshTerrainWater();
};
