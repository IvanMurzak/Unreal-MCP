// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIMeshTerrainCommon.h"
#include "UnrealAIMeshTerrainRqlApi.h"
#include "UnrealAIMeshTerrainSculptApi.h"

#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionPreviewSection.h"

namespace UnrealAIMeshTerrainModuleLocal
{
	using namespace UnrealAIMeshTerrain;

	double Round(double Value)
	{
		return FMath::RoundToDouble(Value * 10.0) / 10.0;
	}

	TArray<TSharedPtr<FJsonValue>> Vector(std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const double Value : Values)
			Items.Add(MakeShared<FJsonValueNumber>(Round(Value)));
		return Items;
	}

	TArray<TSharedPtr<FJsonValue>> Names(const TArray<FName>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FName& Value : Values)
			Items.Add(MakeShared<FJsonValueString>(Value.ToString()));
		return Items;
	}

	/** {Min [X, Y, Z], Max [X, Y, Z], SizeMeters [X, Y]} of a world box (cm); empty for an invalid box. */
	TSharedPtr<FJsonValue> BoxJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Box.IsValid)
		{
			Out->SetArrayField(TEXT("Min"), Vector({ Box.Min.X, Box.Min.Y, Box.Min.Z }));
			Out->SetArrayField(TEXT("Max"), Vector({ Box.Max.X, Box.Max.Y, Box.Max.Z }));
			Out->SetArrayField(TEXT("SizeMeters"), Vector({ (Box.Max.X - Box.Min.X) / 100.0, (Box.Max.Y - Box.Min.Y) / 100.0 }));
		}
		return MakeShared<FJsonValueObject>(Out);
	}

	/** Every modifier component of the editor world (base meshes left out unless @p bBases), bound to a terrain or not. */
	TArray<UObject*> WorldModifiers(bool bBases)
	{
		TArray<UObject*> Out;
		const UWorld* World = UnrealMcpRql::EditorWorld();
		for (TObjectIterator<UModifierComponent> It; It && World; ++It)
		{
			if (IsValid(*It) && It->GetWorld() == World && (bBases || !It->IsBase()))
				Out.Add(*It);
		}
		return Out;
	}

	TSharedPtr<FJsonValue> BuildJson(UObject* Object)
	{
		const AMeshPartition* Terrain = Cast<AMeshPartition>(Object);
		const FBuildState State = BuildState(Terrain);
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("Ready"), State.IsReady());
		Out->SetNumberField(TEXT("Sections"), State.Sections);
		Out->SetNumberField(TEXT("Triangles"), State.Triangles);
		Out->SetBoolField(TEXT("Building"), State.bBuilding);
		Out->SetNumberField(TEXT("Cooking"), State.Cooking);
		Out->SetNumberField(TEXT("Finishing"), State.Finishing);
		Out->SetNumberField(TEXT("CompilingMeshes"), State.CompilingMeshes);
		Out->SetNumberField(TEXT("Actors"), State.Actors);
		Out->SetNumberField(TEXT("LoadedActors"), State.LoadedActors);
		Out->SetStringField(TEXT("State"), State.Actors > State.LoadedActors
			? FString::Printf(TEXT("%s; %d of its %d base / modifier actors are not loaded (World Partition): any meshTerrain call loads them"),
				*State.Describe(), State.Actors - State.LoadedActors, State.Actors)
			: State.Describe());
		return MakeShared<FJsonValueObject>(Out);
	}

	TSharedPtr<FJsonValue> ModifierJson(UObject* Object)
	{
		const UModifierComponent* Modifier = Cast<UModifierComponent>(Object);
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Modifier)
			return MakeShared<FJsonValueObject>(Out);
		const AMeshPartition* Terrain = TerrainOf(Modifier);
		const UMeshPartitionEditorComponent* Component = EditorComponent(Terrain);
		Out->SetStringField(TEXT("Kind"), ModifierKind(Modifier));
		Out->SetStringField(TEXT("Actor"), Modifier->GetOwner() ? Modifier->GetOwner()->GetActorNameOrLabel() : FString());
		Out->SetStringField(TEXT("Terrain"), Terrain ? Terrain->GetActorNameOrLabel() : FString());
		Out->SetStringField(TEXT("PriorityLayer"), Modifier->GetType().ToString());
		Out->SetNumberField(TEXT("LayerIndex"), Component ? static_cast<double>(Component->GetModifierLayerIndex(Modifier->GetType())) : -1.0);
		Out->SetNumberField(TEXT("SubPriority"), Modifier->GetPriority());
		Out->SetBoolField(TEXT("Disabled"), Modifier->GetIsDisabledFlag());
		Out->SetBoolField(TEXT("IsBase"), Modifier->IsBase());
		Out->SetField(TEXT("Bounds"), BoxJson(Modifier->ComputeCombinedBounds()));
		return MakeShared<FJsonValueObject>(Out);
	}
}

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Mesh Terrains (UE 5.8 Mesh Partition, Experimental)
 * are queried with rql-query. Reflection exposes the MeshPartition actor, its definition and the modifier components
 * ("MeshPartition", "MeshPartitionDefinition", "NoiseModifier", ...); the module adds world sources, the build state,
 * sections, bounds and modifier summaries, and the agent guide rendered into the unreal-rql skill.
 */
class FUnrealAIMeshTerrainRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("MeshTerrain"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-mesh-terrain"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAIMeshTerrain")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		using namespace UnrealAIMeshTerrainModuleLocal;
		UClass* TerrainClass = AMeshPartition::StaticClass();
		UClass* ModifierClass = UModifierComponent::StaticClass();

		Context.AddObjectListSource(TEXT("meshTerrains"), TEXT("the Mesh Terrain (Mesh Partition) actors of the editor world"),
			TerrainClass, []()
			{
				TArray<UObject*> Out;
				for (AMeshPartition* Terrain : Terrains())
					Out.Add(Terrain);
				return Out;
			});
		Context.AddObjectListSource(TEXT("meshTerrainModifiers"),
			TEXT("the Mesh Terrain modifier components of the editor world (noise, splines, shapes, sculpt layers, ...; not the base meshes)"),
			ModifierClass, []() { return WorldModifiers(/*bBases*/ false); });

		Context.AddApiSource(TEXT("meshTerrain"),
			TEXT("Mesh Terrain operations: call Rebuild(terrain, timeoutSeconds) / WaitForBuild(terrain, timeoutSeconds) / Surface(terrain, point) / SurfacePoints(terrain, points) / EnableMeshTerrain()"),
			UUnrealAIMeshTerrainRqlApi::StaticClass(), TEXT("MeshTerrainOperations"));
		Context.AddApiSource(TEXT("meshTerrainBrush"),
			TEXT("Mesh Terrain brush strokes, stored like Mesh Terrain Mode's Sculpt / Paint tools store them: call Sculpt(terrain, mode, point, radius, height, strength, falloff) / Paint(terrain, channel, point, radius, weight, falloff) / Erase(terrain, channel, point, radius, falloff)"),
			UUnrealAIMeshTerrainSculptApi::StaticClass(), TEXT("MeshTerrainBrushOperations"));

		Context.AddObjectMember(TerrainClass, TEXT("Definition"), TEXT("the terrain's Mesh Partition definition (material, priority layers, paint channels)"),
			UMeshPartitionDefinition::StaticClass(), [](UObject* Object) -> UObject*
			{
				const AMeshPartition* Terrain = Cast<AMeshPartition>(Object);
				return Terrain ? Terrain->GetMeshPartitionDefinition() : nullptr;
			});
		Context.AddJsonMember(TerrainClass, TEXT("PriorityLayers"),
			TEXT("the priority layers modifiers are applied in, first to last (the definition's Modifier Layer Priorities)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const AMeshPartition* Terrain = Cast<AMeshPartition>(Object);
				const UMeshPartitionDefinition* Definition = Terrain ? Terrain->GetMeshPartitionDefinition() : nullptr;
				return MakeShared<FJsonValueArray>(Definition ? Names(Definition->GetModifierTypePriorities()) : TArray<TSharedPtr<FJsonValue>>());
			});
		Context.AddJsonMember(TerrainClass, TEXT("Channels"), TEXT("the paint / weight channels of the terrain's definition"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const AMeshPartition* Terrain = Cast<AMeshPartition>(Object);
				const UMeshPartitionDefinition* Definition = Terrain ? Terrain->GetMeshPartitionDefinition() : nullptr;
				return MakeShared<FJsonValueArray>(Definition ? Names(Definition->GetChannelNames()) : TArray<TSharedPtr<FJsonValue>>());
			});
		Context.AddJsonMember(TerrainClass, TEXT("Bounds"),
			TEXT("the built terrain's world box (cm): Min [X, Y, Z], Max [X, Y, Z] (Z: the lowest and highest ground), SizeMeters [X, Y]"),
			[](UObject* Object) -> TSharedPtr<FJsonValue> { return BoxJson(BuiltBounds(Cast<AMeshPartition>(Object))); });
		Context.AddJsonMember(TerrainClass, TEXT("Build"),
			TEXT("the asynchronous build: Ready (mesh, collision and rendering match the modifiers), Sections, Triangles (of the mesh shown now; the final Nanite mesh counts its fallback LOD), Building, Cooking (collision), Finishing (final mesh not made), CompilingMeshes, Actors / LoadedActors (World Partition), State"),
			[](UObject* Object) -> TSharedPtr<FJsonValue> { return BuildJson(Object); });
		Context.AddJsonMember(TerrainClass, TEXT("Sections"),
			TEXT("the built (preview) sections: Name, Triangles (rendered now), Bounds {Min, Max, SizeMeters}"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const APreviewSection* Section : PreviewSections(Cast<AMeshPartition>(Object)))
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("Name"), Section->GetActorNameOrLabel());
					Item->SetNumberField(TEXT("Triangles"), RenderedTriangles(Section));
					Item->SetField(TEXT("Bounds"), BoxJson(Section->GetPreviewMeshBounds()));
					Items.Add(MakeShared<FJsonValueObject>(Item));
				}
				return MakeShared<FJsonValueArray>(Items);
			});
		Context.AddObjectListMember(TerrainClass, TEXT("Modifiers"),
			TEXT("the modifiers that shape the terrain, in build order (priority layer, then sub-priority); base meshes left out"),
			ModifierClass, [](UObject* Object)
			{
				TArray<UObject*> Out;
				for (UModifierComponent* Modifier : Modifiers(Cast<AMeshPartition>(Object)))
					Out.Add(Modifier);
				return Out;
			});
		Context.AddObjectListMember(TerrainClass, TEXT("BaseMeshes"),
			TEXT("the terrain's base meshes (MeshProvider modifiers: the sections Create or a heightmap import made)"),
			ModifierClass, [](UObject* Object)
			{
				TArray<UObject*> Out;
				for (UModifierComponent* Modifier : Modifiers(Cast<AMeshPartition>(Object), /*bBases*/ true))
				{
					if (Modifier->IsBase())
						Out.Add(Modifier);
				}
				return Out;
			});

		Context.AddObjectMember(ModifierClass, TEXT("Terrain"), TEXT("the Mesh Terrain the modifier shapes (null when none)"),
			TerrainClass, [](UObject* Object) -> UObject* { return TerrainOf(Cast<UModifierComponent>(Object)); });
		Context.AddJsonMember(ModifierClass, TEXT("Kind"), TEXT("the modifier kind: Noise, Spline, Boolean, Patch, TexturePatch, Lattice, Remesh, ProjectMeshLayers (sculpt), MeshProvider (a base mesh), ..."),
			[](UObject* Object) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueString>(ModifierKind(Cast<UModifierComponent>(Object))); });
		Context.AddJsonMember(ModifierClass, TEXT("Disabled"), TEXT("true when the modifier is switched off (its Disabled flag)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UModifierComponent* Modifier = Cast<UModifierComponent>(Object);
				return MakeShared<FJsonValueBoolean>(Modifier && Modifier->GetIsDisabledFlag());
			});
		Context.AddJsonMember(ModifierClass, TEXT("Modifier"),
			TEXT("modifier summary: Kind, Actor, Terrain, PriorityLayer, LayerIndex (build order of the layer), SubPriority (order within it, higher later), Disabled, IsBase, Bounds"),
			[](UObject* Object) -> TSharedPtr<FJsonValue> { return ModifierJson(Object); });

		Context.AddGuide(TEXT("Mesh Terrain (Experimental, UE 5.8+)"), TEXT(R"MD(A Mesh Terrain (Unreal 5.8's experimental Mesh Partition) is a `MeshPartition` actor shaped by a stack of
modifier components (noise, splines, shapes, sculpt layers, ...) on top of its base meshes, built asynchronously into
preview sections (the rendered mesh and its collision). `meshTerrains` lists the terrains, `meshTerrainModifiers` the
modifiers of the level; `Build`, `Bounds`, `Sections`, `Modifiers`, `PriorityLayers`, `Channels` and `Definition`
summarise a terrain, `Modifier` (Kind, PriorityLayer, SubPriority, Disabled, Bounds) a modifier.

```
meshTerrains | select Label Build Bounds PriorityLayers Channels
meshTerrains | first | get Modifiers | select Modifier
meshTerrainModifiers | where Kind == "Noise" | select $ref Modifier
```

Modifiers apply layer by layer (the definition's `PriorityLayers`, first to last), then by `Priority` (sub-priority,
higher later) within a layer. A terrain rebuilds after every change in the background: read heights with `Surface`, which
waits for that build, rather than with line traces, and call `WaitForBuild` after `set` on a modifier before reading
`Build` or rendering a screenshot.

```
meshTerrain | call Surface(unreal.find("Terrain"), [1200, -800, 0])      {Inside, Height, Slope, Location, Normal, Channels}
meshTerrain | call SurfacePoints(unreal.find("Terrain"), [[0, 0, 0], [500, 0, 0], [1000, 0, 0]])   one call for many points
meshTerrainModifiers | where Kind == "Noise" | set bIsDisabled = true
meshTerrain | call WaitForBuild(unreal.find("Terrain"))                    {Ready, Sections, Triangles, Modifiers, Seconds, State}
meshTerrain | call Rebuild(unreal.find("Terrain"))                         rebuild every section from the modifiers
meshTerrain | call EnableMeshTerrain()                                     MeshTerrainMode + MeshPartition in the project file
```

`Height` is the world Z (cm) collision and traces hit, `Slope` degrees (0 flat), `Location` a spawn point on the ground,
`Channels` the paint weights there; off the terrain `Inside` is false.

Brush strokes (`meshTerrainBrush`) shape and paint by hand, like Mesh Terrain Mode's Sculpt and Paint tools: around a
world X, Y within `Radius` cm (at most 25000) with a soft edge (`Falloff` 0..1, 0 = hard). Sculpt modes: `Raise` / `Lower`
(by `Height` cm at the centre), `Flatten` (to world Z `Height`), `Smooth`; `Strength` 0..1 scales a stroke. `Paint` moves a
channel (one of the terrain's `Channels`, e.g. "Red") towards `Weight` 0..1; `Erase` takes it to 0. A stroke is stored in a
brush modifier (`Kind` ProjectMeshLayers, "<terrain>_Brush"): strokes near one go into it, so they accumulate and stay
editable in the mode. Each call is one undo step and returns when the terrain is rebuilt.

```
meshTerrainBrush | call Sculpt(unreal.find("Terrain"), "Raise", [1200, -800, 0], 1500, 400)      a 4 m hill, 15 m radius
meshTerrainBrush | call Sculpt(unreal.find("Terrain"), "Flatten", [0, 0, 0], 2000, 150, 1, 0.3)  a building pad at Z 150
meshTerrainBrush | call Sculpt(unreal.find("Terrain"), "Smooth", [0, 0, 0], 2500)
meshTerrainBrush | call Paint(unreal.find("Terrain"), "Red", [0, 0, 0], 2000, 1)                {Brush, NewBrush, Vertices, Before, After, Ready, Seconds}
meshTerrainBrush | call Erase(unreal.find("Terrain"), "Red", [0, 0, 0], 500)
```

`Before` / `After` are the full-resolution ground height (or channel weight) at the vertex nearest the point; `Surface`
reads the rendered collision, which on rough ground can differ by some cm. The operations load a terrain's base and modifier
actors (World Partition) before they work; `Build.LoadedActors` shows how many are loaded. Mesh Terrain needs Unreal
Engine 5.8+, the MeshTerrainMode plugin and a World Partition level.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIMeshTerrainRqlModule>, UnrealAIMeshTerrain)
