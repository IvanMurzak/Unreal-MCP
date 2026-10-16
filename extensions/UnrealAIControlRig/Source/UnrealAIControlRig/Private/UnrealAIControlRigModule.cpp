// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

// --- Control Rig + editor APIs the tools wrap ------------------------------------------------
#include "ControlRigBlueprintLegacy.h"          // UControlRigBlueprint (ControlRigDeveloper) — authoring asset + GetHierarchy()
#include "ControlRigComponent.h"                 // UControlRigComponent (ControlRig)
#include "Rigs/RigHierarchy.h"                   // URigHierarchy + element-key getters
#include "Rigs/RigHierarchyElements.h"           // FRigBaseElement / FRigControlElement / FRigControlSettings
#include "Rigs/RigHierarchyDefines.h"            // FRigElementKey / ERigElementType / ERigControlType
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GameFramework/Actor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"                         // TActorIterator
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIControlRig, Log, All);

// =================================================================================================
//  Module-unique file-local helpers (the module is unity-built — every .cpp is concatenated into one
//  TU, so an anonymous namespace does NOT make a helper file-private; prefix every helper with the
//  module name to avoid a one-definition-rule collision with another extension or a core family).
// =================================================================================================
namespace
{
	// Stringify an ERigElementType (Bone / Null / Control / Curve / …) for structured output.
	FString UnrealAIControlRig_ElementTypeToString(ERigElementType InType)
	{
		switch (InType)
		{
			case ERigElementType::Bone:      return TEXT("Bone");
			case ERigElementType::Null:      return TEXT("Null");
			case ERigElementType::Control:   return TEXT("Control");
			case ERigElementType::Curve:     return TEXT("Curve");
			case ERigElementType::Reference: return TEXT("Reference");
			case ERigElementType::Connector: return TEXT("Connector");
			case ERigElementType::Socket:    return TEXT("Socket");
			default:                         return TEXT("Unknown");
		}
	}

	// Stringify an ERigControlType (Bool / Float / Transform / …) for the controls inspector.
	FString UnrealAIControlRig_ControlTypeToString(ERigControlType InType)
	{
		if (const UEnum* Enum = StaticEnum<ERigControlType>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(InType));
		}
		return TEXT("Unknown");
	}

	// Load a Control Rig blueprint asset (the authoring asset, which itself owns the URigHierarchy) by
	// content path. Returns nullptr if the path is empty or no such asset exists — callers turn that
	// into a defensive FUnrealMcpToolResult::Error, never a crash.
	UControlRigBlueprint* UnrealAIControlRig_LoadBlueprint(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		return LoadObject<UControlRigBlueprint>(nullptr, *Path);
	}
}

/**
 * The extension's tool provider — an implementation of the Unreal-MCP extension contract
 * (IUnrealMcpToolProvider). It declares this extension's tools through the fluent
 * FUnrealMcpToolRegistry builder. See https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/EXTENSIONS.md.
 *
 * Control Rig is a heavy, plugin-gated animation-rigging system. This extension stays deliberately
 * THIN and READ-ONLY: every tool is a handler lambda over game-thread-safe Control Rig /
 * AssetRegistry / editor APIs, with no async work, no subsystems, and no owned UI. The family inspects
 * Control Rig blueprint assets (their rig-element hierarchy: bones / nulls / controls / curves) and a
 * live actor's Control Rig component — it does NOT author or mutate assets (asset creation is an
 * editor-module-heavy, headless-brittle operation deliberately left out of the first cut).
 *
 * Handlers are DEFENSIVE — UE builds without C++ exceptions, so a crash inside a handler is an editor
 * crash; every tool validates its inputs and the engine state it touches and returns
 * FUnrealMcpToolResult::Error(...) instead of dereferencing a null.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName — `commands/bump-version.ps1`
 * updates both atomically.
 */
class FUnrealAIControlRigProvider : public IUnrealMcpToolProvider
{
public:
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-control-rig"); }
	virtual FText GetDisplayName() const override { return NSLOCTEXT("UnrealAIControlRig", "DisplayName", "Unreal AI ControlRig"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.2"); }

	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
	{
		// =====================================================================================
		//  Tool ids are kebab-case (^[a-z0-9]+(-[a-z0-9]+)*$), prefixed `control-rig-`. Handlers run
		//  ON the game thread (the dispatcher guarantees it), so editor / engine APIs are called
		//  directly. A handler returns FUnrealMcpToolResult::Success(text, structuredJson) or
		//  ::Error(message). This is an inspection-only (read-only) family.
		// =====================================================================================

		// -------------------------------------------------------------------------------------
		// control-rig-list — enumerate every Control Rig blueprint (UControlRigBlueprint) asset in
		// the project via the AssetRegistry (no asset is loaded — cheap, read-only).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("control-rig-list"))
			.Title(TEXT("List Control Rigs"))
			.Description(TEXT("Lists every Control Rig blueprint (UControlRigBlueprint) asset in the project via the "
			                  "Asset Registry, without loading any of them. Optionally filter by a content-path prefix. "
			                  "Returns { count, controlRigs:[{ name, path }] }."))
			.ParamString(TEXT("pathPrefix"), TEXT("Optional content-path prefix filter, e.g. '/Game/Rigs'. Empty = whole project."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FAssetRegistryModule& AssetRegistryModule =
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

				TArray<FAssetData> Assets;
				AssetRegistry.GetAssetsByClass(UControlRigBlueprint::StaticClass()->GetClassPathName(), Assets);

				const FString PathPrefix = Call.GetString(TEXT("pathPrefix")).TrimStartAndEnd();

				TArray<TSharedPtr<FJsonValue>> RigsJson;
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
					RigsJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), RigsJson.Num());
				Structured->SetArrayField(TEXT("controlRigs"), RigsJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Control Rig asset(s)."), RigsJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// control-rig-get — load one Control Rig blueprint and report its rig-element hierarchy
		// summary (per-type element counts + every element's name & type). Read-only.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("control-rig-get"))
			.Title(TEXT("Get Control Rig"))
			.Description(TEXT("Inspects a single Control Rig blueprint asset (read-only) and reports its rig element "
			                  "hierarchy. Returns { path, name, boneCount, nullCount, controlCount, curveCount, "
			                  "elementCount, elements:[{ name, type }] }."))
			.ParamString(TEXT("path"), TEXT("Asset path of the Control Rig blueprint, e.g. '/Game/Rigs/MyControlRig'."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/Rigs/MyControlRig')."));
				}

				UControlRigBlueprint* Blueprint = UnrealAIControlRig_LoadBlueprint(Path);
				if (!Blueprint)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Control Rig found at '%s'."), *Path));
				}

				URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
				if (!Hierarchy)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Control Rig '%s' has no rig hierarchy."), *Path));
				}

				TArray<TSharedPtr<FJsonValue>> ElementsJson;
				for (const FRigElementKey& Key : Hierarchy->GetAllKeys(/*bTraverse=*/true))
				{
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Key.Name.ToString());
					Entry->SetStringField(TEXT("type"), UnrealAIControlRig_ElementTypeToString(Key.Type));
					ElementsJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Path);
				Structured->SetStringField(TEXT("name"), Blueprint->GetName());
				Structured->SetNumberField(TEXT("boneCount"), Hierarchy->Num(ERigElementType::Bone));
				Structured->SetNumberField(TEXT("nullCount"), Hierarchy->Num(ERigElementType::Null));
				Structured->SetNumberField(TEXT("controlCount"), Hierarchy->Num(ERigElementType::Control));
				Structured->SetNumberField(TEXT("curveCount"), Hierarchy->Num(ERigElementType::Curve));
				Structured->SetNumberField(TEXT("elementCount"), ElementsJson.Num());
				Structured->SetArrayField(TEXT("elements"), ElementsJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Control Rig '%s' has %d element(s) (%d bone, %d control, %d null, %d curve)."),
						*Blueprint->GetName(), ElementsJson.Num(),
						Hierarchy->Num(ERigElementType::Bone), Hierarchy->Num(ERigElementType::Control),
						Hierarchy->Num(ERigElementType::Null), Hierarchy->Num(ERigElementType::Curve)),
					Structured);
			});

		// -------------------------------------------------------------------------------------
		// control-rig-list-controls — load one Control Rig blueprint and enumerate its CONTROL
		// elements with their control type (Bool / Float / Transform / …). Read-only.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("control-rig-list-controls"))
			.Title(TEXT("List Control Rig Controls"))
			.Description(TEXT("Lists the control elements of a Control Rig blueprint (read-only), each with its control "
			                  "type (e.g. Float, Transform, Bool). Returns { path, controlCount, controls:[{ name, controlType }] }."))
			.ParamString(TEXT("path"), TEXT("Asset path of the Control Rig blueprint, e.g. '/Game/Rigs/MyControlRig'."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/Rigs/MyControlRig')."));
				}

				UControlRigBlueprint* Blueprint = UnrealAIControlRig_LoadBlueprint(Path);
				if (!Blueprint)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Control Rig found at '%s'."), *Path));
				}

				URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
				if (!Hierarchy)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Control Rig '%s' has no rig hierarchy."), *Path));
				}

				TArray<TSharedPtr<FJsonValue>> ControlsJson;
				for (const FRigElementKey& Key : Hierarchy->GetControlKeys(/*bTraverse=*/true))
				{
					FString ControlTypeStr = TEXT("Unknown");
					if (const FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key))
					{
						ControlTypeStr = UnrealAIControlRig_ControlTypeToString(Control->Settings.ControlType);
					}
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Key.Name.ToString());
					Entry->SetStringField(TEXT("controlType"), ControlTypeStr);
					ControlsJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Path);
				Structured->SetNumberField(TEXT("controlCount"), ControlsJson.Num());
				Structured->SetArrayField(TEXT("controls"), ControlsJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Control Rig '%s' has %d control(s)."), *Blueprint->GetName(), ControlsJson.Num()),
					Structured);
			});

		// -------------------------------------------------------------------------------------
		// control-rig-list-bones — load one Control Rig blueprint and enumerate its BONE elements
		// with each bone's first (immediate) parent. Read-only.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("control-rig-list-bones"))
			.Title(TEXT("List Control Rig Bones"))
			.Description(TEXT("Lists the bone elements of a Control Rig blueprint (read-only), each with its immediate "
			                  "parent bone (empty for a root). Returns { path, boneCount, bones:[{ name, parent }] }."))
			.ParamString(TEXT("path"), TEXT("Asset path of the Control Rig blueprint, e.g. '/Game/Rigs/MyControlRig'."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/Rigs/MyControlRig')."));
				}

				UControlRigBlueprint* Blueprint = UnrealAIControlRig_LoadBlueprint(Path);
				if (!Blueprint)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Control Rig found at '%s'."), *Path));
				}

				URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
				if (!Hierarchy)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Control Rig '%s' has no rig hierarchy."), *Path));
				}

				TArray<TSharedPtr<FJsonValue>> BonesJson;
				for (const FRigElementKey& Key : Hierarchy->GetBoneKeys(/*bTraverse=*/true))
				{
					const FRigElementKey ParentKey = Hierarchy->GetFirstParent(Key);
					const FString ParentName = ParentKey.IsValid() ? ParentKey.Name.ToString() : FString();
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Key.Name.ToString());
					Entry->SetStringField(TEXT("parent"), ParentName);
					BonesJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Path);
				Structured->SetNumberField(TEXT("boneCount"), BonesJson.Num());
				Structured->SetArrayField(TEXT("bones"), BonesJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Control Rig '%s' has %d bone(s)."), *Blueprint->GetName(), BonesJson.Num()),
					Structured);
			});

		// -------------------------------------------------------------------------------------
		// control-rig-get-component — find an actor in the active editor world by label and inspect
		// its UControlRigComponent (the bound Control Rig class). Read-only.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("control-rig-get-component"))
			.Title(TEXT("Get Control Rig Component"))
			.Description(TEXT("Inspects the Control Rig component of an actor in the active editor world (read-only). "
			                  "Looks the actor up by its label. Returns { actorName, componentName, controlRigClass, hasControlRig } "
			                  "(controlRigClass is the bound rig instance's class, or \"None\" when no rig is bound) "
			                  "or a defensive error if the actor or component is not found."))
			.ParamString(TEXT("actorName"), TEXT("Label of the actor to inspect, as shown in the World Outliner."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ActorName = Call.GetString(TEXT("actorName")).TrimStartAndEnd();
				if (ActorName.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'actorName' (the actor's World Outliner label)."));
				}

				if (!GEditor)
				{
					return FUnrealMcpToolResult::Error(TEXT("No editor (GEditor) is available to resolve the actor."));
				}
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world to search for the actor."));
				}

				AActor* FoundActor = nullptr;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (Actor && Actor->GetActorLabel() == ActorName)
					{
						FoundActor = Actor;
						break;
					}
				}
				if (!FoundActor)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No actor named '%s' in the active editor world."), *ActorName));
				}

				UControlRigComponent* Component = FoundActor->FindComponentByClass<UControlRigComponent>();
				if (!Component)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Actor '%s' has no Control Rig component."), *ActorName));
				}

				// Obtain the bound rig CLASS via the documented public accessor GetControlRig() rather
				// than the raw UPROPERTY UControlRigComponent::ControlRigClass: that member's accessibility
				// is not stable across UE versions (it is not reliably public in 5.8+), and referencing it
				// directly breaks the fresh compile on those engines. GetControlRig() is a stable, public
				// BlueprintPure API present across UE 5.7/5.8 and returns the live UControlRig instance whose
				// UClass is the bound rig class. Degrade gracefully: when no rig is bound/instantiated (e.g.
				// the component has no class set, or the rig has not been instanced at pure editor time),
				// report "None" rather than dereferencing a non-existent member or a null pointer.
				UControlRig* ControlRig = Component->GetControlRig();
				const bool bHasRig = (ControlRig != nullptr);
				const FString ControlRigClassName =
					bHasRig ? ControlRig->GetClass()->GetName() : FString(TEXT("None"));

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("actorName"), ActorName);
				Structured->SetStringField(TEXT("componentName"), Component->GetName());
				Structured->SetStringField(TEXT("controlRigClass"), ControlRigClassName);
				Structured->SetBoolField(TEXT("hasControlRig"), bHasRig);
				return FUnrealMcpToolResult::Success(
					bHasRig
						? FString::Printf(TEXT("Actor '%s' has Control Rig component '%s' (rig class: %s)."),
							*ActorName, *Component->GetName(), *ControlRigClassName)
						: FString::Printf(TEXT("Actor '%s' has Control Rig component '%s' with no Control Rig bound."),
							*ActorName, *Component->GetName()),
					Structured);
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
class FUnrealAIControlRigModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeUnique<FUnrealAIControlRigProvider>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
		UE_LOG(LogUnrealAIControlRig, Log, TEXT("[UnrealAIControlRig] registered MCP tool provider '%s'."), *Provider->GetExtensionId());
	}

	virtual void ShutdownModule() override
	{
		if (Provider.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
			Provider.Reset();
			UE_LOG(LogUnrealAIControlRig, Log, TEXT("[UnrealAIControlRig] unregistered MCP tool provider."));
		}
	}

private:
	TUniquePtr<FUnrealAIControlRigProvider> Provider;
};

IMPLEMENT_MODULE(FUnrealAIControlRigModule, UnrealAIControlRig)
