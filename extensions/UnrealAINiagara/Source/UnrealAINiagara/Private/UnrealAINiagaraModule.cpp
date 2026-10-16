// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

// --- Niagara + editor APIs the tools wrap ----------------------------------------------------
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAINiagara, Log, All);

/**
 * The extension's tool provider — an implementation of the Unreal-MCP extension contract
 * (IUnrealMcpToolProvider). It declares this extension's tools through the fluent
 * FUnrealMcpToolRegistry builder. See https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/EXTENSIONS.md.
 *
 * Niagara is a heavy, plugin-gated VFX system. This extension stays deliberately THIN: every tool is
 * a handler lambda over game-thread-safe Niagara / AssetRegistry / editor APIs, with no async work,
 * no subsystems, and no owned UI. Handlers are DEFENSIVE — UE builds without C++ exceptions, so a
 * crash inside a handler is an editor crash; every tool validates its inputs and the engine state it
 * touches and returns FUnrealMcpToolResult::Error(...) instead of dereferencing a null.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName — `commands/bump-version.ps1`
 * updates both atomically.
 */
class FUnrealAINiagaraProvider : public IUnrealMcpToolProvider
{
public:
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-niagara"); }
	virtual FText GetDisplayName() const override { return NSLOCTEXT("UnrealAINiagara", "DisplayName", "Unreal AI Niagara"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.0"); }

	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
	{
		// =====================================================================================
		//  Tool ids are kebab-case (^[a-z0-9]+(-[a-z0-9]+)*$). Handlers run ON the game thread
		//  (the dispatcher guarantees it), so editor / engine APIs are called directly. A handler
		//  returns FUnrealMcpToolResult::Success(text, structuredJson) or ::Error(message).
		// =====================================================================================

		// -------------------------------------------------------------------------------------
		// niagara-list-systems — enumerate every UNiagaraSystem asset in the project via the
		// AssetRegistry (no asset is loaded — cheap, read-only).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("niagara-list-systems"))
			.Title(TEXT("List Niagara Systems"))
			.Description(TEXT("Lists every Niagara system (UNiagaraSystem) asset in the project via the Asset "
			                  "Registry, without loading any of them. Optionally filter by a content-path prefix. "
			                  "Returns { count, systems:[{ name, path }] }."))
			.ParamString(TEXT("pathPrefix"), TEXT("Optional content-path prefix filter, e.g. '/Game/VFX'. Empty = whole project."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FAssetRegistryModule& AssetRegistryModule =
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

				TArray<FAssetData> Assets;
				AssetRegistry.GetAssetsByClass(UNiagaraSystem::StaticClass()->GetClassPathName(), Assets);

				const FString PathPrefix = Call.GetString(TEXT("pathPrefix")).TrimStartAndEnd();

				TArray<TSharedPtr<FJsonValue>> SystemsJson;
				for (const FAssetData& Asset : Assets)
				{
					const FString ObjectPath = Asset.GetObjectPathString();
					if (!PathPrefix.IsEmpty() && !ObjectPath.StartsWith(PathPrefix))
					{
						continue;
					}
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
					Entry->SetStringField(TEXT("path"), ObjectPath);
					SystemsJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), SystemsJson.Num());
				Structured->SetArrayField(TEXT("systems"), SystemsJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Niagara system(s)."), SystemsJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// niagara-get-system — load one system and report its basic, read-only properties
		// (emitter count + per-emitter name / enabled flag).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("niagara-get-system"))
			.Title(TEXT("Get Niagara System"))
			.Description(TEXT("Inspects a single Niagara system asset (read-only) and reports its emitters. "
			                  "Returns { path, name, emitterCount, emitters:[{ name, enabled }] }."))
			.ParamString(TEXT("path"), TEXT("Asset path of the Niagara system, e.g. '/Game/VFX/MySystem'."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/VFX/MySystem')."));
				}

				UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path);
				if (!System)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Niagara system found at '%s'."), *Path));
				}

				TArray<TSharedPtr<FJsonValue>> EmittersJson;
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Handle.GetName().ToString());
					Entry->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
					EmittersJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Path);
				Structured->SetStringField(TEXT("name"), System->GetName());
				Structured->SetNumberField(TEXT("emitterCount"), EmittersJson.Num());
				Structured->SetArrayField(TEXT("emitters"), EmittersJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Niagara system '%s' has %d emitter(s)."),
						*System->GetName(), EmittersJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// niagara-spawn-component — spawn a Niagara component for a system in the editor world at
		// a location. Mutates the world (destructive + open-world hints).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("niagara-spawn-component"))
			.Title(TEXT("Spawn Niagara Component"))
			.Description(TEXT("Spawns a Niagara component for the given system into the active editor world at a "
			                  "location. Returns { systemPath, componentName, x, y, z }."))
			.ParamString(TEXT("systemPath"), TEXT("Asset path of the Niagara system to spawn, e.g. '/Game/VFX/MySystem'."),
				EUnrealMcpParamRequirement::Required)
			.ParamVector(TEXT("location"), TEXT("World location { x, y, z } to spawn at. Defaults to the origin."))
			.DestructiveHint(true)
			.OpenWorldHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString SystemPath = Call.GetString(TEXT("systemPath")).TrimStartAndEnd();
				if (SystemPath.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'systemPath' (e.g. '/Game/VFX/MySystem')."));
				}

				if (!GEditor)
				{
					return FUnrealMcpToolResult::Error(TEXT("No editor (GEditor) is available to host the spawned component."));
				}
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world to spawn into."));
				}

				UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
				if (!System)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Niagara system found at '%s'."), *SystemPath));
				}

				const FVector Location = Call.GetVector(TEXT("location"), FVector::ZeroVector);
				UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
					World, System, Location, FRotator::ZeroRotator, FVector(1.0f),
					/*bAutoDestroy=*/false, /*bAutoActivate=*/true);
				if (!Component)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Failed to spawn a Niagara component for '%s'."), *SystemPath));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("systemPath"), SystemPath);
				Structured->SetStringField(TEXT("componentName"), Component->GetName());
				Structured->SetNumberField(TEXT("x"), Location.X);
				Structured->SetNumberField(TEXT("y"), Location.Y);
				Structured->SetNumberField(TEXT("z"), Location.Z);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Spawned Niagara component '%s' for '%s' at (%.1f, %.1f, %.1f)."),
						*Component->GetName(), *SystemPath, Location.X, Location.Y, Location.Z), Structured);
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
class FUnrealAINiagaraModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeUnique<FUnrealAINiagaraProvider>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
		UE_LOG(LogUnrealAINiagara, Log, TEXT("[UnrealAINiagara] registered MCP tool provider '%s'."), *Provider->GetExtensionId());
	}

	virtual void ShutdownModule() override
	{
		if (Provider.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
			Provider.Reset();
			UE_LOG(LogUnrealAINiagara, Log, TEXT("[UnrealAINiagara] unregistered MCP tool provider."));
		}
	}

private:
	TUniquePtr<FUnrealAINiagaraProvider> Provider;
};

IMPLEMENT_MODULE(FUnrealAINiagaraModule, UnrealAINiagara)
