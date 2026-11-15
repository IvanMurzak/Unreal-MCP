// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIMeshTerrainModifiersApi.generated.h"

class UTexture2D;

namespace UE::MeshPartition
{
	class AMeshPartition;
	class UModifierComponent;
}

namespace UnrealAIMeshTerrain
{
	/** A lattice modifier's control points in world space, in its own order (empty for any other modifier): its LatticePoints member. */
	TArray<FVector> LatticeWorldPoints(const UE::MeshPartition::UModifierComponent* Modifier);
}

/** The result of every `meshTerrainModifier` operation: the modifier it added or changed and the terrain's build when it returned. */
USTRUCT(BlueprintType)
struct FUnrealAIMeshTerrainModifierResult
{
	GENERATED_BODY()

	/** the modifier component (null after Remove) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	TObjectPtr<UE::MeshPartition::UModifierComponent> Modifier = nullptr;

	/** the label of the modifier's actor */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString Actor;

	/** Noise, Patch, Spline, Boolean, ... */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString Kind;

	/** the label of the terrain it shapes */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString Terrain;

	/** its build position: the definition's priority layer ("None": after every listed layer) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString PriorityLayer;

	/** its sub-priority within the layer (higher is applied later, on top) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double Priority = 0.0;

	/** true when it is switched off */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	bool Disabled = false;

	/** true when the terrain's mesh, collision and rendering match its modifiers (Surface reads the result) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	bool Ready = false;

	/** how long the call waited for the terrain to build (seconds) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double Seconds = 0.0;

	/** "built", or what is still pending */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString State;
};

/**
 * The `meshTerrainModifier` RQL source (type "MeshTerrainModifierOperations"): add, change, reorder, switch and remove the
 * modifiers that shape a Mesh Terrain (UE 5.8 Mesh Partition), e.g.
 * `meshTerrainModifier | call AddNoise(unreal.find("Terrain"), [0, 0, 0], [8000, 8000], 600)`. Every operation is one undo
 * step, binds the modifier to the terrain it names and returns after the terrain is rebuilt (WaitForBuild). The doc
 * comment is the description agents see. BlueprintCallable only so UHT records the C++ default arguments;
 * BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAIMeshTerrainModifiersApi : public UObject
{
	GENERATED_BODY()

public:
	/** add a noise modifier: rolling hills over a Size [X, Y] cm area centred on Location, Strength cm high (Intensity), Frequency in 1/cm (0.001: features about 10 m apart, smaller is broader), Falloff 0..1 (fade at the edges); Label names its actor (optional); returns the modifier after the terrain is rebuilt */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult AddNoise(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector2D Size = FVector2D(2000.0, 2000.0), double Strength = 500.0, double Frequency = 0.001, double Falloff = 0.5, const FString& Label = TEXT(""));

	/** add a patch modifier: a flat round pad at Location's height (Z: raise or lower the ground to it) Radius cm wide, blending into the ground over Falloff cm around it (a building site, a plateau, a pit floor) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult AddPatch(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, double Radius = 1000.0, double Falloff = 1000.0, const FString& Label = TEXT(""));

	/** add a weight (paint) modifier: write Value (0..1) into the paint channel Channel (one of the terrain's Channels) in a disc of Radius cm around Location, fading to 0 over Falloff cm; the ground's shape is not changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult AddWeight(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, FVector Location, double Radius = 1000.0, double Falloff = 1000.0, double Value = 1.0, const FString& Label = TEXT(""));

	/** add a spline modifier: the ground follows a path through Points (2 .. 1000 world points; their Z is the ground height there) Width cm wide, blending into the ground over Falloff cm on each side (a road, a river bed, a ridge); Closed joins the last point to the first and fills the inside (a plateau outline); Channel (optional) also paints that channel along it */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult AddSpline(UE::MeshPartition::AMeshPartition* Terrain, const TArray<FVector>& Points, double Width = 400.0, double Falloff = 500.0, bool Closed = false, const FString& Channel = TEXT(""), const FString& Label = TEXT(""));

	/** add a shape (Mesh Terrain Mode's Shapes): Shape is Box, Sphere, Cylinder, Capsule, Cone, Torus, Rectangle or Disc, Size [X, Y, Z] cm centred on Location, turned by Rotation [Pitch, Yaw, Roll]; Operation Union (add it to the ground: a mesa, a rock), Subtract (cut it out: a crater, a cave mouth), Trim (keep the ground only inside it) or Project (press the ground onto the shape's top: a flat pad; the default for the flat Rectangle and Disc) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult AddShape(UE::MeshPartition::AMeshPartition* Terrain, const FString& Shape, FVector Location, FVector Size = FVector(1000.0, 1000.0, 1000.0), const FString& Operation = TEXT(""), FRotator Rotation = FRotator::ZeroRotator, const FString& Label = TEXT(""));

	/** add a lattice modifier: a Size [X, Y, Z] cm box of Resolution^3 control points (2 .. 9) centred on Location; it deforms the ground once its points move: read them with the modifier's LatticePoints member and move them with SetLatticePoints */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult AddLattice(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector Size = FVector(2000.0, 2000.0, 2000.0), int32 Resolution = 5, const FString& Label = TEXT(""));

	/** move a lattice modifier's control points to Points (world positions, as many and in the order its LatticePoints member lists them): the ground inside the lattice follows them */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult SetLatticePoints(UE::MeshPartition::UModifierComponent* Modifier, const TArray<FVector>& Points);

	/** add a texture patch: a heightmap texture (Texture, an asset path; mid grey = no change) stamped over a Size [X, Y] cm area centred on Location, Height cm from black to white */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult AddTexturePatch(UE::MeshPartition::AMeshPartition* Terrain, UTexture2D* Texture, FVector Location, FVector2D Size = FVector2D(2000.0, 2000.0), double Height = 1000.0, const FString& Label = TEXT(""));

	/** add a remesh modifier: rebuild the triangles of a Size [X, Y, Z] cm box centred on Location with edges about EdgeLength cm long (finer detail for later modifiers, or coarser ground) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult AddRemesh(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector Size = FVector(2000.0, 2000.0, 50000.0), double EdgeLength = 200.0, const FString& Label = TEXT(""));

	/** add a modifier of any other scriptable Kind with its defaults at Location (MeshProject, InstancedPatch, SplineRemesh, ProjectMeshLayers, or one of the kinds above), then shape it with set on its properties (rql-schema <Kind>Modifier) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult Add(UE::MeshPartition::AMeshPartition* Terrain, const FString& Kind, FVector Location, const FString& Label = TEXT(""));

	/** change a modifier's common parameters (a negative value keeps the current one): Size [X, Y, Z] cm (Noise, Shape, Lattice, TexturePatch, Remesh; Z ignored by the flat ones), Radius cm (Patch, Weight), Width cm (Spline), Strength (Noise height cm, Weight value 0..1, TexturePatch height cm), Falloff (Noise 0..1; Patch, Weight, Spline cm); other properties: set */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult Edit(UE::MeshPartition::UModifierComponent* Modifier, FVector Size = FVector(-1.0, -1.0, -1.0), double Radius = -1.0, double Width = -1.0, double Strength = -1.0, double Falloff = -1.0);

	/** move a modifier's actor to Location (world, cm; its rotation is kept): a patch or shape also takes its height from Z */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult Move(UE::MeshPartition::UModifierComponent* Modifier, FVector Location);

	/** replace a spline modifier's path with Points (2 .. 1000 world points; Z is the ground height), Closed joining the last to the first */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult SetSplinePoints(UE::MeshPartition::UModifierComponent* Modifier, const TArray<FVector>& Points, bool Closed = false);

	/** change where a modifier is applied in the build order: Priority (sub-priority: higher is applied later, on top of lower ones) within PriorityLayer (one of the terrain's PriorityLayers; empty keeps the current layer, "None" puts it after every listed layer) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult SetOrder(UE::MeshPartition::UModifierComponent* Modifier, double Priority, const FString& PriorityLayer = TEXT(""));

	/** switch a modifier on again (its Disabled flag off) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult Enable(UE::MeshPartition::UModifierComponent* Modifier);

	/** switch a modifier off without removing it (its Disabled flag on): the terrain is built without it */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult Disable(UE::MeshPartition::UModifierComponent* Modifier);

	/** make a modifier shape Terrain instead of the terrain it shapes now (a modifier placed by hand can be bound to the wrong one) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult Bind(UE::MeshPartition::UModifierComponent* Modifier, UE::MeshPartition::AMeshPartition* Terrain);

	/** delete a modifier (its actor, when the actor holds nothing else) and rebuild the terrain without it; base meshes cannot be removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainModifierResult Remove(UE::MeshPartition::UModifierComponent* Modifier);
};
