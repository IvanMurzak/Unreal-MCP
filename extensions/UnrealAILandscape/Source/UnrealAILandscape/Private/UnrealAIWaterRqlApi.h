// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIWaterRqlApi.generated.h"

class ALandscapeProxy;
class AWaterBody;
class AWaterBodyLake;
class AWaterBodyOcean;
class AWaterBodyRiver;
class AWaterZone;
class UMaterialInterface;

/**
 * The `water` RQL source (type "WaterOperations"): oceans, lakes and rivers of the Water plugin, e.g.
 * `water | call CreateOcean("Ocean")`. Every body is placed the way the editor's Place Actors panel places it (the Water
 * defaults: material, waves, spline), never edits the landscape (no water brush, so no "Insert New Landscape Edit Layer"
 * dialog and no GPU brush pass), and has its water mesh built before the call returns. Each call is bounded work.
 * BlueprintCallable only so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAIWaterRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** make sure the Water plugin (and the plugins it needs) is enabled in the project file, so packaged games and cooks include the water actors, and that the WaterBodyCollision profile exists; no restart needed (this extension already loads Water); returns what it did */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Water", meta = (BlueprintInternalUseOnly = "true"))
	FString EnableWater();

	/** create or resize the water zone (the area the water mesh renders, one per world): SizeMeters 0 covers every landscape plus a 25% margin (Center is then ignored), else a square SizeMeters wide around Center (X, Y); returns the zone */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Water", meta = (BlueprintInternalUseOnly = "true"))
	AWaterZone* SetZone(double SizeMeters = 0.0, FVector Center = FVector::ZeroVector);

	/** create an ocean whose surface is at world Z Height (cm) filling the water zone (made to cover the landscapes when there is none); land above Height hides it, so one ocean surrounds an island; Material optional (the Water default); returns the ocean */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Water", meta = (BlueprintInternalUseOnly = "true"))
	AWaterBodyOcean* CreateOcean(const FString& Label, double Height = 0.0, UMaterialInterface* Material = nullptr);

	/** create a round lake around Center: X, Y its middle, Z its water surface (read the ground with landscape | call Surface), Radius in cm (100 .. 200000); Material optional; returns the lake */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Water", meta = (BlueprintInternalUseOnly = "true"))
	AWaterBodyLake* CreateLake(const FString& Label, FVector Center, double Radius, UMaterialInterface* Material = nullptr);

	/** create a river through Points (2 .. 200 world points, upstream first; Z is the water surface, so keep it at or below the ground of its bed) Width cm wide (100 .. 20000); Material optional; returns the river */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Water", meta = (BlueprintInternalUseOnly = "true"))
	AWaterBodyRiver* CreateRiver(const FString& Label, const TArray<FVector>& Points, double Width = 1500.0, UMaterialInterface* Material = nullptr);

	/** create a river on Landscape from Source to Mouth (world X, Y) along the lowest ground between them, flowing downhill all the way: Depth > 0 (cm) also carves its bed that deep into the landscape (one undo step with the river), so the water shows even where the path crosses higher ground; Width cm (100 .. 20000); returns the river (read its points with waterBodies | select Water) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Water", meta = (BlueprintInternalUseOnly = "true"))
	AWaterBodyRiver* CreateRiverBetween(const FString& Label, ALandscapeProxy* Landscape, FVector Source, FVector Mouth, double Width = 1500.0, double Depth = 300.0, UMaterialInterface* Material = nullptr);

	/** give a water body its water material safely, building its water mesh first (null: the Place Actors defaults of its type, which also repairs a body with no material); the material must be a water material (Material Domain Surface with "Used with Water"); returns the body */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Water", meta = (BlueprintInternalUseOnly = "true"))
	AWaterBody* SetMaterial(AWaterBody* Body, UMaterialInterface* Material = nullptr);
};
