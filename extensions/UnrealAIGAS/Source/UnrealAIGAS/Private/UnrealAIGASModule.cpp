// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Editor.h"
#include "Engine/World.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

// --- Gameplay Ability System + editor APIs the tools wrap ------------------------------------
#include "Abilities/GameplayAbility.h"
#include "Abilities/GameplayAbilityTypes.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameplayAbilitySpec.h"
#include "GameplayTagContainer.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIGAS, Log, All);

// ================================================================================================
//  File-local helpers. The module is UNITY-BUILT (every .cpp concatenated into one TU), so an
//  anonymous namespace does NOT make a helper file-private — every file-local helper is given a
//  module-unique `UnrealAIGAS_` prefix to avoid an ODR collision with the spec's helpers.
// ================================================================================================
namespace
{
	/** Serialize a gameplay-tag container to a JSON string array (one entry per tag). */
	TArray<TSharedPtr<FJsonValue>> UnrealAIGAS_TagsToJsonArray(const FGameplayTagContainer& Tags)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FGameplayTag& Tag : Tags)
		{
			Out.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		return Out;
	}

	/**
	 * Read a (possibly protected) FGameplayTagContainer UPROPERTY off an object by name, via UE
	 * reflection — sidesteps C++ access control so we can surface GameplayAbility tag fields that
	 * have no public getter (CancelAbilitiesWithTag / BlockAbilitiesWithTag). Returns nullptr when
	 * the property is absent or not a tag container.
	 */
	const FGameplayTagContainer* UnrealAIGAS_ReadTagProperty(const UObject* Obj, const FName PropName)
	{
		if (!Obj)
		{
			return nullptr;
		}
		if (const FStructProperty* Prop = FindFProperty<FStructProperty>(Obj->GetClass(), PropName))
		{
			if (Prop->Struct == FGameplayTagContainer::StaticStruct())
			{
				return Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(Obj);
			}
		}
		return nullptr;
	}

	/** Human-readable name for a GameplayAbility instancing policy. */
	FString UnrealAIGAS_InstancingPolicyToString(const EGameplayAbilityInstancingPolicy::Type Policy)
	{
		switch (Policy)
		{
			case EGameplayAbilityInstancingPolicy::NonInstanced:         return TEXT("NonInstanced");
			case EGameplayAbilityInstancingPolicy::InstancedPerActor:    return TEXT("InstancedPerActor");
			case EGameplayAbilityInstancingPolicy::InstancedPerExecution:return TEXT("InstancedPerExecution");
			default:                                                     return TEXT("Unknown");
		}
	}

	/**
	 * Enumerate every loaded + on-disk subclass of @p BaseClass via the Asset Registry's class graph
	 * (covers native C++ classes AND unloaded Blueprint-generated classes), without loading any of
	 * them. Returns a JSON array of { name, path }. Optionally filters by a class-path prefix.
	 */
	TArray<TSharedPtr<FJsonValue>> UnrealAIGAS_ListDerivedClasses(const UClass* BaseClass, const FString& PathPrefix)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (!BaseClass)
		{
			return Out;
		}

		const FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		const FTopLevelAssetPath BasePath = BaseClass->GetClassPathName();
		TArray<FTopLevelAssetPath> BaseNames;
		BaseNames.Add(BasePath);
		const TSet<FTopLevelAssetPath> Excluded;
		TSet<FTopLevelAssetPath> DerivedNames;
		AssetRegistry.GetDerivedClassNames(BaseNames, Excluded, DerivedNames);

		for (const FTopLevelAssetPath& ClassPath : DerivedNames)
		{
			if (ClassPath == BasePath)
			{
				continue; // the base itself is not a "derived" class
			}
			const FString PathString = ClassPath.ToString();
			if (!PathPrefix.IsEmpty() && !PathString.StartsWith(PathPrefix))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), ClassPath.GetAssetName().ToString());
			Entry->SetStringField(TEXT("path"), PathString);
			Out.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Out;
	}

	/** Load a UGameplayAbility class by path, tolerating a Blueprint path missing the `_C` suffix. */
	UClass* UnrealAIGAS_LoadAbilityClass(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* Loaded = LoadClass<UGameplayAbility>(nullptr, *Path))
		{
			return Loaded;
		}
		if (!Path.EndsWith(TEXT("_C")))
		{
			return LoadClass<UGameplayAbility>(nullptr, *(Path + TEXT("_C")));
		}
		return nullptr;
	}

	/** Find an actor in @p World by its editor label or internal name (first match wins). */
	AActor* UnrealAIGAS_FindActorByName(UWorld* World, const FString& ActorName)
	{
		if (!World || ActorName.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && (Actor->GetActorNameOrLabel() == ActorName || Actor->GetName() == ActorName))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	/** Resolve an actor's AbilitySystemComponent via the GAS interface, falling back to a component search. */
	UAbilitySystemComponent* UnrealAIGAS_GetASC(AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}
		if (const IAbilitySystemInterface* AsiActor = Cast<IAbilitySystemInterface>(Actor))
		{
			if (UAbilitySystemComponent* Asc = AsiActor->GetAbilitySystemComponent())
			{
				return Asc;
			}
		}
		return Actor->FindComponentByClass<UAbilitySystemComponent>();
	}
}

/**
 * The extension's tool provider — an implementation of the Unreal-MCP extension contract
 * (IUnrealMcpToolProvider). It declares this extension's tools through the fluent
 * FUnrealMcpToolRegistry builder. See https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/EXTENSIONS.md.
 *
 * The Gameplay Ability System (GAS) is a heavy, plugin-gated gameplay framework. This extension stays
 * deliberately THIN: every tool is a handler lambda over game-thread-safe GAS / AssetRegistry / editor
 * APIs, with no async work, no subsystems, and no owned UI. Handlers are DEFENSIVE — UE builds without
 * C++ exceptions, so a crash inside a handler is an editor crash; every tool validates its inputs and
 * the engine state it touches and returns FUnrealMcpToolResult::Error(...) instead of dereferencing a null.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName — `commands/bump-version.ps1`
 * updates both atomically.
 */
class FUnrealAIGASProvider : public IUnrealMcpToolProvider
{
public:
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-gas"); }
	virtual FText GetDisplayName() const override { return NSLOCTEXT("UnrealAIGAS", "DisplayName", "Unreal AI GAS"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.0"); }

	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
	{
		// =====================================================================================
		//  Tool ids are kebab-case (^[a-z0-9]+(-[a-z0-9]+)*$). Handlers run ON the game thread
		//  (the dispatcher guarantees it), so editor / engine APIs are called directly. A handler
		//  returns FUnrealMcpToolResult::Success(text, structuredJson) or ::Error(message).
		// =====================================================================================

		// -------------------------------------------------------------------------------------
		// gas-list-abilities — enumerate every UGameplayAbility subclass (native + Blueprint) via
		// the Asset Registry's class graph, without loading any of them (cheap, read-only).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("gas-list-abilities"))
			.Title(TEXT("List Gameplay Abilities"))
			.Description(TEXT("Lists every Gameplay Ability class (UGameplayAbility subclass — native C++ and "
			                  "Blueprint) known to the project via the Asset Registry, without loading any of "
			                  "them. Optionally filter by a class-path prefix. Returns { count, abilities:[{ name, path }] }."))
			.ParamString(TEXT("pathPrefix"), TEXT("Optional class-path prefix filter, e.g. '/Game/Abilities'. Empty = whole project."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString PathPrefix = Call.GetString(TEXT("pathPrefix")).TrimStartAndEnd();
				TArray<TSharedPtr<FJsonValue>> Abilities =
					UnrealAIGAS_ListDerivedClasses(UGameplayAbility::StaticClass(), PathPrefix);

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), Abilities.Num());
				Structured->SetArrayField(TEXT("abilities"), Abilities);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Gameplay Ability class(es)."), Abilities.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// gas-list-effects — enumerate every UGameplayEffect subclass via the Asset Registry.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("gas-list-effects"))
			.Title(TEXT("List Gameplay Effects"))
			.Description(TEXT("Lists every Gameplay Effect class (UGameplayEffect subclass — native C++ and "
			                  "Blueprint) known to the project via the Asset Registry, without loading any of "
			                  "them. Optionally filter by a class-path prefix. Returns { count, effects:[{ name, path }] }."))
			.ParamString(TEXT("pathPrefix"), TEXT("Optional class-path prefix filter, e.g. '/Game/Effects'. Empty = whole project."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString PathPrefix = Call.GetString(TEXT("pathPrefix")).TrimStartAndEnd();
				TArray<TSharedPtr<FJsonValue>> Effects =
					UnrealAIGAS_ListDerivedClasses(UGameplayEffect::StaticClass(), PathPrefix);

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), Effects.Num());
				Structured->SetArrayField(TEXT("effects"), Effects);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Gameplay Effect class(es)."), Effects.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// gas-list-attribute-sets — enumerate every UAttributeSet subclass via the Asset Registry.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("gas-list-attribute-sets"))
			.Title(TEXT("List Attribute Sets"))
			.Description(TEXT("Lists every Attribute Set class (UAttributeSet subclass — native C++ and Blueprint) "
			                  "known to the project via the Asset Registry, without loading any of them. Optionally "
			                  "filter by a class-path prefix. Returns { count, attributeSets:[{ name, path }] }."))
			.ParamString(TEXT("pathPrefix"), TEXT("Optional class-path prefix filter, e.g. '/Game/Attributes'. Empty = whole project."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString PathPrefix = Call.GetString(TEXT("pathPrefix")).TrimStartAndEnd();
				TArray<TSharedPtr<FJsonValue>> Sets =
					UnrealAIGAS_ListDerivedClasses(UAttributeSet::StaticClass(), PathPrefix);

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), Sets.Num());
				Structured->SetArrayField(TEXT("attributeSets"), Sets);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Attribute Set class(es)."), Sets.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// gas-inspect-ability — load ONE Gameplay Ability class and report its read-only design-time
		// configuration off the class default object: tags, cost/cooldown gameplay effects, and the
		// instancing policy. Required-input: a defensive Error on a missing/unresolvable class path.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("gas-inspect-ability"))
			.Title(TEXT("Inspect Gameplay Ability"))
			.Description(TEXT("Inspects a single Gameplay Ability class (read-only) by loading its class default "
			                  "object and reporting its configuration: ability/cancel/block gameplay tags, the cost "
			                  "and cooldown Gameplay Effect classes, and the instancing policy. Returns { path, name, "
			                  "instancingPolicy, abilityTags[], cancelAbilitiesWithTag[], blockAbilitiesWithTag[], "
			                  "costGameplayEffect, cooldownGameplayEffect }."))
			.ParamString(TEXT("path"), TEXT("Class path of the Gameplay Ability, e.g. '/Game/Abilities/GA_Fireball.GA_Fireball_C' "
			                                "or a native '/Script/...' class path (as returned by gas-list-abilities)."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/Abilities/GA_Fireball.GA_Fireball_C')."));
				}

				UClass* AbilityClass = UnrealAIGAS_LoadAbilityClass(Path);
				if (!AbilityClass)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Gameplay Ability class found at '%s'."), *Path));
				}

				UGameplayAbility* AbilityCDO = AbilityClass->GetDefaultObject<UGameplayAbility>();
				if (!AbilityCDO)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Class '%s' has no Gameplay Ability default object."), *Path));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Path);
				Structured->SetStringField(TEXT("name"), AbilityClass->GetName());
				Structured->SetStringField(TEXT("instancingPolicy"),
					UnrealAIGAS_InstancingPolicyToString(AbilityCDO->GetInstancingPolicy()));

				Structured->SetArrayField(TEXT("abilityTags"),
					UnrealAIGAS_TagsToJsonArray(AbilityCDO->GetAssetTags()));

				if (const FGameplayTagContainer* CancelTags =
						UnrealAIGAS_ReadTagProperty(AbilityCDO, TEXT("CancelAbilitiesWithTag")))
				{
					Structured->SetArrayField(TEXT("cancelAbilitiesWithTag"), UnrealAIGAS_TagsToJsonArray(*CancelTags));
				}
				if (const FGameplayTagContainer* BlockTags =
						UnrealAIGAS_ReadTagProperty(AbilityCDO, TEXT("BlockAbilitiesWithTag")))
				{
					Structured->SetArrayField(TEXT("blockAbilitiesWithTag"), UnrealAIGAS_TagsToJsonArray(*BlockTags));
				}

				const UGameplayEffect* CostGE = AbilityCDO->GetCostGameplayEffect();
				const UGameplayEffect* CooldownGE = AbilityCDO->GetCooldownGameplayEffect();
				Structured->SetStringField(TEXT("costGameplayEffect"), CostGE ? CostGE->GetName() : FString());
				Structured->SetStringField(TEXT("cooldownGameplayEffect"), CooldownGE ? CooldownGE->GetName() : FString());

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Gameplay Ability '%s' (%s)."),
						*AbilityClass->GetName(),
						*UnrealAIGAS_InstancingPolicyToString(AbilityCDO->GetInstancingPolicy())), Structured);
			});

		// -------------------------------------------------------------------------------------
		// gas-grant-ability — grant a Gameplay Ability to a named actor's AbilitySystemComponent in
		// the active editor world. Mutating (destructive + open-world hints). Defensive at every
		// step: missing inputs, no editor world, actor not found, no ASC, unresolvable class, or a
		// non-authoritative ASC each yield a well-formed Error before GiveAbility is ever reached.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("gas-grant-ability"))
			.Title(TEXT("Grant Gameplay Ability"))
			.Description(TEXT("Grants a Gameplay Ability to a named actor's AbilitySystemComponent in the active "
			                  "editor world (the actor must already own an AbilitySystemComponent). Returns "
			                  "{ actor, abilityClass, level, handle }."))
			.ParamString(TEXT("actor"), TEXT("Name or editor label of the target actor in the active editor world."),
				EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("abilityClass"), TEXT("Class path of the Gameplay Ability to grant, e.g. "
			                                        "'/Game/Abilities/GA_Fireball.GA_Fireball_C'."),
				EUnrealMcpParamRequirement::Required)
			.ParamInt(TEXT("level"), TEXT("Ability level to grant at. Defaults to 1."))
			.DestructiveHint(true)
			.OpenWorldHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ActorName = Call.GetString(TEXT("actor")).TrimStartAndEnd();
				if (ActorName.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'actor' (the target actor's name or editor label)."));
				}
				const FString AbilityClassPath = Call.GetString(TEXT("abilityClass")).TrimStartAndEnd();
				if (AbilityClassPath.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'abilityClass' (e.g. '/Game/Abilities/GA_Fireball.GA_Fireball_C')."));
				}

				if (!GEditor)
				{
					return FUnrealMcpToolResult::Error(TEXT("No editor (GEditor) is available."));
				}
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT("No active editor world."));
				}

				AActor* Actor = UnrealAIGAS_FindActorByName(World, ActorName);
				if (!Actor)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No actor named '%s' found in the active editor world."), *ActorName));
				}

				UAbilitySystemComponent* Asc = UnrealAIGAS_GetASC(Actor);
				if (!Asc)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent to grant the ability to."), *ActorName));
				}
				if (!Asc->IsOwnerActorAuthoritative())
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Actor '%s' AbilitySystemComponent is not authoritative; cannot grant abilities."), *ActorName));
				}

				UClass* AbilityClass = UnrealAIGAS_LoadAbilityClass(AbilityClassPath);
				if (!AbilityClass)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No Gameplay Ability class found at '%s'."), *AbilityClassPath));
				}

				const int32 Level = Call.Has(TEXT("level")) ? static_cast<int32>(Call.GetInt(TEXT("level"))) : 1;
				const FGameplayAbilitySpec Spec(AbilityClass, Level);
				const FGameplayAbilitySpecHandle Handle = Asc->GiveAbility(Spec);
				if (!Handle.IsValid())
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Failed to grant '%s' to actor '%s'."), *AbilityClass->GetName(), *ActorName));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("actor"), ActorName);
				Structured->SetStringField(TEXT("abilityClass"), AbilityClass->GetName());
				Structured->SetNumberField(TEXT("level"), Level);
				Structured->SetStringField(TEXT("handle"), Handle.ToString());
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Granted '%s' to actor '%s' at level %d."),
						*AbilityClass->GetName(), *ActorName, Level), Structured);
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
class FUnrealAIGASModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeUnique<FUnrealAIGASProvider>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
		UE_LOG(LogUnrealAIGAS, Log, TEXT("[UnrealAIGAS] registered MCP tool provider '%s'."), *Provider->GetExtensionId());
	}

	virtual void ShutdownModule() override
	{
		if (Provider.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
			Provider.Reset();
			UE_LOG(LogUnrealAIGAS, Log, TEXT("[UnrealAIGAS] unregistered MCP tool provider."));
		}
	}

private:
	TUniquePtr<FUnrealAIGASProvider> Provider;
};

IMPLEMENT_MODULE(FUnrealAIGASModule, UnrealAIGAS)
