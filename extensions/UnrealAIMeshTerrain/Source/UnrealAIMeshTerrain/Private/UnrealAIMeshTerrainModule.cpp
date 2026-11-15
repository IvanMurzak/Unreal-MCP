// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIMeshTerrainCommon.h"
#include "UnrealAIMeshTerrainRqlApi.h"
#include "UnrealAIMeshTerrainModifiersApi.h"

#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionPreviewSection.h"
#include "Modifiers/MeshPartitionLatticeModifier.h"

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
		Context.AddApiSource(TEXT("meshTerrainModifier"),
			TEXT("Mesh Terrain modifier operations: call AddNoise / AddPatch / AddWeight / AddSpline / AddShape / AddLattice / AddTexturePatch / AddRemesh / Add(terrain, kind, location) / Edit / Move / SetSplinePoints / SetLatticePoints / SetOrder / Enable / Disable / Bind / Remove(modifier); each waits until the terrain is rebuilt"),
			UUnrealAIMeshTerrainModifiersApi::StaticClass(), TEXT("MeshTerrainModifierOperations"));

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
		Context.AddJsonMember(UE::MeshPartition::ULatticeModifier::StaticClass(), TEXT("LatticePoints"),
			TEXT("the lattice's control points (world [X, Y, Z], cm) in the order meshTerrainModifier | call SetLatticePoints(modifier, points) takes them"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FVector& Point : LatticeWorldPoints(Cast<UModifierComponent>(Object)))
					Items.Add(MakeShared<FJsonValueArray>(Vector({ Point.X, Point.Y, Point.Z })));
				return MakeShared<FJsonValueArray>(Items);
			});

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
`Channels` the paint weights there; off the terrain `Inside` is false. The operations load a terrain's base and modifier
actors (World Partition) before they work; `Build.LoadedActors` shows how many are loaded. Mesh Terrain needs Unreal
Engine 5.8+, the MeshTerrainMode plugin and a World Partition level.)MD"));
		Context.AddGuide(TEXT("Mesh Terrain modifiers (Experimental, UE 5.8+)"), TEXT(R"MD(`meshTerrainModifier` adds and changes the modifiers that shape a
Mesh Terrain. Every Add names its terrain (a modifier is bound to it, never to the nearest one); every call is one undo step and
returns after the terrain is rebuilt: `{Modifier, Actor, Kind, Terrain, PriorityLayer, Priority, Disabled, Ready, Seconds,
State}`; read the result with `Surface`. Sizes, radii and heights are cm; Location Z is the height of a Patch or Shape (a Spline takes its points' Z).

```
meshTerrainModifier | call AddNoise(unreal.find("Terrain"), [0, 0, 0], [8000, 8000], 600, 0.0005)        hills: Size [X, Y], Strength (height), Frequency (1/cm), Falloff 0..1
meshTerrainModifier | call AddPatch(unreal.find("Terrain"), [1200, -800, 350], 1500, 800)                  flat pad at Z: Radius, Falloff
meshTerrainModifier | call AddSpline(unreal.find("Terrain"), [[0, 0, 100], [5000, 2000, 150]], 600, 400)  road: Width, Falloff, Closed, Channel
meshTerrainModifier | call AddShape(unreal.find("Terrain"), "Sphere", [3000, 0, 0], [2000, 2000, 1200], "Subtract")   Box Sphere Cylinder Capsule Cone Torus (Union / Subtract / Trim), Rectangle Disc (Project)
meshTerrainModifier | call AddWeight(unreal.find("Terrain"), "Red", [0, 0, 0], 1000, 500, 1)              paint a channel (Channels)
meshTerrainModifier | call AddLattice(unreal.find("Terrain"), [0, 0, 0], [4000, 4000, 2000], 3)           then SetLatticePoints(modifier, points) from its LatticePoints
meshTerrainModifier | call AddTexturePatch(unreal.find("Terrain"), "/Game/T_Height", [0, 0, 0], [4000, 4000], 1000)   heightmap stamp
meshTerrainModifier | call AddRemesh(unreal.find("Terrain"), [0, 0, 0], [4000, 4000, 50000], 100)          finer triangles
meshTerrainModifier | call Add(unreal.find("Terrain"), "InstancedPatch", [0, 0, 0])                         any other kind with defaults, then set
meshTerrainModifier | call Edit(unreal.find("Hills").Modifier, [-1, -1, -1], -1, -1, 900)               Size / Radius / Width / Strength / Falloff (-1 keeps)
meshTerrainModifier | call Move(unreal.find("Pad").Modifier, [0, 500, 400])
meshTerrainModifier | call SetOrder(unreal.find("Pad").Modifier, 10)                                    higher Priority = applied later, on top
meshTerrainModifier | call Disable(unreal.find("Pad").Modifier)        / Enable / Remove / Bind(modifier, terrain) / SetSplinePoints(modifier, points, closed)
meshTerrainModifiers | where Kind == "Patch" | call meshTerrainModifier.Edit($, [-1, -1, -1], 2000)       one call per modifier
```

Other properties: `set` them on the modifier (`rql-schema NoiseModifier`), then `meshTerrain | call WaitForBuild(terrain)`.
Use the operations, not `set`, to move a modifier (`set Location` on its actor does not rebuild the terrain), to change a
WeightUtility's InnerValue (Edit Strength; the engine caches the old weights) and a Lattice's size (Edit; never set LatticeResolution: its
points would no longer match, an engine ensure). Where two modifiers overlap the later one in build order wins (`Modifiers` lists
them in order); the Boolean modifier may log "reported errors ... cracks" for box shapes: an engine warning, check
`Surface`.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIMeshTerrainRqlModule>, UnrealAIMeshTerrain)
