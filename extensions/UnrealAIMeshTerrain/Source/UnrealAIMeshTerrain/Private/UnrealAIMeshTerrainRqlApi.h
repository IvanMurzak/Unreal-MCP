// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIMeshTerrainRqlApi.generated.h"

namespace UE::MeshPartition
{
	class AMeshPartition;
}

/** The result of `meshTerrain | call Surface(terrain, point)`: the built terrain surface under a world X, Y. */
USTRUCT(BlueprintType)
struct FUnrealAIMeshTerrainSurface
{
	GENERATED_BODY()

	/** false when no built terrain is under the point (the other fields are then zero) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	bool Inside = false;

	/** the surface height (world Z, cm): what line traces and collision hit */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double Height = 0.0;

	/** the surface slope in degrees (0 flat, 90 vertical) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double Slope = 0.0;

	/** the point on the surface: [X, Y, Height], a spawn location */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FVector Location = FVector::ZeroVector;

	/** the surface's up-facing unit normal */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FVector Normal = FVector::UpVector;

	/** the paint / weight channels the modifiers wrote at the point (usually 0..1; empty when none writes one) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	TMap<FString, double> Channels;
};

/** The result of Rebuild / WaitForBuild: where a terrain's build stands when the call returns. */
USTRUCT(BlueprintType)
struct FUnrealAIMeshTerrainBuild
{
	GENERATED_BODY()

	/** true when the sections, their collision and their meshes are built: Surface, traces and rendering match the modifiers */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	bool Ready = false;

	/** the built (preview) sections */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	int32 Sections = 0;

	/** the triangles of the built sections */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	int32 Triangles = 0;

	/** the modifiers that shape the terrain (base meshes not counted) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	int32 Modifiers = 0;

	/** how long the call waited for the build (seconds) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double Seconds = 0.0;

	/** "built", or what is still pending */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString State;
};

/**
 * The `meshTerrain` RQL source (type "MeshTerrainOperations"): build and read Mesh Terrains (UE 5.8 Mesh Partition), e.g.
 * `meshTerrain | call Surface(unreal.find("Terrain"), [0, 0, 0])`. Every UFUNCTION here is callable from rql-mutate; the
 * doc comment is the description agents see. BlueprintCallable only so UHT records the C++ default arguments;
 * BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAIMeshTerrainRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** rebuild every section of a Mesh Terrain from its modifiers and wait (at most TimeoutSeconds, 1..600) until its mesh, collision and rendering match them: for a terrain edited outside these operations (Mesh Terrain Mode, Python, set) or that looks stale; returns the build state (Ready, Sections, Triangles, Modifiers, Seconds, State) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainBuild Rebuild(UE::MeshPartition::AMeshPartition* Terrain, double TimeoutSeconds = 240.0);

	/** wait (at most TimeoutSeconds, 1..600) until the Mesh Terrain's pending build is done, without invalidating anything: after `set` on a modifier or terrain; returns the build state */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true", RqlReadOnly))
	FUnrealAIMeshTerrainBuild WaitForBuild(UE::MeshPartition::AMeshPartition* Terrain, double TimeoutSeconds = 240.0);

	/** the built surface of a Mesh Terrain at Point (world X and Y; Z is ignored): Inside, Height (world Z, cm, what traces and collision hit), Slope (degrees), Location ([X, Y, Height], a spawn point), Normal and Channels (the paint weights there); read-only; waits for a pending build first (at most 120 s); a point off the terrain gives Inside false */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true", RqlReadOnly))
	FUnrealAIMeshTerrainSurface Surface(UE::MeshPartition::AMeshPartition* Terrain, FVector Point);

	/** Surface at every point of Points (world X, Y; Z ignored; at most 10000) in one call, in order: a profile along a road, a grid of spawn points; waits for a pending build first */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true", RqlReadOnly))
	TArray<FUnrealAIMeshTerrainSurface> SurfacePoints(UE::MeshPartition::AMeshPartition* Terrain, const TArray<FVector>& Points);

	/** enable the MeshTerrainMode and MeshPartition plugins in the project file (.uproject), so Mesh Terrains load in a packaged game and the editor keeps the mode; returns what changed; a restart is needed only when they were not loaded */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FString EnableMeshTerrain();
};
