// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIMeshTerrainWaterCommon.h"
#include "UnrealAIMeshTerrainWaterRqlApi.h"

#include "LandscapeSettings.h"
#include "Materials/MaterialInterface.h"

#include "MeshPartition.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionWaterModifier.h"

#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterSplineComponent.h"
#include "WaterZoneActor.h"

namespace UnrealAIMeshTerrainWaterModuleLocal
{
	using namespace UnrealAIMeshTerrainWater;

	TArray<UObject*> Objects(const TArray<AWaterBody*>& Bodies)
	{
		return TArray<UObject*>(Bodies);
	}

	TSharedPtr<FJsonValue> StatusJson(UObject* Object)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		const AWaterBody* Body = Cast<AWaterBody>(Object);
		const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		if (!Component)
			return MakeShared<FJsonValueObject>(Out);
		const UWaterModifier* Modifier = MatchingModifier(Body);
		const AMeshPartition* Terrain = Modifier ? Modifier->GetAffectedMeshPartition() : nullptr;
		const UWaterSplineComponent* Spline = Component->GetWaterSpline();
		const UMaterialInterface* Material = Component->GetWaterMaterial();
		const AWaterZone* Zone = Component->GetWaterZone();
		const FString Issue = Problem(Body);
		Out->SetStringField(TEXT("Type"), WaterType(Body));
		Out->SetStringField(TEXT("Terrain"), IsValid(Terrain) ? Terrain->GetActorLabel() : FString());
		Out->SetStringField(TEXT("Modifier"), Modifier ? Modifier->GetClass()->GetName() : FString());
		Out->SetNumberField(TEXT("Modifiers"), WaterModifiers(Body).Num());
		Out->SetBoolField(TEXT("AffectsTerrain"), Component->bAffectsLandscape);
		Out->SetNumberField(TEXT("SurfaceZ"), FMath::RoundToDouble(Body->GetActorLocation().Z));
		Out->SetNumberField(TEXT("ChannelDepth"), FMath::RoundToDouble(Component->CurveSettings.ChannelDepth));
		Out->SetNumberField(TEXT("SplinePoints"), Spline ? Spline->GetNumberOfSplinePoints() : 0);
		Out->SetStringField(TEXT("Material"), Material ? Material->GetName() : FString());
		Out->SetStringField(TEXT("Zone"), Zone ? Zone->GetActorLabel() : FString());
		Out->SetBoolField(TEXT("Ready"), Issue.IsEmpty());
		Out->SetStringField(TEXT("Problem"), Issue);
		return MakeShared<FJsonValueObject>(Out);
	}
}

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Water bodies on Mesh Terrains (UE 5.8 MeshPartitionWater,
 * Experimental). Reflection exposes the water body actors and the water modifier components ("LakeModifier",
 * "RiverModifier"); the module adds a world source, the operations, the body status and the agent guide.
 */
class FUnrealAIMeshTerrainWaterRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("MeshTerrainWater"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-mesh-terrain-water"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAIMeshTerrainWater")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		using namespace UnrealAIMeshTerrainWaterModuleLocal;
		UClass* BodyClass = AWaterBody::StaticClass();
		UClass* TerrainClass = AMeshPartition::StaticClass();

		Context.AddObjectListSource(TEXT("meshTerrainWaterBodies"),
			TEXT("the Water body actors (lake, river, ocean, custom) of the editor world, with the Mesh Terrain each shapes (on a World Partition map all of them: it loads the unloaded ones)"),
			BodyClass, []() { return Objects(WaterBodies(/*bLoad*/ true)); });
		Context.AddApiSource(TEXT("meshTerrainWater"),
			TEXT("water on Mesh Terrain: call CreateLake(terrain, label, center, radius, depth) / CreateRiver(terrain, label, points, width, depth) / CreateOcean(label, height) / Attach(body, terrain) / Detach(body) / Delete(body) / WaitForBuild(body) / SetZone / SetMaterial / EnableMeshTerrainWater; each waits until the terrain is rebuilt"),
			UUnrealAIMeshTerrainWaterRqlApi::StaticClass(), TEXT("MeshTerrainWaterOperations"));

		Context.AddJsonMember(BodyClass, TEXT("MeshTerrainWater"),
			TEXT("the body on Mesh Terrain: Type, Terrain (the label of the terrain its modifier shapes), Modifier (LakeModifier, RiverModifier), Modifiers (water modifiers on it), AffectsTerrain, SurfaceZ, ChannelDepth, SplinePoints, Material, Zone, Ready (it shapes its terrain, the terrain is built and the water renders) and Problem (why not, with what to call)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue> { return StatusJson(Object); });
		Context.AddObjectMember(BodyClass, TEXT("TerrainModifier"), TEXT("the body's Mesh Partition water modifier of its kind (null when none)"),
			UE::MeshPartition::UModifierComponent::StaticClass(), [](UObject* Object) -> UObject*
			{
				return UnrealAIMeshTerrainWater::MatchingModifier(Cast<AWaterBody>(Object));
			});
		Context.AddObjectListMember(TerrainClass, TEXT("WaterBodies"), TEXT("the lakes and rivers whose water modifier shapes the terrain"),
			BodyClass, [](UObject* Object)
			{
				return UnrealAIMeshTerrainWaterModuleLocal::Objects(UnrealAIMeshTerrainWater::BodiesOf(Cast<AMeshPartition>(Object), /*bLoad*/ true));
			});

		Context.AddGuide(TEXT("Water on Mesh Terrain (Experimental, UE 5.8+)"), TEXT(R"MD(Oceans, lakes and rivers of the Water plugin on a Mesh Terrain (Unreal 5.8's
experimental Mesh Partition Water). A lake or river carries a Mesh Partition water modifier of its kind (`LakeModifier`,
`RiverModifier`) bound to the terrain: it shapes the ground to the water (the bed `ChannelDepth` below the surface, the shore
blended out to the ground around it). Create them with `meshTerrainWater`, not with `editor | call SpawnActor("WaterBody...")`:
a body placed that way shapes nothing until it is attached. Each call is one undo step and returns when the terrain is
rebuilt: `{Body, Label, Type, Terrain, Modifier, Location, Ready, Seconds, State, Problem}`.

```
meshTerrainWater | call CreateLake(unreal.find("Island"), "Lake", [12000, -8000, 300], 3000)          Z is the water surface; Radius cm
meshTerrainWater | call CreateLake(unreal.find("Island"), "Pond", [0, 0, 150], 1500, 400)             Depth: the bed 4 m below the surface
meshTerrainWater | call CreateRiver(unreal.find("Island"), "River", [[-20000, 0, 600], [0, 3000, 300], [20000, 0, 0]], 1500, 300)   upstream first; Width, Depth cm
meshTerrainWater | call CreateOcean("Ocean", 0)                                                       sea level Z 0, fills the water zone
meshTerrainWater | call Attach(unreal.find("WaterBodyLake"), unreal.find("Island"))                   a lake or river placed another way
meshTerrainWater | call Delete(unreal.find("Pond"))                                                   the ground under it comes back
meshTerrainWaterBodies | select Label MeshTerrainWater                                               Ready / Problem: shapes, renders, or what to call
meshTerrains | first | get WaterBodies | select Label
```

Read the ground first (`meshTerrain | call Surface(terrain, point)`, Mesh Terrain extension) and put the water surface at it
or a little below: the modifier sets the ground around the water to the surface height, so a lake whose surface is far above
the ground raises a plateau with the lake on top, and one far below digs a pit. A river's points are its surface: keep each a
little below the ground there and flowing downhill (upstream first); its bed is carved `Depth` below. The water bodies are
also modifiers of the terrain (`meshTerrainModifiers | where Kind == "Lake"`), applied after the terrain's other layers:
sculpt and add hills first, add water last; after changing the ground under water call `meshTerrainWater | call
WaitForBuild(body)`.

The ocean needs no modifier: its surface is a height, and ground above it hides it. For an island make the terrain larger
than the island with its ground on the seabed below the ocean, raise the land above it with a Patch, then `CreateOcean`
(`meshTerrainModifier | call AddPatch(...)`, see the Mesh Terrain guide). Never give an ocean a modifier: the engine's
experimental OceanModifier can override the ground other modifiers shaped under it (a lake or hill vanishes); `Problem`
reports one and `Detach(body)` removes it. One ocean per world; `SetZone()` sizes the water zone over every Mesh Terrain (+25%).

`Problem` names what to call: `Attach` (no modifier, bound to no terrain, disabled, Affects Landscape off),
`SetMaterial` (no or not a water material), `SetZone` (no water zone), `WaitForBuild` (the terrain still builds). The first
water material compiles its shaders for a while; `Ready` turns true when they are done. Under -nullrhi the water does not
render (the terrain is still shaped). Needs Unreal Engine 5.8+ with the MeshPartitionWater, Water and MeshPartition plugins
(`EnableMeshTerrainWater()` writes them into the project file); on older engines this extension cannot be installed.)MD"));
	}
};

/**
 * The RQL module, plus no modal dialog from the Water plugin while it is loaded: on a level that also has a Landscape, a
 * water body that affects the ground makes the Water editor ask the Landscape editor for a "Water" edit layer, which opens
 * the modal "Insert New Landscape Edit Layer" dialog and blocks every tool call until a human closes it. The setting is
 * off in memory (never saved) while the module is loaded; the layer is still created.
 */
class FUnrealAIMeshTerrainWaterEditorModule : public TUnrealMcpRqlEditorModule<FUnrealAIMeshTerrainWaterRqlModule>
{
public:
	virtual void StartupModule() override
	{
		TUnrealMcpRqlEditorModule::StartupModule();
		ULandscapeSettings* Settings = GetMutableDefault<ULandscapeSettings>();
		bPreviousDialog = Settings->bShowDialogForAutomaticLayerCreation;
		Settings->bShowDialogForAutomaticLayerCreation = false;
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized())
			GetMutableDefault<ULandscapeSettings>()->bShowDialogForAutomaticLayerCreation = bPreviousDialog;
		TUnrealMcpRqlEditorModule::ShutdownModule();
	}

private:
	bool bPreviousDialog = true;
};

IMPLEMENT_MODULE(FUnrealAIMeshTerrainWaterEditorModule, UnrealAIMeshTerrainWater)
