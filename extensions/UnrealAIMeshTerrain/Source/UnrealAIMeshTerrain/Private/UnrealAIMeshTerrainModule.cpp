// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIMeshTerrainCommon.h"
#include "UnrealAIMeshTerrainRqlApi.h"
#include "UnrealAIMeshTerrainCreateApi.h"
#include "UnrealAIMeshTerrainSculptApi.h"
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

	/**
	 * Load the World Partition base / modifier actors of @p ToLoad before listing them: after a level is reopened they are
	 * unloaded, and an agent would find no modifier to name (unreal.find) until some operation loaded them. Not while
	 * Mesh Terrain cannot be edited (Play In Editor running, plugins off).
	 */
	void LoadForListing(TConstArrayView<AMeshPartition*> ToLoad)
	{
		if (!UnavailableReason().IsEmpty())
			return;
		for (const AMeshPartition* Terrain : ToLoad)
			LoadTerrainActors(Terrain);
	}

	/** The modifiers of a terrain (@p Object) in build order, loaded first; with @p bBases only its base meshes. */
	TArray<UObject*> ListedModifiers(UObject* Object, bool bBases)
	{
		AMeshPartition* Terrain = Cast<AMeshPartition>(Object);
		LoadForListing({ Terrain });
		TArray<UObject*> Out;
		for (UModifierComponent* Modifier : Modifiers(Terrain, bBases))
		{
			if (!bBases || Modifier->IsBase())
				Out.Add(Modifier);
		}
		return Out;
	}

	/**
	 * Every modifier component of the editor world, bound to a terrain or not; base meshes left out unless @p bBases, except
	 * the ones a deleted terrain left behind (bound to none: leftovers to Remove).
	 */
	TArray<UObject*> WorldModifiers(bool bBases)
	{
		LoadForListing(Terrains());
		TArray<UObject*> Out;
		const UWorld* World = UnrealMcpRql::EditorWorld();
		for (TObjectIterator<UModifierComponent> It; It && World; ++It)
		{
			if (IsValid(*It) && It->GetWorld() == World && (bBases || !It->IsBase() || !IsValid(TerrainOf(*It))))
				Out.Add(*It);
		}
		return Out;
	}

	/** The Bounds member: the built ground's box, the terrain's own (Base) box, and where the ground is highest and lowest. */
	TSharedPtr<FJsonValue> BoundsJson(UObject* Object)
	{
		const AMeshPartition* Terrain = Cast<AMeshPartition>(Object);
		const TSharedPtr<FJsonValue> Out = BoxJson(BuiltBounds(Terrain));
		const TSharedPtr<FJsonObject>& Fields = Out->AsObject();
		if (!Fields->Values.IsEmpty())
		{
			const FBox Base = BaseBounds(Terrain);
			Fields->SetField(TEXT("Base"), BoxJson(Base));
			TOptional<FVector> Highest, Lowest;
			GroundExtremes(Terrain, Base, Highest, Lowest);
			if (Highest)
				Fields->SetArrayField(TEXT("Highest"), Vector({ Highest->X, Highest->Y, Highest->Z }));
			if (Lowest)
				Fields->SetArrayField(TEXT("Lowest"), Vector({ Lowest->X, Lowest->Y, Lowest->Z }));
		}
		return Out;
	}

	TSharedPtr<FJsonValue> BuildJson(UObject* Object)
	{
		const AMeshPartition* Terrain = Cast<AMeshPartition>(Object);
		const FBuildState State = BuildState(Terrain);
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		// A terrain with unloaded base / modifier actors (World Partition, after the level is reopened) is built without them.
		const bool bUnloaded = State.Actors > State.LoadedActors;
		// While Play In Editor runs the editor shows no sections (nothing to report as built) and the operations refuse.
		const FString Unavailable = UnavailableReason();
		Out->SetBoolField(TEXT("Ready"), State.IsReady() && !bUnloaded && Unavailable.IsEmpty());
		Out->SetNumberField(TEXT("Sections"), State.Sections);
		Out->SetNumberField(TEXT("Triangles"), State.Triangles);
		Out->SetBoolField(TEXT("Building"), State.bBuilding);
		Out->SetNumberField(TEXT("Cooking"), State.Cooking);
		Out->SetNumberField(TEXT("Finishing"), State.Finishing);
		Out->SetNumberField(TEXT("CompilingMeshes"), State.CompilingMeshes);
		Out->SetNumberField(TEXT("Actors"), State.Actors);
		Out->SetNumberField(TEXT("LoadedActors"), State.LoadedActors);
		FString StateText;
		if (!Unavailable.IsEmpty())
			StateText = Unavailable; // first: nothing else is meaningful while the operations refuse
		else if (bUnloaded)
		{
			StateText = FString::Printf(TEXT("%s; %d of its %d base / modifier actors are not loaded (World Partition), so it is built without them: any meshTerrain call loads them"),
				State.bNoBase ? TEXT("not built") : *State.Describe(), State.Actors - State.LoadedActors, State.Actors);
		}
		else
			StateText = State.Describe();
		Out->SetStringField(TEXT("State"), StateText);
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
		// GetModifierLayerIndex is unsigned: a modifier in no priority layer of the definition ("None") is INDEX_NONE (-1).
		const int32 LayerIndex = Component ? static_cast<int32>(Component->GetModifierLayerIndex(Modifier->GetType())) : INDEX_NONE;
		Out->SetNumberField(TEXT("LayerIndex"), LayerIndex);
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
			TEXT("the Mesh Terrain modifier components of the editor world (noise, splines, shapes, sculpt layers, ...; not the base meshes, except those a deleted terrain left behind); loads the terrains' World Partition actors"),
			ModifierClass, []() { return WorldModifiers(/*bBases*/ false); });

		Context.AddApiSource(TEXT("meshTerrain"),
			TEXT("Mesh Terrain operations: call Rebuild(terrain, timeoutSeconds) / WaitForBuild(terrain, timeoutSeconds) / Surface(terrain, point) / SurfacePoints(terrain, points) / EnableMeshTerrain()"),
			UUnrealAIMeshTerrainRqlApi::StaticClass(), TEXT("MeshTerrainOperations"));
		Context.AddApiSource(TEXT("meshTerrainModifier"),
			TEXT("Mesh Terrain modifier operations: call AddNoise / AddPatch / AddWeight / AddSpline / AddRoad / AddShape / AddLattice / AddTexturePatch / AddRemesh / Add(terrain, kind, location) / Edit / Move / SetSplinePoints / SetLatticePoints / SetOrder / Enable / Disable / Bind / Remove(modifier); each waits until the terrain is rebuilt"),
			UUnrealAIMeshTerrainModifiersApi::StaticClass(), TEXT("MeshTerrainModifierOperations"));
		Context.AddApiSource(TEXT("meshTerrainCreate"),
			TEXT("make and delete Mesh Terrains: call Create(label, location, sizeMeters, ...) / ImportHeightmap(label, file, location, sizeMeters, heightScale, ...) / Delete(terrain)"),
			UUnrealAIMeshTerrainCreateApi::StaticClass(), TEXT("MeshTerrainCreateOperations"));
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
			TEXT("the built ground's world box (cm): Min [X, Y, Z], Max [X, Y, Z] (Z: the lowest and highest ground), SizeMeters [X, Y]; Base {Min, Max, SizeMeters}: the terrain's own extent (its base meshes: the size Create / ImportHeightmap gave it; the built box is larger where a Union shape reaches past the edge); Highest and Lowest [X, Y, Z]: where the ground within Base is highest and lowest (to a vertex of its collision)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue> { return BoundsJson(Object); });
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
			ModifierClass, [](UObject* Object) { return ListedModifiers(Object, /*bBases*/ false); });
		Context.AddObjectListMember(TerrainClass, TEXT("BaseMeshes"),
			TEXT("the terrain's base meshes (MeshProvider modifiers: the sections Create or a heightmap import made)"),
			ModifierClass, [](UObject* Object) { return ListedModifiers(Object, /*bBases*/ true); });

		Context.AddObjectMember(ModifierClass, TEXT("Terrain"), TEXT("the Mesh Terrain the modifier shapes (null when none)"),
			TerrainClass, [](UObject* Object) -> UObject* { return TerrainOf(Cast<UModifierComponent>(Object)); });
		Context.AddJsonMember(ModifierClass, TEXT("Kind"), TEXT("the modifier kind: Noise, Spline, Boolean, Patch, TexturePatch, Lattice, Remesh, ProjectMeshLayers (sculpt), MeshProvider (a base mesh), ..."),
			[](UObject* Object) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueString>(ModifierKind(Cast<UModifierComponent>(Object))); });
		// Agents select these on a modifier as they do on a terrain (Modifier.Actor / Modifier.Bounds hold the same).
		Context.AddJsonMember(ModifierClass, TEXT("Label"), TEXT("the label of the modifier's actor (what unreal.find takes)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UModifierComponent* Modifier = Cast<UModifierComponent>(Object);
				return MakeShared<FJsonValueString>(Modifier && Modifier->GetOwner() ? Modifier->GetOwner()->GetActorNameOrLabel() : FString());
			});
		Context.AddJsonMember(ModifierClass, TEXT("Bounds"), TEXT("where the modifier acts: its world box (cm) Min [X, Y, Z], Max [X, Y, Z], SizeMeters [X, Y]"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UModifierComponent* Modifier = Cast<UModifierComponent>(Object);
				return BoxJson(Modifier ? Modifier->ComputeCombinedBounds() : FBox(ForceInit));
			});
		Context.AddJsonMember(ModifierClass, TEXT("Disabled"), TEXT("true when the modifier is switched off (its Disabled flag)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UModifierComponent* Modifier = Cast<UModifierComponent>(Object);
				return MakeShared<FJsonValueBoolean>(Modifier && Modifier->GetIsDisabledFlag());
			});
		Context.AddJsonMember(ModifierClass, TEXT("Modifier"),
			TEXT("modifier summary: Kind, Actor, Terrain, PriorityLayer, LayerIndex (build order of its priority layer; -1: in none of the definition's layers, applied after them), SubPriority (order within it, higher later), Disabled, IsBase, Bounds"),
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
summarise a terrain, `Label`, `Kind`, `Bounds` and `Modifier` (Kind, PriorityLayer, SubPriority, Disabled, Bounds) a modifier. Read a terrain's structure
through these sources and members, not with Python: Mesh Partition keeps its actors' and modifiers' settings under engine
names Python does not find ("Failed to find property"); `rql-schema NoiseModifier` (any modifier class) lists a modifier's
properties for `select` and `set`. Members, `Surface`, `SurfacePoints` and `WaitForBuild` are read-only (rql-query);
every other operation changes the level (rql-mutate).

```
meshTerrains | select Label Build Bounds PriorityLayers Channels
meshTerrains | where Label == "Island" | select Bounds        Base.SizeMeters: its size; Highest / Lowest [X, Y, Z]: its highest and lowest ground
meshTerrains | first | get Modifiers | select Modifier
meshTerrainModifiers | where Kind == "Noise" | select $ref Modifier
meshTerrainModifiers | select Label Kind Bounds Terrain.Label                 what each modifier is and where it acts
meshTerrainModifiers | where Terrain == null | select $ref Modifier   leftovers (see Repairs)
```

`Bounds` is the built ground's box, larger than the terrain's own extent `Bounds.Base` where a Union shape reaches past the edge.

Modifiers apply layer by layer (the definition's `PriorityLayers`, first to last), then by `Priority` (sub-priority,
higher later) within a layer. A terrain rebuilds after every change in the background: read heights with `Surface`, which
waits for that build, rather than with line traces, and call `WaitForBuild` after `set` on a modifier before reading
`Build` or rendering a screenshot. `Surface`, `SurfacePoints` and `WaitForBuild` are read-only: they work in rql-query.

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
(by `Height` cm at the centre), `Flatten` (to world Z `Height`), `Smooth`; `Strength` (0, 1] scales a stroke. `Paint` moves a
channel (one of the terrain's `Channels`, e.g. "Red") towards `Weight` 0..1; `Erase` takes it to 0. The channels are the
definition's generic paint slots (the default definition's are Red, Green, Blue, Yellow; its material decides how each
looks): paint a "rock" or "sand" layer into one of them and say which; unpainted ground keeps the base look. The default
material (`/MeshPartition/Materials/M_MeshPartition_Default`) only tints each channel with a flat debug colour over a grey
base: there is no grass, sand or rock look to find in it (do not dig through its graph); for real looks the terrain needs a
definition with its own material. A stroke is stored in a
brush modifier (`Kind` ProjectMeshLayers, "<terrain>_Brush"): strokes near one go into it, so they accumulate and stay
editable in the mode. Each call is one undo step and returns when the terrain is rebuilt (a stroke that times out
waiting for the build is already applied: `WaitForBuild`, do not repeat it). The brush stores offsets on top of the
modifiers before it, so edit noise / shapes first and sculpt last. A stroke is at most 250 m in radius: shape larger features
(a hill 1 km across, a basin) with a modifier (`meshTerrainModifier`: AddShape Sphere / Cone Union, AddNoise, AddTexturePatch)
and paint large areas with AddWeight.

```
meshTerrainBrush | call Sculpt(unreal.find("Terrain"), "Raise", [1200, -800, 0], 1500, 400)      a 4 m hill, 15 m radius
meshTerrainBrush | call Sculpt(unreal.find("Terrain"), "Flatten", [0, 0, 0], 2000, 150, 1, 0.3)  a building pad at Z 150
meshTerrainBrush | call Sculpt(unreal.find("Terrain"), "Smooth", [0, 0, 0], 2500)
meshTerrainBrush | call Paint(unreal.find("Terrain"), "Red", [0, 0, 0], 2000, 1)                {Brush, NewBrush, Vertices, Before, After, Ready, Seconds}
meshTerrainBrush | call Erase(unreal.find("Terrain"), "Red", [0, 0, 0], 500)
```

`Before` / `After` are the full-resolution ground height (or channel weight) at the vertex nearest the point; `Surface`
reads the rendered collision, which on rough ground can differ by some cm. The operations load a terrain's base and modifier
actors (World Partition) before they work; `Build.LoadedActors` shows how many are loaded. While Play In Editor runs the
editor hides the built sections: `Surface`, `SurfacePoints` and `WaitForBuild` refuse until play stops. After the level is reopened its
modifiers are unloaded and `unreal.find("<modifier label>")` finds nothing: list `meshTerrainModifiers` (or a terrain's
`Modifiers`) first, which loads them. Mesh Terrain needs Unreal Engine 5.8+, the MeshTerrainMode plugin and a World
Partition level.)MD"));
		Context.AddGuide(TEXT("Mesh Terrain modifiers (Experimental, UE 5.8+)"), TEXT(R"MD(`meshTerrainModifier` adds and changes the modifiers that shape a
Mesh Terrain. Every Add names its terrain (a modifier is bound to it, never to the nearest one); every call is one undo step and
returns after the terrain is rebuilt: `{Modifier, Actor, Kind, Terrain, Location, PriorityLayer, Priority, Disabled, Ready,
Seconds, State}`; read the result with `Surface`. Give each modifier a Label to find it again (`unreal.find("Hills")`): default labels
("NoiseModifier", "NoiseModifier2", ...) count across all terrains of the level. Sizes, radii and heights are cm; Location Z
is the height of a Patch or Shape (a Spline takes its points' Z).

```
meshTerrainModifier | call AddNoise(unreal.find("Terrain"), [0, 0, 0], [8000, 8000], 600, 0.0005, 0.5, "Hills")   hills: Size [X, Y], Strength (height), Frequency (1/cm), Falloff 0..1, Label
meshTerrainModifier | call AddPatch(unreal.find("Terrain"), [1200, -800, 350], 1500, 800, "Pad")           flat pad at Z: Radius, Falloff, Label
meshTerrainModifier | call AddSpline(unreal.find("Terrain"), [[0, 0, 100], [5000, 2000, 150]], 600, 400)  road: Width, Falloff, Closed, Channel
meshTerrainModifier | call AddRoad(unreal.find("Terrain"), [[0, 0, 0], [30000, 5000, 0], [60000, 0, 0]], 800, 8)   road: Width cm, MaxSlope deg (path X, Y; Z from the ground)
meshTerrainModifier | call AddShape(unreal.find("Terrain"), "Sphere", [3000, 0, 0], [2000, 2000, 1200], "Subtract")   Box Sphere Cylinder Capsule Cone Torus (Union / Subtract / Trim), Rectangle Disc (Project)
meshTerrainModifier | call AddWeight(unreal.find("Terrain"), "Red", [0, 0, 0], 1000, 500, 1)              paint a channel (Channels)
meshTerrainModifier | call AddLattice(unreal.find("Terrain"), [0, 0, 0], [4000, 4000, 2000], 3)           then SetLatticePoints(modifier, points) from its LatticePoints
meshTerrainModifier | call AddTexturePatch(unreal.find("Terrain"), "/Game/T_Height", [0, 0, 0], [4000, 4000], 1000)   heightmap stamp (not under -nullrhi)
meshTerrainModifier | call AddRemesh(unreal.find("Terrain"), [0, 0, 0], [4000, 4000, 50000], 100)          finer triangles
meshTerrainModifier | call Add(unreal.find("Terrain"), "InstancedPatch", [0, 0, 0])                         any other kind with defaults, then set
meshTerrainModifier | call Edit(unreal.find("Hills").Modifier, [-1, -1, -1], -1, -1, 900)               Size / Radius / Width / Strength / Falloff (-1 keeps)
meshTerrainModifier | call Move(unreal.find("Pad").Modifier, [0, 500, 400])                          TO this world position (not an offset)
meshTerrainModifier | call SetOrder(unreal.find("Pad").Modifier, 10)                                    higher Priority = applied later, on top
meshTerrainModifier | call Disable(unreal.find("Pad").Modifier)        / Enable / Remove / Bind(modifier, terrain) / SetSplinePoints(modifier, points, closed)
meshTerrainModifiers | where Kind == "Patch" | call meshTerrainModifier.Edit($, [-1, -1, -1], 2000)       one call per modifier
```

Other properties: `set` them on the modifier (`rql-schema NoiseModifier`), then `meshTerrain | call WaitForBuild(terrain)`.
Use the operations, not `set`, to move a modifier (`set Location` on its actor does not rebuild the terrain), to change a
WeightUtility's InnerValue (Edit Strength; the engine caches the old weights) and a Lattice's size (Edit; never set LatticeResolution: its
points would no longer match, an engine ensure). Where two modifiers overlap the later one in build order wins (`Modifiers` lists
them in order); the Boolean modifier may log "reported errors ... cracks" for box shapes: an engine warning, check
`Surface`. A Patch and brush strokes move the vertices within their radius: a Boolean shape's surface keeps the shape's
own triangles (a 1 km Sphere's are 100+ m across), so a pad or stroke smaller than those changes nothing there (`AddRemesh`
over the shape first gives it finer triangles).

A road with a grade limit: `AddRoad(terrain, points, width, maxSlope)` takes the path's X, Y, follows the ground and cuts or
fills where it is steeper, and remeshes a Boolean shape's coarse surface under it. Read `Reached`, `MaxSlope` (built) and
`Note`: a path shorter than `MinLengthMeters` (about rise / tan(maxSlope), with a margin) ends below the target; wind it
(switchbacks, a spiral around a hill), `Remove` the road (and its `Remesh`) and call again. A remeshed hill takes minutes
to rebuild after every later change there: an operation's 120 s wait may time out with the change applied, then
`WaitForBuild(terrain, 600)`.

Repairs: a modifier's `Bounds` say where it acts. Leftovers bound to no terrain (`where Terrain == null`): `Bind` a modifier to
a terrain or `Remove` it; a deleted terrain's base meshes (`Modifier.IsBase`) can only be removed; `meshTerrainCreate | call Delete(terrain)` deletes a whole
terrain with its modifiers. A pad at the wrong height: `Move` it to its own X, Y with the new Z (`Move` takes a world position, not an offset). A stray brush stroke: the opposite stroke
(Raise / Lower) at the same point with the same radius, height and falloff cancels it exactly (the brush stores offsets), or
`Remove` its brush when it holds no other stroke.)MD"));

		Context.AddGuide(TEXT("Mesh Terrain: create, import a heightmap, delete"), TEXT(R"MD(`meshTerrainCreate` makes Mesh Terrains the way
Mesh Terrain Mode's Create palette does; each call is one undo step and returns when the terrain is built.

```
meshTerrainCreate | call Create("Island", [0, 0, 0], 500)                       flat, 500 x 500 m, ground at Z 0, ~1 quad per metre
meshTerrainCreate | call Create("Valley", [0, 0, 200], 2000, 1000, 1000)        2 x 1 km, 1000 x 500 quads
meshTerrainCreate | call ImportHeightmap("Alps", "Heightmaps/alps.png", [0, 0, 0], 2000, 100)
meshTerrainCreate | call Delete(unreal.find("Island"))                         with its base meshes and modifiers
```

`Create(label, location, sizeMeters, sizeYMeters = square, resolution = quads along X, definition, timeoutSeconds)`:
10 .. 20000 m, at most 2000 x 2000 quads (sections of at most 500 x 500). The material, priority layers and paint channels
come from the definition (default `/MeshPartition/DataAssets/MPD_Default`): pass another `MeshPartitionDefinition` asset path
for another material. `ImportHeightmap(label, file, location, sizeMeters, heightScale = 100, resolution, definition,
timeoutSeconds)` reads a 16-bit grayscale `.png` or a `.r16` / `.raw` (little-endian, square) in Landscape units: value 32768
is `location` Z and one step is `heightScale / 128` cm (a Landscape's Z scale; 100 gives -256 .. +256 m); one quad per pixel
step (at most 2048 per side), image row 0 at the -Y edge, Y size from the image's aspect. Place terrains side by side, not
overlapping: Play In Editor's section build then warns about the other terrain's actors. Save the level before Play In Editor:
the engine asks about unsaved Mesh Terrain base meshes otherwise.

An island with the sea all around: Mesh Terrain has no water, so the sea is a height, world Z 0 (sea level) by convention.
Create the terrain larger than the island with its ground on the seabed, raise the land with a Patch, then shape the
mountains and hills on top (modifiers added later apply on top of the Patch):

```
meshTerrainCreate | call Create("Island", [0, 0, -2000], 2400)                                    2.4 km of seabed at -20 m
meshTerrainModifier | call AddPatch(unreal.find("Island"), [0, 0, 200], 90000, 15000, "Land")    land at +2 m out to 900 m, a 150 m beach slope to the seabed
editor | call SpawnActor({ClassName: "/Engine/BasicShapes/Plane", Label: "Sea", Location: [0, 0, 0], Scale: [2400, 2400, 1]})   optional sea surface
```

A Patch's Radius is its flat part and its Falloff the slope beyond (both cm): centred on the terrain, keep Radius + Falloff
under half its shorter side (90000 + 15000 < 120000 cm), so a ring of seabed stays all around. The plane is only a flat
surface at sea level (a Water plugin ocean looks better where the project has one); it blocks traces and walking at Z 0:
read the ground with `Surface`. Tell the user the sea level and what the sea is.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIMeshTerrainRqlModule>, UnrealAIMeshTerrain)
