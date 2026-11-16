// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAILandscapeRqlApi.generated.h"

class ALandscape;
class ALandscapeProxy;
class UMaterial;
class UMaterialInterface;

/** The result of `landscape | call Surface(landscape, point)`: the ground under a world X, Y. */
USTRUCT(BlueprintType)
struct FUnrealAILandscapeSurface
{
	GENERATED_BODY()

	/** false when the point is outside the landscape (the other fields are then zero) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	bool Inside = false;

	/** the ground height (world Z, cm): what line traces and collision hit */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	double Height = 0.0;

	/** the ground slope in degrees (0 flat, 90 vertical) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	double Slope = 0.0;

	/** the point on the ground: [X, Y, Height], a spawn location */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	FVector Location = FVector::ZeroVector;

	/** the ground's up-facing unit normal */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	FVector Normal = FVector::UpVector;
};

/** One result of `landscape | call Bumps(landscape)`: a place where the ground stands out of its surroundings. */
USTRUCT(BlueprintType)
struct FUnrealAILandscapeBump
{
	GENERATED_BODY()

	/** "Bump" (raised above its surroundings: a spike, mound or ridge) or "Pit" (sunk below them: a hole, crater or ditch) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	FString Kind;

	/** its highest (a bump) or lowest (a pit) point on the ground: [X, Y, Z] */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	FVector Location = FVector::ZeroVector;

	/** how far it stands above (+) or below (-) the ground around it there, cm */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	double Height = 0.0;

	/** the radius (cm) that covers it: Smooth(landscape, Location, Radius * 1.5) blends it away */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Landscape")
	double Radius = 0.0;
};

/**
 * The `landscape` RQL source (type "LandscapeOperations"): create, sculpt and paint landscapes, e.g.
 * `landscape | call Create("Terrain", [0, 0, 0], 100)`. Every UFUNCTION here is callable from rql-mutate (Unreal-MCP's
 * IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see. BlueprintCallable only so UHT records
 * the C++ default arguments; BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAILandscapeRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** create a flat landscape centred on Location, SizeMeters wide on X and Y (8 to 2000 m; about one vertex per metre), with Material (optional: a landscape material, e.g. from CreateLayerMaterial) and room for ground MaxHeightMeters above and below Location's Z (1 to 10000 m; default 256: height steps of 0.78 cm; more room gives coarser steps, ZScale / 128 cm, e.g. 1024 m: 3.1 cm); then shape it with Sculpt, Flatten and Carve and colour it with Paint; its collision is ready when the call returns; returns the landscape actor */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	ALandscape* Create(const FString& Label, FVector Location, double SizeMeters = 100.0, UMaterialInterface* Material = nullptr, double MaxHeightMeters = 256.0);

	/** raise (Height > 0, cm) or lower (Height < 0) a landscape around Center (world X and Y) within Radius (cm) with a smooth falloff to the edge: one call is one hill or hollow; fails without changing anything when the ground would leave the landscape's height range (Bounds.HeightRange; Create's MaxHeightMeters); heights, collision and rendering are updated when the call returns (read them back with Surface); returns how many vertices changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Sculpt(ALandscapeProxy* Landscape, FVector Center, double Radius, double Height);

	/** level a landscape to world Z Height (cm) within Radius (cm) of Center (world X and Y): a flat pad, plateau or basin; Falloff (cm, optional) blends the ground around it back to its old height over that distance (0: a sharp edge); returns how many vertices changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Flatten(ALandscapeProxy* Landscape, FVector Center, double Radius, double Height, double Falloff = 0.0);

	/** dig a channel (a riverbed, ditch or road cut) Depth cm (1 .. 10000) below the ground along Points (2 .. 1000 world points; Z is ignored): Width cm (100 .. 20000) of level bottom following the ground, then banks rising 1:1 to the old ground; one undo step; add water along it with water | call CreateRiver; returns how many vertices changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Carve(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Depth);

	/** paint a layer by name ("Grass") onto a landscape around Center (world X and Y) within Radius (cm; 0 = the whole landscape) with a soft edge; Strength 0..1 (1 = only this layer at the centre); the layer is added to the landscape (its layer info asset saved beside the level) when it has none of that name; it shows when the landscape material blends a layer of that name (CreateLayerMaterial); returns how many vertices changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Paint(ALandscapeProxy* Landscape, const FString& Layer, FVector Center, double Radius = 0.0, double Strength = 1.0);

	/** replace the ground within Radius (cm) of Center (world X and Y) by a smooth surface fitted to the ground around that circle, so a spike, pit, bump or hole disappears into its surroundings (a slope stays a slope); Strength 0..1 (1: all the way); fails without changing anything when the result leaves the height range; returns how many vertices changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Smooth(ALandscapeProxy* Landscape, FVector Center, double Radius, double Strength = 1.0);

	/** level a strip Width cm wide (100 .. 20000) along Points (2 .. 1000 world points) to the points' Z, straight from point to point (raising or lowering the ground): a road bed, ramp, railway or path; Falloff cm (0 .. 20000) blends its sides back to the ground (0: a sharp edge); MaxSlope degrees (optional, 0: any): fails without changing anything when a stretch climbs more steeply, naming it and the length it needs; returns how many vertices changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Ramp(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Falloff = 500.0, double MaxSlope = 0.0);

	/** find where the ground stands out of its surroundings: spikes, pits, mounds and holes at least MinHeight cm (default 200) above or below the mean ground within Scale cm (default 5000) around them; read-only (works in rql-query); returns the largest 50 first, each {Kind (Bump or Pit), Location, Height, Radius}; a smooth hill or valley wider than Scale is not one */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true", RqlReadOnly))
	TArray<FUnrealAILandscapeBump> Bumps(ALandscapeProxy* Landscape, double MinHeight = 200.0, double Scale = 5000.0);

	/** create a landscape material at Path ("/Game/Landscape/M_Terrain") that blends one paint layer per entry of Layers, each a name with an optional colour: "Grass #3a7d2c, Rock #808080"; assign it with Create or set LandscapeMaterial, then Paint the layers; returns the material (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	UMaterial* CreateLayerMaterial(const FString& Path, const FString& Layers);

	/** the ground of a landscape at Point (world X and Y; Z is ignored): Inside, Height (world Z, cm, what traces and collision hit), Slope (degrees), Location ([X, Y, Height], a spawn point on the ground) and Normal; read-only; a point outside the landscape gives Inside false */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true", RqlReadOnly))
	FUnrealAILandscapeSurface Surface(ALandscapeProxy* Landscape, FVector Point);

	/** delete a whole landscape: its landscape actor and every streaming proxy of a World Partition map, loaded or not (Destroy on the landscape actor alone leaves the proxies behind); one undo step; returns how many actors were deleted */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Delete(ALandscapeProxy* Landscape);

	/** merge every edit layer of a landscape again and rebuild its collision and, when it has Nanite enabled, its Nanite mesh: for a landscape edited outside these operations (Landscape mode, Python) whose collision or Nanite looks stale; returns how many landscape components it has */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Rebuild(ALandscapeProxy* Landscape);
};
