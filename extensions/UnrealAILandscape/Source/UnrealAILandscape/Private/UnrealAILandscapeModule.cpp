// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"

#include "Editor.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAILandscape, Log, All);

namespace UnrealAILandscapeRqlLocal
{
	/** Every actor of class T in the editor world (the persistent level and loaded sublevels). */
	template <typename T>
	TArray<UObject*> EditorActors()
	{
		TArray<UObject*> Out;
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
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
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName (scripts/extension_release.py bump updates both).
 */
class FUnrealAILandscapeRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("Landscape"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-landscape"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.1"); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		using namespace UnrealAILandscapeRqlLocal;
		Context.AddObjectListSource(TEXT("landscapes"), TEXT("the Landscape actors and streaming proxies of the editor world"),
			[]() { return EditorActors<ALandscapeProxy>(); });
		Context.AddObjectListSource(TEXT("waterBodies"), TEXT("the Water body actors (river, lake, ocean, custom) of the editor world"),
			[]() { return EditorActors<AWaterBody>(); });
		Context.AddObjectListSource(TEXT("waterZones"), TEXT("the Water zone actors of the editor world"),
			[]() { return EditorActors<AWaterZone>(); });

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
		Context.AddJsonMember(AWaterBody::StaticClass(), TEXT("Water"),
			TEXT("water body summary: Type, SplinePoints, Material, Zone"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				const AWaterBody* Body = Cast<AWaterBody>(Object);
				if (UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr)
				{
					const UWaterSplineComponent* Spline = Component->GetWaterSpline();
					const UMaterialInterface* Material = Component->GetWaterMaterial();
					const AWaterZone* Zone = Component->GetWaterZone();
					Out->SetStringField(TEXT("Type"), WaterBodyTypeName(Component->GetWaterBodyType()));
					Out->SetNumberField(TEXT("SplinePoints"), Spline ? Spline->GetNumberOfSplinePoints() : 0);
					Out->SetStringField(TEXT("Material"), Material ? Material->GetPathName() : FString());
					Out->SetStringField(TEXT("Zone"), Zone ? Zone->GetActorNameOrLabel() : FString());
				}
				return MakeShared<FJsonValueObject>(Out);
			});

		Context.AddGuide(TEXT("Landscape and Water"), TEXT(R"MD(Landscapes, water bodies and water zones are actors of the editor world: `landscapes`, `waterBodies`, `waterZones`
(the same actors as `actors | where $ is LandscapeProxy`). Their reflected properties are members (LandscapeMaterial,
the water body's components, the zone's ZoneExtent, ...); `Sections`, `PaintLayers` and `Water` summarise what is not.

```
landscapes | select $ref Label Sections PaintLayers LandscapeMaterial
waterBodies | select Label Water Location
waterZones | select Label Location ZoneExtent
landscapes | where Sections.IsPrimary | set LandscapeMaterial = "/Game/Materials/M_Terrain"
```

Sculpting and painting are not RQL operations; edit landscape heights and layers in Landscape mode.)MD"));
	}
};

/** Editor module that owns the RQL module and registers it as a modular feature, so Unreal-MCP discovers it. */
class FUnrealAILandscapeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RqlModule = MakeUnique<FUnrealAILandscapeRqlModule>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
		UE_LOG(LogUnrealAILandscape, Log, TEXT("[UnrealAILandscape] registered RQL module '%s'."), *RqlModule->GetRqlModuleName());
	}

	virtual void ShutdownModule() override
	{
		if (RqlModule.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
			RqlModule.Reset();
		}
	}

private:
	TUniquePtr<FUnrealAILandscapeRqlModule> RqlModule;
};

IMPLEMENT_MODULE(FUnrealAILandscapeModule, UnrealAILandscape)
