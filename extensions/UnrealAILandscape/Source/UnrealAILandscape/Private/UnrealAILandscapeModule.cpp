// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAILandscapeRqlApi.h"

#include "Engine/World.h"
#include "EngineUtils.h"               // TActorIterator
#include "Landscape.h"                 // ALandscape (the primary landscape actor)
#include "LandscapeInfo.h"             // ULandscapeInfo + FLandscapeInfoLayerSettings (paint layers)
#include "LandscapeProxy.h"            // ALandscapeProxy (+ streaming proxies)
#include "Materials/MaterialInterface.h"
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
				// Every proxy's collision heightfield (on a World Partition map the primary actor has no components of its own).
				FBox Box(ForceInit);
				const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Object);
				if (ULandscapeInfo* Info = Proxy ? Proxy->GetLandscapeInfo() : nullptr)
				{
					for (const TPair<FIntPoint, ULandscapeHeightfieldCollisionComponent*>& Pair : Info->XYtoCollisionComponentMap)
					{
						if (Pair.Value)
							Box += Pair.Value->Bounds.GetBox();
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
			TEXT("water body summary: Type, SplinePoints"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				const AWaterBody* Body = Cast<AWaterBody>(Object);
				if (UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr)
				{
					const UWaterSplineComponent* Spline = Component->GetWaterSpline();
					Out->SetStringField(TEXT("Type"), WaterBodyTypeName(Component->GetWaterBodyType()));
					Out->SetNumberField(TEXT("SplinePoints"), Spline ? Spline->GetNumberOfSplinePoints() : 0);
				}
				return MakeShared<FJsonValueObject>(Out);
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

`Height` is the world Z (cm) traces and collision hit, `Slope` degrees (0 flat), `Location` a spawn point on the ground
(spawned meshes have their pivot there: lift by half the mesh height when the pivot is centred). A point off the landscape
gives `Inside: false`. A trace from above also hits your own actors (probes, roads, trees): `Surface` reads only the
ground. From a distance the editor draws the landscape at a lower LOD, so a thin mesh laid exactly on the ground (a
road ribbon) can look buried in far views while it sits right up close: sample `Surface` every metre or less along it
and lift it 5 to 10 cm, or paint the road as a layer instead. `call Rebuild(unreal.find("Terrain"))` merges a landscape
edited elsewhere (Landscape mode, Python) again and rebuilds its collision and Nanite mesh.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAILandscapeRqlModule>, UnrealAILandscape)
