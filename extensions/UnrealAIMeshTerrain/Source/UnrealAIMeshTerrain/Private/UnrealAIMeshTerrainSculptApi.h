// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIMeshTerrainSculptApi.generated.h"

namespace UE::MeshPartition
{
	class AMeshPartition;
}

/** The result of a brush stroke (`meshTerrainBrush | call Sculpt / Paint / Erase`). */
USTRUCT(BlueprintType)
struct FUnrealAIMeshTerrainStroke
{
	GENERATED_BODY()

	/** the brush modifier actor (Mesh Terrain Mode's BrushModifier, a ProjectMeshLayersModifier) that stores the stroke */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString Brush;

	/** true when the stroke placed that brush modifier (no brush covered the area yet) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	bool NewBrush = false;

	/** the terrain vertices the stroke changed */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	int32 Vertices = 0;

	/** at the vertex nearest the point, before the stroke: the ground height (world Z, cm) for Sculpt, the channel weight for Paint / Erase */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double Before = 0.0;

	/** ... and after it */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double After = 0.0;

	/** true when the terrain's mesh, collision and rendering show the stroke (the call waited for the build) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	bool Ready = false;

	/** "built", or (Ready false) that the stroke is applied while the terrain still builds: WaitForBuild before reading it */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString State;
	/** how long the stroke and its build took (seconds) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double Seconds = 0.0;
};

/** The result of `meshTerrainBrush | call PaintWhere`. */
USTRUCT(BlueprintType)
struct FUnrealAIMeshTerrainPaintWhere
{
	GENERATED_BODY()

	/** the brush modifier actors that hold the paint (one per tile of at most 600 x 600 m) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	TArray<FString> Brushes;

	/** how many of them this call placed (the others already covered their tile: a call with the same Within reuses them) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	int32 NewBrushes = 0;

	/** the terrain vertices whose weight changed */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	int32 Vertices = 0;

	/** the terrain vertices in Within that matched the slope and height ranges (painted at full Strength) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	int32 Matched = 0;

	/** true when the terrain's mesh, collision and rendering show the paint */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	bool Ready = false;

	/** "built", or (Ready false) that the paint is applied while the terrain still builds */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString State;

	/** the tiles left unpainted and why (empty when every tile was painted) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	FString Note;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Terrain")
	double Seconds = 0.0;
};

/**
 * The `meshTerrainBrush` RQL source (type "MeshTerrainBrushOperations"): brush strokes on a Mesh Terrain, stored the way
 * Mesh Terrain Mode's Sculpt and Paint tools store them: in the sculpt layer / weight attributes of a brush modifier
 * (UProjectMeshLayersModifier, "BrushModifier") edited through the tools' UEditableModifierToolTarget. Strokes in an area
 * a brush modifier already covers go into that modifier (its active sculpt layer), so they stay editable in the mode.
 */
UCLASS(Transient)
class UUnrealAIMeshTerrainSculptApi : public UObject
{
	GENERATED_BODY()

public:
	/** sculpt a Mesh Terrain around Point (world X, Y) within Radius (cm, at most 25000) with a soft edge (Falloff 0..1: the outer part of the radius that fades, 0 = hard edge); Mode "Raise" / "Lower" moves the ground Height cm (0..100000) up / down at the centre, "Flatten" pulls it to the world Z Height, "Smooth" evens it out (Height ignored); Strength (0 exclusive) ..1 scales the stroke; stored in a brush modifier (one per area, repeated strokes accumulate in it, editable in Mesh Terrain Mode's Sculpt tools); one undo step; returns when the terrain is rebuilt: Brush, NewBrush, Vertices, Before / After (the full-resolution height at the centre), Ready, Seconds; when the build takes longer than 240 s the stroke is still applied: Ready false and State say so, call meshTerrain | call WaitForBuild, do not repeat it */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainStroke Sculpt(UE::MeshPartition::AMeshPartition* Terrain, const FString& Mode, FVector Point, double Radius, double Height = 100.0, double Strength = 1.0, double Falloff = 0.5);

	/** paint a paint channel of a Mesh Terrain (one of its Channels, e.g. "Red") around Point (world X, Y) within Radius (cm, at most 25000) towards Weight (0..1, 0 erases; reached where the brush is at full strength: inside the radius less its Falloff part) with a soft edge (Falloff 0..1); stored in a brush modifier's weight attributes like Mesh Terrain Mode's Paint tool; one undo step; returns when rebuilt: Brush, NewBrush, Vertices, Before / After (the weight at the centre), Ready, Seconds; read it back with meshTerrain | call Surface (Channels) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainStroke Paint(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, FVector Point, double Radius, double Weight = 1.0, double Falloff = 0.5);

	/** erase a paint channel of a Mesh Terrain around Point (world X, Y) within Radius (cm, at most 25000) with a soft edge (Falloff 0..1): Paint with Weight 0; where nothing is painted it changes nothing (Vertices 0) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIMeshTerrainStroke Erase(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, FVector Point, double Radius, double Falloff = 0.5);

	/** paint a channel of a Mesh Terrain (one of its Channels) where the ground's slope is MinSlope..MaxSlope degrees and its height MinHeight..MaxHeight (world Z, cm), within Within ([MinX, MinY, MaxX, MaxY] world cm; empty: the whole terrain): every such vertex moves to Strength (the weight 0..1), with soft edges 2 degrees past the slope bounds and 50 cm past the height bounds; the rest keeps its weight. Sand on beaches: MinHeight -200, MaxHeight 300; rock on cliffs: MinSlope 30; grass on flat ground: MaxSlope 15. Slope and height are the ground's when the call runs (the modifiers before the paint): paint last, after shaping. Stored like Paint strokes, in brush modifiers (one per tile of at most 600 x 600 m, labelled Label or "<terrain>_Paint"; a later call over the same tiles reuses them); one undo step; returns when rebuilt: Brushes, NewBrushes, Vertices (changed), Matched, Ready, State, Note (tiles left unpainted), Seconds */
	// The defaults are RQL's (C++ cannot default the array after them): UHT's CPP_Default_ keys, which RQL reads.
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Mesh Terrain", meta = (BlueprintInternalUseOnly = "true", CPP_Default_MinSlope = "0.000000",
		CPP_Default_MaxSlope = "90.000000", CPP_Default_MinHeight = "-10000000.000000", CPP_Default_MaxHeight = "10000000.000000", CPP_Default_Strength = "1.000000",
		CPP_Default_Label = "", CPP_Default_Within = ""))
	FUnrealAIMeshTerrainPaintWhere PaintWhere(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, double MinSlope, double MaxSlope,
		double MinHeight, double MaxHeight, double Strength, const FString& Label, const TArray<double>& Within);
};
