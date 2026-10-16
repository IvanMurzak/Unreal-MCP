// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

// --- PCG + editor APIs the tools wrap --------------------------------------------------------
#include "PCGGraph.h"
#include "PCGComponent.h"
#include "PCGNode.h"
#include "PCGCommon.h" // EPCGNodeTitleType
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h" // TActorIterator

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIPCG, Log, All);

namespace
{
	// Module-unique helper name — the module is unity-built (every .cpp concatenated into one TU),
	// so an anonymous namespace does NOT make a helper file-private. Prefix with the module name to
	// avoid an ODR collision with any same-name helper in another .cpp of this module.
	AActor* UnrealAIPCG_FindActorByName(UWorld* World, const FString& ActorName)
	{
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetName() == ActorName || Actor->GetActorNameOrLabel() == ActorName)
			{
				return Actor;
			}
		}
		return nullptr;
	}
}

/**
 * The extension's tool provider — an implementation of the Unreal-MCP extension contract
 * (IUnrealMcpToolProvider). It declares this extension's tools through the fluent
 * FUnrealMcpToolRegistry builder. See https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/EXTENSIONS.md.
 *
 * PCG (Procedural Content Generation) is a heavy, plugin-gated framework. This extension stays
 * deliberately THIN: every tool is a handler lambda over game-thread-safe PCG / AssetRegistry /
 * editor APIs, with no async work, no subsystems, and no owned UI. Handlers are DEFENSIVE — UE
 * builds without C++ exceptions, so a crash inside a handler is an editor crash; every tool
 * validates its inputs and the engine state it touches and returns FUnrealMcpToolResult::Error(...)
 * instead of dereferencing a null.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName — `commands/bump-version.ps1`
 * updates both atomically.
 */
class FUnrealAIPCGProvider : public IUnrealMcpToolProvider
{
public:
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-pcg"); }
	virtual FText GetDisplayName() const override { return NSLOCTEXT("UnrealAIPCG", "DisplayName", "Unreal AI PCG"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.0"); }

	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
	{
		// =====================================================================================
		//  Tool ids are kebab-case (^[a-z0-9]+(-[a-z0-9]+)*$). Handlers run ON the game thread
		//  (the dispatcher guarantees it), so editor / engine APIs are called directly. A handler
		//  returns FUnrealMcpToolResult::Success(text, structuredJson) or ::Error(message).
		// =====================================================================================

		// -------------------------------------------------------------------------------------
		// pcg-list-graphs — enumerate every UPCGGraph asset in the project via the AssetRegistry
		// (no asset is loaded — cheap, read-only).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("pcg-list-graphs"))
			.Title(TEXT("List PCG Graphs"))
			.Description(TEXT("Lists every PCG graph (UPCGGraph) asset in the project via the Asset Registry, "
			                  "without loading any of them. Optionally filter by a content-path prefix. "
			                  "Returns { count, graphs:[{ name, path }] }."))
			.ParamString(TEXT("pathPrefix"), TEXT("Optional content-path prefix filter, e.g. '/Game/PCG'. Empty = whole project."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FAssetRegistryModule& AssetRegistryModule =
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

				TArray<FAssetData> Assets;
				AssetRegistry.GetAssetsByClass(UPCGGraph::StaticClass()->GetClassPathName(), Assets);

				const FString PathPrefix = Call.GetString(TEXT("pathPrefix")).TrimStartAndEnd();

				TArray<TSharedPtr<FJsonValue>> GraphsJson;
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
					GraphsJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), GraphsJson.Num());
				Structured->SetArrayField(TEXT("graphs"), GraphsJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d PCG graph(s)."), GraphsJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// pcg-get-graph — load one graph and report its nodes (read-only).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("pcg-get-graph"))
			.Title(TEXT("Get PCG Graph"))
			.Description(TEXT("Inspects a single PCG graph asset (read-only) and reports its nodes. "
			                  "Returns { path, name, nodeCount, nodes:[{ name, title }] }."))
			.ParamString(TEXT("path"), TEXT("Asset path of the PCG graph, e.g. '/Game/PCG/MyGraph'."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/PCG/MyGraph')."));
				}

				UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *Path);
				if (!Graph)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No PCG graph found at '%s'."), *Path));
				}

				TArray<TSharedPtr<FJsonValue>> NodesJson;
				for (const UPCGNode* Node : Graph->GetNodes())
				{
					if (!Node)
					{
						continue;
					}
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Node->GetName());
					Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
					NodesJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Path);
				Structured->SetStringField(TEXT("name"), Graph->GetName());
				Structured->SetNumberField(TEXT("nodeCount"), NodesJson.Num());
				Structured->SetArrayField(TEXT("nodes"), NodesJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("PCG graph '%s' has %d node(s)."), *Graph->GetName(), NodesJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// pcg-add-component — add a UPCGComponent to an actor in the active editor world, optionally
		// bound to a graph. Mutates the world (destructive + open-world hints).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("pcg-add-component"))
			.Title(TEXT("Add PCG Component"))
			.Description(TEXT("Adds a UPCGComponent to a named actor in the active editor world, optionally bound to a "
			                  "PCG graph asset. Returns { actorName, componentName, graphPath }."))
			.ParamString(TEXT("actorName"), TEXT("Name or label of the target actor in the active editor world."),
				EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("graphPath"), TEXT("Optional asset path of a PCG graph to bind to the new component, e.g. '/Game/PCG/MyGraph'."))
			.DestructiveHint(true)
			.OpenWorldHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ActorName = Call.GetString(TEXT("actorName")).TrimStartAndEnd();
				if (ActorName.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'actorName' (the actor to add a PCG component to)."));
				}

				if (!GEditor)
				{
					return FUnrealMcpToolResult::Error(TEXT("No editor (GEditor) is available to host the PCG component."));
				}
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world to modify."));
				}

				AActor* Actor = UnrealAIPCG_FindActorByName(World, ActorName);
				if (!Actor)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No actor named '%s' found in the editor world."), *ActorName));
				}

				// Optional graph binding — validate it BEFORE mutating the actor so a bad path is a
				// clean defensive error, not a half-added component.
				UPCGGraph* Graph = nullptr;
				const FString GraphPath = Call.GetString(TEXT("graphPath")).TrimStartAndEnd();
				if (!GraphPath.IsEmpty())
				{
					Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
					if (!Graph)
					{
						return FUnrealMcpToolResult::Error(
							FString::Printf(TEXT("No PCG graph found at '%s'."), *GraphPath));
					}
				}

				UPCGComponent* Component = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(), NAME_None, RF_Transactional);
				if (!Component)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Failed to create a PCG component on '%s'."), *ActorName));
				}
				Actor->AddInstanceComponent(Component);
				Component->RegisterComponent();
				if (Graph)
				{
					Component->SetGraph(Graph);
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("actorName"), Actor->GetActorNameOrLabel());
				Structured->SetStringField(TEXT("componentName"), Component->GetName());
				Structured->SetStringField(TEXT("graphPath"), GraphPath);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Added PCG component '%s' to actor '%s'%s."),
						*Component->GetName(), *Actor->GetActorNameOrLabel(),
						GraphPath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" bound to '%s'"), *GraphPath)),
					Structured);
			});

		// -------------------------------------------------------------------------------------
		// pcg-get-component — inspect the PCG component on a named actor (read-only).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("pcg-get-component"))
			.Title(TEXT("Get PCG Component"))
			.Description(TEXT("Inspects the UPCGComponent on a named actor in the active editor world (read-only): "
			                  "its bound graph and generation state. Returns { actorName, componentName, hasGraph, "
			                  "graphName, isGenerating }."))
			.ParamString(TEXT("actorName"), TEXT("Name or label of the actor to inspect in the active editor world."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ActorName = Call.GetString(TEXT("actorName")).TrimStartAndEnd();
				if (ActorName.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'actorName' (the actor whose PCG component to inspect)."));
				}

				if (!GEditor)
				{
					return FUnrealMcpToolResult::Error(TEXT("No editor (GEditor) is available to read the editor world."));
				}
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world to read."));
				}

				AActor* Actor = UnrealAIPCG_FindActorByName(World, ActorName);
				if (!Actor)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No actor named '%s' found in the editor world."), *ActorName));
				}

				UPCGComponent* Component = Actor->FindComponentByClass<UPCGComponent>();
				if (!Component)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Actor '%s' has no PCG component."), *ActorName));
				}

				const UPCGGraph* Graph = Component->GetGraph();
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("actorName"), Actor->GetActorNameOrLabel());
				Structured->SetStringField(TEXT("componentName"), Component->GetName());
				Structured->SetBoolField(TEXT("hasGraph"), Graph != nullptr);
				Structured->SetStringField(TEXT("graphName"), Graph ? Graph->GetName() : FString());
				Structured->SetBoolField(TEXT("isGenerating"), Component->IsGenerating());
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("PCG component '%s' on '%s' (%s)."),
						*Component->GetName(), *Actor->GetActorNameOrLabel(),
						Graph ? *FString::Printf(TEXT("graph '%s'"), *Graph->GetName()) : TEXT("no graph bound")),
					Structured);
			});

		// -------------------------------------------------------------------------------------
		// pcg-generate — trigger generation / refresh on a named actor's PCG component. Mutating
		// (destructive + open-world hints).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("pcg-generate"))
			.Title(TEXT("Generate PCG"))
			.Description(TEXT("Triggers PCG generation (refresh) on the UPCGComponent of a named actor in the active "
			                  "editor world. Returns { actorName, componentName, triggered }."))
			.ParamString(TEXT("actorName"), TEXT("Name or label of the actor whose PCG component to generate."),
				EUnrealMcpParamRequirement::Required)
			.DestructiveHint(true)
			.OpenWorldHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ActorName = Call.GetString(TEXT("actorName")).TrimStartAndEnd();
				if (ActorName.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'actorName' (the actor whose PCG component to generate)."));
				}

				if (!GEditor)
				{
					return FUnrealMcpToolResult::Error(TEXT("No editor (GEditor) is available to drive generation."));
				}
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world to generate in."));
				}

				AActor* Actor = UnrealAIPCG_FindActorByName(World, ActorName);
				if (!Actor)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No actor named '%s' found in the editor world."), *ActorName));
				}

				UPCGComponent* Component = Actor->FindComponentByClass<UPCGComponent>();
				if (!Component)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Actor '%s' has no PCG component to generate."), *ActorName));
				}

				Component->Generate();

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("actorName"), Actor->GetActorNameOrLabel());
				Structured->SetStringField(TEXT("componentName"), Component->GetName());
				Structured->SetBoolField(TEXT("triggered"), true);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Triggered PCG generation on component '%s' of actor '%s'."),
						*Component->GetName(), *Actor->GetActorNameOrLabel()), Structured);
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
class FUnrealAIPCGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeUnique<FUnrealAIPCGProvider>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
		UE_LOG(LogUnrealAIPCG, Log, TEXT("[UnrealAIPCG] registered MCP tool provider '%s'."), *Provider->GetExtensionId());
	}

	virtual void ShutdownModule() override
	{
		if (Provider.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
			Provider.Reset();
			UE_LOG(LogUnrealAIPCG, Log, TEXT("[UnrealAIPCG] unregistered MCP tool provider."));
		}
	}

private:
	TUniquePtr<FUnrealAIPCGProvider> Provider;
};

IMPLEMENT_MODULE(FUnrealAIPCGModule, UnrealAIPCG)
