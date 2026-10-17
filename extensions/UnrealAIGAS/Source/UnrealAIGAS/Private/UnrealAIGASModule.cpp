// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIGASRqlApi.h"

#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AttributeSet.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIGAS, Log, All);

namespace UnrealAIGASRqlLocal
{
	/** Most classes one source lists (Blueprint classes load when the source is read). */
	constexpr int32 MaxClasses = 200;

	/**
	 * The class default objects of every concrete subclass of @p Base — native ones and the project's Blueprint classes
	 * (found through the Asset Registry, so classes that are not loaded yet are included), sorted by class name.
	 */
	TArray<UObject*> ClassDefaults(UClass* Base)
	{
		TSet<UClass*> Classes;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class == Base || !Class->IsChildOf(Base) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				continue;
			const FString Name = Class->GetName();
			if (Name.StartsWith(TEXT("SKEL_")) || Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASHCLASS_")))
				continue;
			Classes.Add(Class);
		}
		const IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TSet<FTopLevelAssetPath> Derived;
		Registry.GetDerivedClassNames({ Base->GetClassPathName() }, TSet<FTopLevelAssetPath>(), Derived);
		for (const FTopLevelAssetPath& Path : Derived)
		{
			if (Classes.Num() >= MaxClasses)
				break;
			if (Path == Base->GetClassPathName() || !Path.GetPackageName().ToString().StartsWith(TEXT("/Game")))
				continue;
			if (UClass* Class = Cast<UClass>(UnrealMcpRql::LoadObject(UClass::StaticClass(), Path.ToString())))
			{
				if (!Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
					Classes.Add(Class);
			}
		}
		TArray<UClass*> Sorted = Classes.Array();
		Sorted.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
		TArray<UObject*> Defaults;
		for (UClass* Class : Sorted)
		{
			if (Defaults.Num() >= MaxClasses)
				break;
			if (UObject* Default = Class->GetDefaultObject())
				Defaults.Add(Default);
		}
		return Defaults;
	}
}

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): the Gameplay Ability System is queried and changed
 * with rql-query / rql-mutate instead of dedicated tools. Reflection exposes the ability / effect / attribute-set
 * properties on their class defaults (tags, instancing policy, cost and cooldown effects, attributes, ...); the module
 * adds the class-default sources, the `gas` operations, an AbilitySystemComponent's granted abilities (not a
 * reflected property) and the agent guide rendered into the unreal-rql skill.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName (scripts/extension_release.py bump updates both).
 */
class FUnrealAIGASRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("GAS"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-gas"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.1"); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		using namespace UnrealAIGASRqlLocal;
		Context.AddObjectListSource(TEXT("gasAbilities"),
			TEXT("the class defaults of every Gameplay Ability class (native and the project's Blueprints), at most 200"),
			UGameplayAbility::StaticClass(), []() { return ClassDefaults(UGameplayAbility::StaticClass()); });
		Context.AddObjectListSource(TEXT("gasEffects"),
			TEXT("the class defaults of every Gameplay Effect class (native and the project's Blueprints), at most 200"),
			UGameplayEffect::StaticClass(), []() { return ClassDefaults(UGameplayEffect::StaticClass()); });
		Context.AddObjectListSource(TEXT("gasAttributeSets"),
			TEXT("the class defaults of every Attribute Set class (native and the project's Blueprints), at most 200"),
			UAttributeSet::StaticClass(), []() { return ClassDefaults(UAttributeSet::StaticClass()); });
		Context.AddApiClass(UUnrealAIGASRqlApi::StaticClass(), TEXT("GASOperations"));
		Context.AddObjectSource(TEXT("gas"), TEXT("Gameplay Ability System operations: call GrantAbility(actor, abilityClass, level)"),
			UUnrealAIGASRqlApi::StaticClass(), []() -> UObject* { return GetMutableDefault<UUnrealAIGASRqlApi>(); });
		Context.AddJsonMember(UAbilitySystemComponent::StaticClass(), TEXT("GrantedAbilities"),
			TEXT("the abilities granted to this component: Ability (class), Level, Handle"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Granted;
				if (const UAbilitySystemComponent* Asc = Cast<UAbilitySystemComponent>(Object))
				{
					for (const FGameplayAbilitySpec& Spec : Asc->GetActivatableAbilities())
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Ability"), Spec.Ability ? Spec.Ability->GetClass()->GetName() : FString());
						Entry->SetNumberField(TEXT("Level"), Spec.Level);
						Entry->SetStringField(TEXT("Handle"), Spec.Handle.ToString());
						Granted.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				return MakeShared<FJsonValueArray>(Granted);
			});
		Context.AddGuide(TEXT("Gameplay Ability System"), TEXT(R"MD(Abilities, effects and attribute sets are classes: their sources list the class defaults, whose members are the
reflected properties (`$type` is the class). Ability System Components are components:
`actors | get Components | where $ is AbilitySystemComponent`.

```
gasAbilities | select $type InstancingPolicy NetExecutionPolicy CostGameplayEffectClass CooldownGameplayEffectClass
gasEffects | select $type DurationPolicy Period
gasAttributeSets | select $type
actors | get Components | where $ is AbilitySystemComponent | select Owner.Label GrantedAbilities
gas | call GrantAbility("Hero", "GA_Fireball_C", 2)          actor (label, name or ref), ability class, level
```

Blueprint ability classes are named with a `_C` suffix (GA_Fireball_C) or by their asset path ("/Game/Abilities/GA_Fireball").
Granting needs an actor with an authoritative AbilitySystemComponent; it changes the editor-world actor only.)MD"));
	}
};

/** Editor module that owns the RQL module and registers it as a modular feature, so Unreal-MCP discovers it. */
class FUnrealAIGASModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RqlModule = MakeUnique<FUnrealAIGASRqlModule>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
		UE_LOG(LogUnrealAIGAS, Log, TEXT("[UnrealAIGAS] registered RQL module '%s'."), *RqlModule->GetRqlModuleName());
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
	TUniquePtr<FUnrealAIGASRqlModule> RqlModule;
};

IMPLEMENT_MODULE(FUnrealAIGASModule, UnrealAIGAS)
