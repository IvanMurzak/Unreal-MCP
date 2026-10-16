// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

// --- Editor / world APIs the tools wrap ------------------------------------------------------
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"               // TActorIterator
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

// --- Landscape (built-in engine module) ------------------------------------------------------
#include "Landscape.h"                 // ALandscape (the primary landscape actor)
#include "LandscapeProxy.h"            // ALandscapeProxy (+ streaming proxies)
#include "LandscapeInfo.h"             // ULandscapeInfo + FLandscapeInfoLayerSettings (paint layers)

// --- Water (the gating engine plugin) --------------------------------------------------------
#include "WaterBodyActor.h"            // AWaterBody
#include "WaterBodyComponent.h"        // UWaterBodyComponent + EWaterBodyType
#include "WaterBodyTypes.h"            // EWaterBodyType
#include "WaterSplineComponent.h"      // UWaterSplineComponent (a USplineComponent)
#include "WaterZoneActor.h"            // AWaterZone

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAILandscape, Log, All);

// =================================================================================================
//  File-local helpers. The module is UNITY-built (every .cpp concatenated into one TU), so an
//  anonymous namespace does NOT make a helper file-private — every helper is given a MODULE-UNIQUE
//  `UnrealAILandscape_` prefix to avoid a same-name ODR collision with the spec's helpers.
// =================================================================================================
namespace
{
	// Resolve the active editor world, or null when no editor / no world is available.
	UWorld* UnrealAILandscape_GetEditorWorld()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		return GEditor->GetEditorWorldContext().World();
	}

	// Map an EWaterBodyType to its LLM-facing label (matches the editor's "Custom" display name).
	const TCHAR* UnrealAILandscape_WaterBodyTypeToString(EWaterBodyType Type)
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

	// The friendly outliner name in the editor (label), falling back to the internal name otherwise.
	FString UnrealAILandscape_ActorName(const AActor* Actor)
	{
		return Actor ? Actor->GetActorNameOrLabel() : FString();
	}

	// True when `Query` matches the actor's editor label OR its internal object name (case-insensitive).
	bool UnrealAILandscape_ActorMatches(const AActor* Actor, const FString& Query)
	{
		if (!Actor)
		{
			return false;
		}
		return Actor->GetActorNameOrLabel().Equals(Query, ESearchCase::IgnoreCase)
			|| Actor->GetName().Equals(Query, ESearchCase::IgnoreCase);
	}
}

/**
 * The extension's tool provider — an implementation of the Unreal-MCP extension contract
 * (IUnrealMcpToolProvider). It declares this extension's tools through the fluent
 * FUnrealMcpToolRegistry builder. See https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/EXTENSIONS.md.
 *
 * The family is a THIN, read-only inspection surface over the engine's built-in Landscape module and
 * the Water plugin (the gating dependency): enumerate / inspect Landscape actors and Water bodies /
 * zones in the active editor world. Every handler is DEFENSIVE — UE builds without C++ exceptions, so
 * a crash inside a handler is an editor crash; each tool validates its inputs and the engine state it
 * touches (GEditor, the editor world, a named actor, an actor's component) and returns
 * FUnrealMcpToolResult::Error(...) instead of dereferencing a null.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName — `commands/bump-version.ps1`
 * updates both atomically.
 */
class FUnrealAILandscapeProvider : public IUnrealMcpToolProvider
{
public:
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-landscape"); }
	virtual FText GetDisplayName() const override { return NSLOCTEXT("UnrealAILandscape", "DisplayName", "Unreal AI Landscape"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.0"); }

	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
	{
		// =====================================================================================
		//  Tool ids are kebab-case (^[a-z0-9]+(-[a-z0-9]+)*$), prefixed `landscape-`. Handlers run
		//  ON the game thread (the dispatcher guarantees it), so editor / engine APIs are called
		//  directly. A handler returns FUnrealMcpToolResult::Success(text, structuredJson) or
		//  ::Error(message). Tools are stamped with this provider's ExtensionId automatically.
		// =====================================================================================

		// -------------------------------------------------------------------------------------
		// landscape-list-actors — enumerate every Landscape actor / proxy in the active editor
		// world (read-only). An empty world returns count 0.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("landscape-list-actors"))
			.Title(TEXT("List Landscape Actors"))
			.Description(TEXT("Lists every Landscape actor / proxy in the active editor world (read-only). For each, "
			                  "reports its name, class, whether it is the primary ALandscape actor (vs a streaming "
			                  "proxy), and its landscape-component count. Returns { count, actors:[{ name, class, "
			                  "isPrimaryLandscape, componentCount }] }."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = UnrealAILandscape_GetEditorWorld();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world is available to inspect."));
				}

				TArray<TSharedPtr<FJsonValue>> ActorsJson;
				for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
				{
					ALandscapeProxy* Proxy = *It;
					if (!Proxy)
					{
						continue;
					}
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), UnrealAILandscape_ActorName(Proxy));
					Entry->SetStringField(TEXT("class"), Proxy->GetClass()->GetName());
					Entry->SetBoolField(TEXT("isPrimaryLandscape"), Cast<ALandscape>(Proxy) != nullptr);
					Entry->SetNumberField(TEXT("componentCount"), Proxy->LandscapeComponents.Num());
					ActorsJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), ActorsJson.Num());
				Structured->SetArrayField(TEXT("actors"), ActorsJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Landscape actor(s)."), ActorsJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// landscape-get-actor — inspect a single Landscape actor by name (read-only). Required
		// input; defensive Error on a missing name or an actor that does not exist.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("landscape-get-actor"))
			.Title(TEXT("Get Landscape Actor"))
			.Description(TEXT("Inspects a single Landscape actor in the active editor world by name (read-only). "
			                  "Reports its section layout (componentSizeQuads, subsectionSizeQuads, numSubsections), "
			                  "component count, landscape material, and paint layers. Returns { name, class, "
			                  "componentCount, componentSizeQuads, subsectionSizeQuads, numSubsections, "
			                  "landscapeMaterial, layerCount, layers:[name] }."))
			.ParamString(TEXT("name"), TEXT("The Landscape actor's editor label or object name, e.g. 'Landscape_0'."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Name = Call.GetString(TEXT("name")).TrimStartAndEnd();
				if (Name.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'name' (the Landscape actor's label, e.g. 'Landscape_0')."));
				}

				UWorld* World = UnrealAILandscape_GetEditorWorld();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world is available to inspect."));
				}

				ALandscapeProxy* Found = nullptr;
				for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
				{
					if (UnrealAILandscape_ActorMatches(*It, Name))
					{
						Found = *It;
						break;
					}
				}
				if (!Found)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Landscape actor named '%s' found in the active editor world."), *Name));
				}

				TArray<TSharedPtr<FJsonValue>> LayersJson;
				if (ULandscapeInfo* Info = Found->GetLandscapeInfo())
				{
					for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
					{
						LayersJson.Add(MakeShared<FJsonValueString>(Layer.GetLayerName().ToString()));
					}
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("name"), UnrealAILandscape_ActorName(Found));
				Structured->SetStringField(TEXT("class"), Found->GetClass()->GetName());
				Structured->SetNumberField(TEXT("componentCount"), Found->LandscapeComponents.Num());
				Structured->SetNumberField(TEXT("componentSizeQuads"), Found->ComponentSizeQuads);
				Structured->SetNumberField(TEXT("subsectionSizeQuads"), Found->SubsectionSizeQuads);
				Structured->SetNumberField(TEXT("numSubsections"), Found->NumSubsections);
				Structured->SetStringField(TEXT("landscapeMaterial"),
					Found->LandscapeMaterial ? Found->LandscapeMaterial->GetPathName() : FString());
				Structured->SetNumberField(TEXT("layerCount"), LayersJson.Num());
				Structured->SetArrayField(TEXT("layers"), LayersJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Landscape actor '%s' has %d component(s) and %d paint layer(s)."),
						*UnrealAILandscape_ActorName(Found), Found->LandscapeComponents.Num(), LayersJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// landscape-list-water-bodies — enumerate every Water body (River / Lake / Ocean / Custom)
		// in the active editor world (read-only). An empty world returns count 0.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("landscape-list-water-bodies"))
			.Title(TEXT("List Water Bodies"))
			.Description(TEXT("Lists every Water body actor in the active editor world (read-only). For each, reports "
			                  "its name, water-body type (River / Lake / Ocean / Custom), and spline-point count. "
			                  "Returns { count, waterBodies:[{ name, type, splinePointCount }] }."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = UnrealAILandscape_GetEditorWorld();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world is available to inspect."));
				}

				TArray<TSharedPtr<FJsonValue>> BodiesJson;
				for (TActorIterator<AWaterBody> It(World); It; ++It)
				{
					AWaterBody* Body = *It;
					if (!Body)
					{
						continue;
					}
					UWaterBodyComponent* Component = Body->GetWaterBodyComponent();

					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), UnrealAILandscape_ActorName(Body));
					Entry->SetStringField(TEXT("type"),
						Component ? UnrealAILandscape_WaterBodyTypeToString(Component->GetWaterBodyType()) : TEXT("Unknown"));
					const UWaterSplineComponent* Spline = Component ? Component->GetWaterSpline() : nullptr;
					Entry->SetNumberField(TEXT("splinePointCount"), Spline ? Spline->GetNumberOfSplinePoints() : 0);
					BodiesJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), BodiesJson.Num());
				Structured->SetArrayField(TEXT("waterBodies"), BodiesJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Water body(ies)."), BodiesJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// landscape-get-water-body — inspect a single Water body by name (read-only). Required
		// input; defensive Error on a missing name or an actor that does not exist.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("landscape-get-water-body"))
			.Title(TEXT("Get Water Body"))
			.Description(TEXT("Inspects a single Water body actor in the active editor world by name (read-only). "
			                  "Reports its type (River / Lake / Ocean / Custom), spline-point count, water material, "
			                  "and owning water zone. Returns { name, type, splinePointCount, waterMaterial, "
			                  "waterZone }."))
			.ParamString(TEXT("name"), TEXT("The Water body actor's editor label or object name, e.g. 'WaterBodyOcean'."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Name = Call.GetString(TEXT("name")).TrimStartAndEnd();
				if (Name.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'name' (the Water body actor's label, e.g. 'WaterBodyOcean')."));
				}

				UWorld* World = UnrealAILandscape_GetEditorWorld();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world is available to inspect."));
				}

				AWaterBody* Found = nullptr;
				for (TActorIterator<AWaterBody> It(World); It; ++It)
				{
					if (UnrealAILandscape_ActorMatches(*It, Name))
					{
						Found = *It;
						break;
					}
				}
				if (!Found)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Water body named '%s' found in the active editor world."), *Name));
				}

				UWaterBodyComponent* Component = Found->GetWaterBodyComponent();
				if (!Component)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Water body '%s' has no water-body component to inspect."), *Name));
				}

				const UWaterSplineComponent* Spline = Component->GetWaterSpline();
				const UMaterialInterface* WaterMaterial = Component->GetWaterMaterial();
				const AWaterZone* WaterZone = Component->GetWaterZone();

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("name"), UnrealAILandscape_ActorName(Found));
				Structured->SetStringField(TEXT("type"), UnrealAILandscape_WaterBodyTypeToString(Component->GetWaterBodyType()));
				Structured->SetNumberField(TEXT("splinePointCount"), Spline ? Spline->GetNumberOfSplinePoints() : 0);
				Structured->SetStringField(TEXT("waterMaterial"), WaterMaterial ? WaterMaterial->GetPathName() : FString());
				Structured->SetStringField(TEXT("waterZone"), WaterZone ? UnrealAILandscape_ActorName(WaterZone) : FString());
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Water body '%s' is a %s with %d spline point(s)."),
						*UnrealAILandscape_ActorName(Found),
						UnrealAILandscape_WaterBodyTypeToString(Component->GetWaterBodyType()),
						Spline ? Spline->GetNumberOfSplinePoints() : 0), Structured);
			});

		// -------------------------------------------------------------------------------------
		// landscape-list-water-zones — enumerate every Water zone (the rendering container that
		// owns water bodies) in the active editor world (read-only). An empty world returns count 0.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("landscape-list-water-zones"))
			.Title(TEXT("List Water Zones"))
			.Description(TEXT("Lists every Water zone actor in the active editor world (read-only). A water zone is "
			                  "the rendering container that water bodies belong to. For each, reports its name, world "
			                  "location, and 2D zone extent. Returns { count, waterZones:[{ name, x, y, z, extentX, "
			                  "extentY }] }."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = UnrealAILandscape_GetEditorWorld();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world is available to inspect."));
				}

				TArray<TSharedPtr<FJsonValue>> ZonesJson;
				for (TActorIterator<AWaterZone> It(World); It; ++It)
				{
					AWaterZone* Zone = *It;
					if (!Zone)
					{
						continue;
					}
					const FVector Location = Zone->GetActorLocation();
					const FVector2D Extent = Zone->GetZoneExtent();

					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), UnrealAILandscape_ActorName(Zone));
					Entry->SetNumberField(TEXT("x"), Location.X);
					Entry->SetNumberField(TEXT("y"), Location.Y);
					Entry->SetNumberField(TEXT("z"), Location.Z);
					Entry->SetNumberField(TEXT("extentX"), Extent.X);
					Entry->SetNumberField(TEXT("extentY"), Extent.Y);
					ZonesJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), ZonesJson.Num());
				Structured->SetArrayField(TEXT("waterZones"), ZonesJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Water zone(s)."), ZonesJson.Num()), Structured);
			});
	}
};

/**
 * Editor module that owns the provider and registers it as a modular feature, so Unreal-MCP discovers
 * it — on boot via initial enumeration, or live via the OnModularFeatureRegistered event when this
 * plugin loads after Unreal-MCP. Unregistering on shutdown triggers a registry rebuild + manifest
 * revision bump on the Unreal-MCP side (the token-economy win: disabling the extension live-removes
 * its tools from the advertised set).
 */
class FUnrealAILandscapeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeUnique<FUnrealAILandscapeProvider>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
		UE_LOG(LogUnrealAILandscape, Log, TEXT("[UnrealAILandscape] registered MCP tool provider '%s'."), *Provider->GetExtensionId());
	}

	virtual void ShutdownModule() override
	{
		if (Provider.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
			Provider.Reset();
			UE_LOG(LogUnrealAILandscape, Log, TEXT("[UnrealAILandscape] unregistered MCP tool provider."));
		}
	}

private:
	TUniquePtr<FUnrealAILandscapeProvider> Provider;
};

IMPLEMENT_MODULE(FUnrealAILandscapeModule, UnrealAILandscape)
