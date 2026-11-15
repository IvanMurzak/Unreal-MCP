// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIMeshTerrainCreateApi.generated.h"

namespace UE::MeshPartition
{
	class AMeshPartition;
	class UMeshPartitionDefinition;
}

/**
 * The `meshTerrainCreate` RQL source (type "MeshTerrainCreateOperations"): make and delete Mesh Terrains (UE 5.8 Mesh
 * Partition) the way Mesh Terrain Mode's Create palette does, e.g. `meshTerrainCreate | call Create("Island", [0, 0, 0], 500)`.
 * Every UFUNCTION here is callable from rql-mutate; the doc comment is the description agents see. BlueprintCallable only
 * so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAIMeshTerrainCreateApi : public UObject
{
	GENERATED_BODY()

public:
	/** create a flat Mesh Terrain centred on Location (world cm, within 200 km of the origin; its ground is at Location Z), SizeMeters wide on X (10 .. 20000) and SizeYMeters on Y (0 = square), with Resolution quads along X (0 = about one per metre; at most 2000 per side, square quads; split into sections of at most 500 x 500 quads) and Definition (optional: a MeshPartitionDefinition asset, its material, priority layers and paint channels; default /MeshPartition/DataAssets/MPD_Default); one undo step; built (mesh, collision, rendering) when the call returns (waits at most TimeoutSeconds, 1..600); returns the terrain actor: shape it with modifiers, read it with meshTerrain | call Surface */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	UE::MeshPartition::AMeshPartition* Create(const FString& Label, FVector Location, double SizeMeters = 200.0, double SizeYMeters = 0.0,
		int32 Resolution = 0, UE::MeshPartition::UMeshPartitionDefinition* Definition = nullptr, double TimeoutSeconds = 120.0);

	/** create a Mesh Terrain from a 16-bit grayscale heightmap File (absolute path or relative to the project: .png, or .r16 / .raw = little-endian 16-bit square, as Landscape exports), centred on Location, SizeMeters wide on X (10 .. 20000; Y follows the image aspect; image row 0 is the -Y edge), heights in Landscape units: value 32768 is Location Z and every step is HeightScale / 128 cm (HeightScale = a Landscape's Z scale, 0.01 .. 10000, default 100: -256 .. +256 m); Resolution = quads along X (0 = one per heightmap pixel step, at most 2048; split into sections of at most 512 x 512 quads); Definition optional; one undo step; built when the call returns (at most TimeoutSeconds, 1..600); returns the terrain actor */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	UE::MeshPartition::AMeshPartition* ImportHeightmap(const FString& Label, const FString& File, FVector Location, double SizeMeters = 1000.0,
		double HeightScale = 100.0, int32 Resolution = 0, UE::MeshPartition::UMeshPartitionDefinition* Definition = nullptr, double TimeoutSeconds = 120.0);

	/** delete a Mesh Terrain with its base meshes, the modifiers that shape it and its built sections (World Partition actors are loaded first), as one undo step; returns what was deleted */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FString Delete(UE::MeshPartition::AMeshPartition* Terrain);
};
