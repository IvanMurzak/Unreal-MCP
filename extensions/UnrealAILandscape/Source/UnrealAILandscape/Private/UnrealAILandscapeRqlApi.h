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
	/** create a flat landscape centred on Location, SizeMeters wide on X and Y (8 to 2000 m; about one vertex per metre), with Material (optional: a landscape material, e.g. from CreateLayerMaterial); then shape it with Sculpt and colour it with Paint; returns the landscape actor */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	ALandscape* Create(const FString& Label, FVector Location, double SizeMeters = 100.0, UMaterialInterface* Material = nullptr);

	/** raise (Height > 0, cm) or lower (Height < 0) a landscape around Center (world X and Y) within Radius (cm) with a smooth falloff to the edge: one call is one hill or hollow; returns how many vertices changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Sculpt(ALandscapeProxy* Landscape, FVector Center, double Radius, double Height);

	/** paint a layer by name ("Grass") onto a landscape around Center (world X and Y) within Radius (cm; 0 = the whole landscape) with a soft edge; Strength 0..1 (1 = only this layer at the centre); the layer is added to the landscape (its layer info asset saved beside the level) when it has none of that name; it shows when the landscape material blends a layer of that name (CreateLayerMaterial); returns how many vertices changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	int32 Paint(ALandscapeProxy* Landscape, const FString& Layer, FVector Center, double Radius = 0.0, double Strength = 1.0);

	/** create a landscape material at Path ("/Game/Landscape/M_Terrain") that blends one paint layer per entry of Layers, each a name with an optional colour: "Grass #3a7d2c, Rock #808080"; assign it with Create or set LandscapeMaterial, then Paint the layers; returns the material (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Landscape", meta = (BlueprintInternalUseOnly = "true"))
	UMaterial* CreateLayerMaterial(const FString& Path, const FString& Layers);
};
