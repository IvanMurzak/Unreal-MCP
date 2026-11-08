// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAILandscapeRqlApi.h"
#include "UnrealAIWaterRqlApi.h"
#include "UnrealAIWaterSafety.h"

#include "Engine/World.h"
#include "EngineUtils.h"               // TActorIterator
#include "Landscape.h"                 // ALandscape (the primary landscape actor)
#include "LandscapeHeightfieldCollisionComponent.h" // the collision heightfields (Bounds)
#include "LandscapeInfo.h"             // ULandscapeInfo + FLandscapeInfoLayerSettings (paint layers)
#include "LandscapeProxy.h"            // ALandscapeProxy (+ streaming proxies)
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "WaterBodyActor.h"            // AWaterBody
#include "WaterBodyComponent.h"        // UWaterBodyComponent + EWaterBodyType
#include "WaterBodyTypes.h"            // EWaterBodyType
#include "WaterSplineComponent.h"      // UWaterSplineComponent (a USplineComponent)
#include "WaterZoneActor.h"            // AWaterZone

namespace UnrealAILandscapeRqlLocal
{
	/** Every actor of class T in the editor world (the persistent level and loaded sublevels). */
	template <typename T>
	TArray<UObject*> EditorActors()
	{
		TArray<UObject*> Out;
		UWorld* World = UnrealMcpRql::EditorWorld();
		if (!World)
			return Out;
		for (TActorIterator<T> It(World); It; ++It)
		{
			if (IsValid(*It))
				Out.Add(*It);
		}
		return Out;
	}

	/**
	 * Why a water body does not show yet, with what to call (empty when it is ready): the readback after the water
	 * operations and for bodies placed another way.
	 */
	FString WaterProblem(const AWaterBody* Body)
	{
		const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		if (!Component)
			return TEXT("it has no water body component");
		const auto Find = [Body]() { return FString::Printf(TEXT("unreal.find(\"%s\")"), *Body->GetActorLabel()); };
		const UMaterialInterface* Material = Component->GetWaterMaterial();
		if (!Material)
			return FString::Printf(TEXT("no water material, so it is invisible: water | call SetMaterial(%s) gives it the Water defaults"), *Find());
		if (!UnrealAIWaterSafety::IsWaterMaterial(Material))
			return FString::Printf(TEXT("its WaterMaterial %s is not a water material: water | call SetMaterial(%s)"), *Material->GetName(), *Find());
		if (Component->GetWaterBodyType() != EWaterBodyType::Transition && !Component->GetWaterZone())
			return TEXT("no water zone renders it: water | call SetZone()");
		if (!FApp::CanEverRender())
			return TEXT("this editor does not render (-nullrhi): the water mesh is built in a rendering editor");
		bool bMesh = false;
		TArray<UStaticMeshComponent*> Meshes;
		Body->GetComponents(Meshes);
		for (const UStaticMeshComponent* Mesh : Meshes)
			bMesh |= Mesh->GetFName() == TEXT("WaterInfoMeshComponent") && Mesh->GetStaticMesh() != nullptr;
		if (!bMesh && Component->ShouldGenerateWaterMeshTile())
			return FString::Printf(TEXT("its water mesh is not built: water | call SetMaterial(%s, \"%s\") rebuilds it"), *Find(), *Material->GetPathName());
		if (Material->IsCompiling())
			return TEXT("its material's shaders are still compiling: it shows when they finish (read Water again)");
		return FString();
	}

	const TCHAR* WaterBodyTypeName(EWaterBodyType Type)
	{
		switch (Type)
		{
		case EWaterBodyType::River:      return TEXT("River");
		case EWaterBodyType::Lake:       return TEXT("Lake");
		case EWaterBodyType::Ocean:      return TEXT("Ocean");
		case EWaterBodyType::Transition: return TEXT("Custom");
		default:                         return TEXT("Unknown");
		}
	}
}

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Landscape and Water are queried with rql-query
 * instead of dedicated tools. Reflection exposes the landscape, water-body and water-zone actors and their components;
 * the module adds world sources for them, the landscape section layout and paint layers and the water-body summary (not
 * reflected properties), and the agent guide rendered into the unreal-rql skill.
 *
 * The version is the .uplugin VersionName.
 */
class FUnrealAILandscapeRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("Landscape"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-landscape"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAILandscape")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		using namespace UnrealAILandscapeRqlLocal;
		Context.AddObjectListSource(TEXT("landscapes"), TEXT("the Landscape actors and streaming proxies of the editor world"),
			ALandscapeProxy::StaticClass(), []() { return EditorActors<ALandscapeProxy>(); });
		Context.AddObjectListSource(TEXT("waterBodies"), TEXT("the Water body actors (river, lake, ocean, custom) of the editor world"),
			AWaterBody::StaticClass(), []() { return EditorActors<AWaterBody>(); });
		Context.AddObjectListSource(TEXT("waterZones"), TEXT("the Water zone actors of the editor world"),
			AWaterZone::StaticClass(), []() { return EditorActors<AWaterZone>(); });

		Context.AddApiSource(TEXT("landscape"),
			TEXT("Landscape operations: call Create(label, location, sizeMeters, material) / Sculpt(landscape, center, radius, height) / Paint(landscape, layer, center, radius, strength) / CreateLayerMaterial(path, layers) / Surface(landscape, point) / Rebuild(landscape)"),
			UUnrealAILandscapeRqlApi::StaticClass(), TEXT("LandscapeOperations"));
		Context.AddApiSource(TEXT("water"),
			TEXT("Water operations: call CreateOcean(label, height, material) / CreateLake(label, center, radius, material) / CreateRiver(label, points, width, material) / CreateRiverBetween(label, landscape, source, mouth, width, depth, material) / SetMaterial(body, material) / SetZone(sizeMeters, center) / EnableWater()"),
			UUnrealAIWaterRqlApi::StaticClass(), TEXT("WaterOperations"));

		// The fields queries filter on are scalar members; `Sections` keeps the layout together for select output.
		Context.AddJsonMember(ALandscapeProxy::StaticClass(), TEXT("IsPrimary"), TEXT("true for the landscape actor, false for a streaming proxy"),
			[](UObject* Object) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueBoolean>(Cast<ALandscape>(Object) != nullptr); });
		Context.AddJsonMember(ALandscapeProxy::StaticClass(), TEXT("ComponentCount"), TEXT("how many landscape components the actor has"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Object);
				return MakeShared<FJsonValueNumber>(Proxy ? Proxy->LandscapeComponents.Num() : 0);
			});
		Context.AddJsonMember(ALandscapeProxy::StaticClass(), TEXT("Sections"),
			TEXT("section layout: IsPrimary, ComponentCount, ComponentSizeQuads, SubsectionSizeQuads, NumSubsections"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				if (const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Object))
				{
					Out->SetBoolField(TEXT("IsPrimary"), Cast<ALandscape>(Proxy) != nullptr);
					Out->SetNumberField(TEXT("ComponentCount"), Proxy->LandscapeComponents.Num());
					Out->SetNumberField(TEXT("ComponentSizeQuads"), Proxy->ComponentSizeQuads);
					Out->SetNumberField(TEXT("SubsectionSizeQuads"), Proxy->SubsectionSizeQuads);
					Out->SetNumberField(TEXT("NumSubsections"), Proxy->NumSubsections);
				}
				return MakeShared<FJsonValueObject>(Out);
			});
		Context.AddJsonMember(ALandscapeProxy::StaticClass(), TEXT("Bounds"),
			TEXT("the whole landscape's world box (cm): Min [X, Y, Z], Max [X, Y, Z] (Z: the lowest and highest ground), SizeMeters [X, Y]"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				// The loaded collision heightfields' local boxes span the ground (ULandscapeInfo::GetCompleteBounds was empty for a
				// landscape created in the session; on a World Partition map the primary actor has no components of its own). The
				// engine pads a flat component's box by one unit up and down: undo it, so Z is the real lowest and highest ground.
				FBox Box(ForceInit);
				const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Object);
				if (const ULandscapeInfo* Info = Proxy ? Proxy->GetLandscapeInfo() : nullptr)
				{
					for (const TPair<FIntPoint, ULandscapeHeightfieldCollisionComponent*>& Pair : Info->XYtoCollisionComponentMap)
					{
						if (!Pair.Value)
							continue;
						FBox Local = Pair.Value->CachedLocalBox;
						if (FMath::IsNearlyEqual(Local.GetExtent().Z, 1.0))
							Local.Min.Z = Local.Max.Z = Local.GetCenter().Z;
						Box += Local.TransformBy(Pair.Value->GetComponentTransform());
					}
				}
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				if (!Box.IsValid)
					return MakeShared<FJsonValueObject>(Out);
				const auto Vector = [](std::initializer_list<double> Values)
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					for (double Value : Values)
						Items.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Value * 10.0) / 10.0));
					return Items;
				};
				Out->SetArrayField(TEXT("Min"), Vector({ Box.Min.X, Box.Min.Y, Box.Min.Z }));
				Out->SetArrayField(TEXT("Max"), Vector({ Box.Max.X, Box.Max.Y, Box.Max.Z }));
				Out->SetArrayField(TEXT("SizeMeters"), Vector({ (Box.Max.X - Box.Min.X) / 100.0, (Box.Max.Y - Box.Min.Y) / 100.0 }));
				return MakeShared<FJsonValueObject>(Out);
			});
		Context.AddJsonMember(ALandscapeProxy::StaticClass(), TEXT("PaintLayers"), TEXT("the names of the landscape's paint layers"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Layers;
				const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Object);
				if (ULandscapeInfo* Info = Proxy ? Proxy->GetLandscapeInfo() : nullptr)
				{
					for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
						Layers.Add(MakeShared<FJsonValueString>(Layer.GetLayerName().ToString()));
				}
				return MakeShared<FJsonValueArray>(Layers);
			});
		Context.AddJsonMember(AWaterBody::StaticClass(), TEXT("WaterType"), TEXT("the water body type: River, Lake, Ocean or Custom"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const AWaterBody* Body = Cast<AWaterBody>(Object);
				const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
				return MakeShared<FJsonValueString>(Component ? WaterBodyTypeName(Component->GetWaterBodyType()) : TEXT("Unknown"));
			});
		Context.AddJsonMember(AWaterBody::StaticClass(), TEXT("Water"),
			TEXT("water body status: Type, SplinePoints, Material, AffectsLandscape, Ready (it renders) and Problem (why not, with what to call)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				const AWaterBody* Body = Cast<AWaterBody>(Object);
				if (UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr)
				{
					const UWaterSplineComponent* Spline = Component->GetWaterSpline();
					const UMaterialInterface* Material = Component->GetWaterMaterial();
					const FString Problem = WaterProblem(Body);
					Out->SetStringField(TEXT("Type"), WaterBodyTypeName(Component->GetWaterBodyType()));
					Out->SetNumberField(TEXT("SplinePoints"), Spline ? Spline->GetNumberOfSplinePoints() : 0);
					Out->SetStringField(TEXT("Material"), Material ? Material->GetName() : FString());
					Out->SetBoolField(TEXT("AffectsLandscape"), Component->bAffectsLandscape);
					Out->SetBoolField(TEXT("Ready"), Problem.IsEmpty());
					Out->SetStringField(TEXT("Problem"), Problem);
				}
				return MakeShared<FJsonValueObject>(Out);
			});
		Context.AddJsonMember(AWaterBody::StaticClass(), TEXT("Path"),
			TEXT("the water body's spline points in world space (cm), [X, Y, Z] each; Z is the water surface"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Points;
				const AWaterBody* Body = Cast<AWaterBody>(Object);
				if (const UWaterSplineComponent* Spline = Body ? Body->GetWaterSpline() : nullptr)
				{
					for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
					{
						const FVector Point = Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World);
						TArray<TSharedPtr<FJsonValue>> Xyz;
						for (const double Value : { Point.X, Point.Y, Point.Z })
							Xyz.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Value)));
						Points.Add(MakeShared<FJsonValueArray>(Xyz));
					}
				}
				return MakeShared<FJsonValueArray>(Points);
			});
		Context.AddObjectMember(AWaterBody::StaticClass(), TEXT("WaterMaterial"), TEXT("the water body's water material"),
			UMaterialInterface::StaticClass(), [](UObject* Object) -> UObject*
			{
				const AWaterBody* Body = Cast<AWaterBody>(Object);
				const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
				return Component ? Component->GetWaterMaterial() : nullptr;
			});
		Context.AddObjectMember(AWaterBody::StaticClass(), TEXT("WaterZone"), TEXT("the water zone that renders the body"),
			AWaterZone::StaticClass(), [](UObject* Object) -> UObject*
			{
				const AWaterBody* Body = Cast<AWaterBody>(Object);
				const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
				return Component ? Component->GetWaterZone() : nullptr;
			});

		Context.AddGuide(TEXT("Landscape and Water"), TEXT(R"MD(Landscapes, water bodies and water zones are actors of the editor world: `landscapes`, `waterBodies`, `waterZones`
(the same actors as `actors | where $ is LandscapeProxy`). Their reflected properties are members (LandscapeMaterial,
the water body's components, the zone's ZoneExtent, ...); `IsPrimary`, `ComponentCount`, `PaintLayers`, `WaterType`,
`WaterMaterial` and `WaterZone` add what is not, `Sections` and `Water` summarise it.

```
landscapes | select $ref Label Sections PaintLayers LandscapeMaterial
waterBodies | where WaterType == "River" | select Label Water WaterZone Location
waterZones | select Label Location ZoneExtent
landscapes | where IsPrimary | set LandscapeMaterial = "/Game/Materials/M_Terrain"
```

Create, sculpt and paint with the `landscape` operations (each call is one undo step):

```
landscape | call CreateLayerMaterial("/Game/Landscape/M_Terrain", "Grass #3a7d2c, Rock #7a7a7a")    a material blending two paint layers
landscape | call Create("Terrain", [0, 0, 0], 100, "/Game/Landscape/M_Terrain")                      100 x 100 m, flat, centred on the location
[[2000, 1500, 1200, 600], [-1800, -900, 1500, 900]] | call landscape.Sculpt(unreal.find("Terrain"), [$[0], $[1], 0], $[2], $[3])    hills: center X Y, radius, height (cm)
landscape | call Paint(unreal.find("Terrain"), "Grass", [0, 0, 0], 0)                                 a whole-landscape base layer (radius 0)
[[2000, 1500], [-1800, -900]] | call landscape.Paint(unreal.find("Terrain"), "Rock", [$[0], $[1], 0], 1000)   rock on the hilltops
landscapes | select Label PaintLayers Sections
```

Heights are in cm (negative Height digs a hollow); a paint layer shows when the material blends a layer of that name.
Created materials and layer infos are unsaved: `editor | call SaveAll()`.

Read the ground back with `Surface` (rql-mutate, read-only) and `Bounds` instead of line traces: Create, Sculpt and
Paint return with the landscape merged, so its collision, `Surface` and the rendered surface agree at once.

```
landscapes | where IsPrimary | select Label Bounds                                                 world box: Min/Max [X, Y, Z] cm, SizeMeters
landscape | call Surface(unreal.find("Terrain"), [2000, 1500, 0])                                   {Inside, Height, Slope, Location, Normal}
range(0, 100) | call landscape.Surface(unreal.find("Terrain"), [($ % 10) * 1000 - 4500, floor($ / 10) * 1000 - 4500, 0])   a 10 x 10 grid
```

`Height` is the world Z (cm) that traces and collision hit, `Slope` degrees (0 flat), `Location` a spawn point on the
ground; off the landscape `Inside` is false. Traces from above also hit your own actors (probes, roads, trees);
`Surface` reads only the ground. Far views draw the landscape at a lower LOD, so a thin mesh laid on the ground (a road)
can look buried from afar: sample `Surface` every metre along it and lift it 5-10 cm, or paint the road as a layer.
`call Rebuild(unreal.find("Terrain"))` re-merges a landscape edited elsewhere (Landscape mode, Python) and rebuilds its
collision and Nanite mesh.

Water: create oceans, lakes and rivers with the `water` operations rather than `editor | call SpawnActor("WaterBody...")`.
They place bodies like the Place Actors panel (material, waves), size them for the landscape, never edit the landscape
through the Water brush (no "Water" edit layer, no slow GPU brush pass), build the water mesh before returning and
create the water zone when there is none. (A body placed with SpawnActor or Python gets the Water defaults on the next
tick, and its "Water" edit layer is made without the engine's modal dialog.)

```
water | call CreateOcean("Ocean", 0)                                                    sea level Z 0 around the island (fills the water zone)
water | call CreateLake("Lake", [12000, -8000, 2400], 3000)                             Z of the center is the water surface
water | call CreateRiverBetween("River", unreal.find("Terrain"), [-20000, 30000, 0], [60000, -90000, 0], 1500, 300)
water | call CreateRiver("Creek", [[0, 0, 900], [4000, 1000, 700], [9000, 0, 400]], 800)  your own points, upstream first
waterBodies | select Label Water Path                                                  Water.Ready / Water.Problem: renders or why not
water | call SetMaterial(unreal.find("Lake"), "/Water/Materials/WaterSurface/Water_Material")
water | call SetZone()                                                                  zone over every landscape (+25%), or SetZone(3000, [0, 0, 0])
water | call EnableWater()                                                              Water in the project file, for packaged games
```

Land above the ocean's height hides it, so one ocean surrounds an island. `CreateRiverBetween` follows the lowest
ground from source to mouth, keeps the water flowing downhill and (Depth > 0, cm) carves its bed into the landscape, so
it shows even across a rise; keep the mouth a little inside the landscape. A water body's surface is its spline's Z: a
lake or river below the ground around it is hidden. `Water.Problem` names what to call when a body does not render.
The first water material compiles its shaders for a while after the call; `Water.Ready` turns true when they are done.)MD"));
	}
};

/** The RQL module plus the Water modal guard (UnrealAIWaterSafety) for the module's lifetime. */
class FUnrealAILandscapeEditorModule : public TUnrealMcpRqlEditorModule<FUnrealAILandscapeRqlModule>
{
public:
	virtual void StartupModule() override
	{
		TUnrealMcpRqlEditorModule::StartupModule();
		UnrealAIWaterSafety::Start();
	}

	virtual void ShutdownModule() override
	{
		UnrealAIWaterSafety::Stop();
		TUnrealMcpRqlEditorModule::ShutdownModule();
	}
};

IMPLEMENT_MODULE(FUnrealAILandscapeEditorModule, UnrealAILandscape)
