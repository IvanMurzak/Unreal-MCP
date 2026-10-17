// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIGASRqlApi.h"

#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): the Gameplay Ability System is queried and changed
 * with rql-query / rql-mutate instead of dedicated tools. Reflection exposes the ability / effect / attribute-set
 * properties on their class defaults (tags, instancing policy, cost and cooldown effects, attributes, ...); the module
 * adds the class-default sources, the `gas` operations, an AbilitySystemComponent's granted abilities (not a
 * reflected property) and the agent guide rendered into the unreal-rql skill.
 *
 * The version is the .uplugin VersionName.
 */
class FUnrealAIGASRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("GAS"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-gas"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAIGAS")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddClassDefaultsSource(TEXT("gasAbilities"),
			TEXT("the class defaults of every Gameplay Ability class (native and the project's Blueprints), at most 200"),
			UGameplayAbility::StaticClass());
		Context.AddClassDefaultsSource(TEXT("gasEffects"),
			TEXT("the class defaults of every Gameplay Effect class (native and the project's Blueprints), at most 200"),
			UGameplayEffect::StaticClass());
		Context.AddClassDefaultsSource(TEXT("gasAttributeSets"),
			TEXT("the class defaults of every Attribute Set class (native and the project's Blueprints), at most 200"),
			UAttributeSet::StaticClass());
		Context.AddApiSource(TEXT("gas"), TEXT("Gameplay Ability System operations: call GrantAbility(actor, abilityClass, level)"),
			UUnrealAIGASRqlApi::StaticClass(), TEXT("GASOperations"));
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

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIGASRqlModule>, UnrealAIGAS)
