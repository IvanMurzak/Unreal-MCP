// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIGameplayAbility.h"
#include "UnrealAIGASAttributes.h"
#include "UnrealAIGASRqlApi.h"

#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "UObject/UObjectIterator.h"

namespace UnrealAIGASModuleLocal
{
	TSharedPtr<FJsonValue> JsonList(TArray<TSharedPtr<FJsonValue>> Rows)
	{
		return MakeShared<FJsonValueArray>(MoveTemp(Rows));
	}

	TSharedPtr<FJsonValue> TagList(const FGameplayTagContainer& Tags)
	{
		TArray<TSharedPtr<FJsonValue>> Names;
		for (const FGameplayTag& Tag : Tags)
			Names.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		return JsonList(MoveTemp(Names));
	}

	/** The Ability System Components of the running game (Play In Editor), or of the editor world when it is not running. */
	TArray<UObject*> GameAbilitySystems()
	{
		UWorld* World = GEditor && GEditor->PlayWorld ? GEditor->PlayWorld.Get() : UnrealMcpRql::EditorWorld();
		TArray<UObject*> Out;
		for (TObjectIterator<UAbilitySystemComponent> It; It; ++It)
		{
			if (IsValid(*It) && !It->IsTemplate() && It->GetWorld() == World && World)
				Out.Add(*It);
		}
		return Out;
	}

	TSharedPtr<FJsonValue> GrantedAbilities(UObject* Object)
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
				Entry->SetBoolField(TEXT("Active"), Spec.IsActive());
				Granted.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		return JsonList(MoveTemp(Granted));
	}

	TSharedPtr<FJsonValue> Attributes(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		if (const UAbilitySystemComponent* Asc = Cast<UAbilitySystemComponent>(Object))
		{
			for (const UAttributeSet* Set : Asc->GetSpawnedAttributes())
			{
				if (!Set)
					continue;
				for (const FGameplayAttribute& Attribute : UnrealAIGAS::AttributesOf(Set->GetClass()))
				{
					TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("Name"), Attribute.GetName());
					Row->SetStringField(TEXT("Set"), Set->GetClass()->GetName());
					Row->SetNumberField(TEXT("Current"), Asc->GetNumericAttribute(Attribute));
					Row->SetNumberField(TEXT("Base"), Asc->GetNumericAttributeBase(Attribute));
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
		}
		return JsonList(MoveTemp(Rows));
	}

	TSharedPtr<FJsonValue> ActiveEffects(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		const UAbilitySystemComponent* Asc = Cast<UAbilitySystemComponent>(Object);
		const UWorld* World = Asc ? Asc->GetWorld() : nullptr;
		if (Asc && World)
		{
			for (const FActiveGameplayEffectHandle& Handle : Asc->GetActiveEffects(FGameplayEffectQuery()))
			{
				const FActiveGameplayEffect* Active = Asc->GetActiveGameplayEffect(Handle);
				if (!Active || !Active->Spec.Def)
					continue;
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("Effect"), Active->Spec.Def->GetClass()->GetName());
				Row->SetNumberField(TEXT("Duration"), Active->GetDuration());
				Row->SetNumberField(TEXT("Remaining"), Active->GetDuration() > 0.f ? Active->GetTimeRemaining(World->GetTimeSeconds()) : -1.f);
				Row->SetNumberField(TEXT("Period"), Active->GetPeriod());
				Row->SetNumberField(TEXT("Stacks"), Active->Spec.GetStackCount());
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		return JsonList(MoveTemp(Rows));
	}

	/** UGameplayEffect::GetStackingType is not exported: read the property, as the Details panel does. */
	int64 StackingType(const UGameplayEffect& Effect)
	{
		const FProperty* Property = FindFProperty<FProperty>(UGameplayEffect::StaticClass(), TEXT("StackingType"));
		return Property ? *Property->ContainerPtrToValuePtr<uint8>(&Effect) : 0;
	}

	FString MagnitudeText(const FGameplayEffectModifierMagnitude& Magnitude)
	{
		float Value = 0.f;
		if (Magnitude.GetStaticMagnitudeIfPossible(1.f, Value))
			return FString::Printf(TEXT("%g"), Value);
		if (Magnitude.GetSetByCallerFloat().DataTag.IsValid())
			return FString::Printf(TEXT("SetByCaller(%s)"), *Magnitude.GetSetByCallerFloat().DataTag.ToString());
		return Magnitude.GetValueForEditorDisplay().ToString();
	}

	/** An effect's class default in one line per aspect: what CreateEffect / AddModifier / SetEffectTags / SetStacking set. */
	TSharedPtr<FJsonValue> EffectSummary(UObject* Object)
	{
		const UGameplayEffect* Effect = Cast<UGameplayEffect>(Object);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!Effect)
			return MakeShared<FJsonValueObject>(Row);
		FString Duration = StaticEnum<EGameplayEffectDurationType>()->GetNameStringByValue(static_cast<int64>(Effect->DurationPolicy));
		float Seconds = 0.f;
		if (Effect->DurationPolicy == EGameplayEffectDurationType::HasDuration && Effect->DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, Seconds))
			Duration = FString::Printf(TEXT("%gs"), Seconds);
		Row->SetStringField(TEXT("Duration"), Duration);
		Row->SetNumberField(TEXT("Period"), Effect->Period.GetValueAtLevel(1.f));
		TArray<TSharedPtr<FJsonValue>> Modifiers;
		for (const FGameplayModifierInfo& Modifier : Effect->Modifiers)
		{
			const TCHAR* OpName = Modifier.ModifierOp == EGameplayModOp::Additive ? TEXT("Add") : Modifier.ModifierOp == EGameplayModOp::Multiplicitive ? TEXT("Multiply")
				: Modifier.ModifierOp == EGameplayModOp::Division ? TEXT("Divide") : Modifier.ModifierOp == EGameplayModOp::Override ? TEXT("Override") : TEXT("Other");
			Modifiers.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s %s %s"),
				Modifier.Attribute.IsValid() ? *UnrealAIGAS::QualifiedName(Modifier.Attribute) : TEXT("(none)"), OpName, *MagnitudeText(Modifier.ModifierMagnitude))));
		}
		Row->SetField(TEXT("Modifiers"), JsonList(MoveTemp(Modifiers)));
		Row->SetField(TEXT("GrantedTags"), TagList(Effect->GetGrantedTags()));
		Row->SetField(TEXT("AssetTags"), TagList(Effect->GetAssetTags()));
		Row->SetStringField(TEXT("Stacking"), StaticEnum<EGameplayEffectStackingType>()->GetNameStringByValue(StackingType(*Effect)));
		return MakeShared<FJsonValueObject>(Row);
	}

	/** An ability's class default: cost, cooldown and tags (as SetCost / SetCooldown / SetAbilityTags set them), projectile settings. */
	TSharedPtr<FJsonValue> AbilitySummary(UObject* Object)
	{
		UGameplayAbility* Ability = Cast<UGameplayAbility>(Object);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!Ability)
			return MakeShared<FJsonValueObject>(Row);
		const UGameplayEffect* Cost = Ability->GetCostGameplayEffect();
		const UGameplayEffect* Cooldown = Ability->GetCooldownGameplayEffect();
		Row->SetStringField(TEXT("Cost"), Cost ? Cost->GetClass()->GetName() : FString(TEXT("None")));
		Row->SetStringField(TEXT("Cooldown"), Cooldown ? Cooldown->GetClass()->GetName() : FString(TEXT("None")));
		Row->SetField(TEXT("CooldownTags"), Ability->GetCooldownTags() ? TagList(*Ability->GetCooldownTags()) : TagList(FGameplayTagContainer()));
		Row->SetField(TEXT("AbilityTags"), TagList(Ability->GetAssetTags()));
		if (const UUnrealAIGameplayAbility* Data = Cast<UUnrealAIGameplayAbility>(Ability))
		{
			Row->SetStringField(TEXT("Projectile"), Data->ProjectileClass ? Data->ProjectileClass->GetName() : FString(TEXT("None")));
			Row->SetStringField(TEXT("HitEffect"), Data->HitEffect ? Data->HitEffect->GetName() : FString(TEXT("None")));
			Row->SetNumberField(TEXT("HitMagnitude"), Data->HitMagnitude);
			TArray<TSharedPtr<FJsonValue>> SelfEffects;
			for (const TSubclassOf<UGameplayEffect>& Effect : Data->EffectsOnSelf)
				SelfEffects.Add(MakeShared<FJsonValueString>(Effect ? Effect->GetName() : FString(TEXT("None"))));
			Row->SetField(TEXT("EffectsOnSelf"), JsonList(MoveTemp(SelfEffects)));
		}
		return MakeShared<FJsonValueObject>(Row);
	}
}

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): the Gameplay Ability System is authored, queried and
 * tested with rql-query / rql-mutate instead of dedicated tools. Reflection exposes the ability / effect / attribute-set
 * properties on their class defaults; the module adds the class-default sources, the Ability System Components of the
 * running game, the `gas` operations (UnrealAIGASRqlApi.h), members reflection does not show (granted abilities,
 * attribute values, owned tags, active effects, effect and ability summaries) and the agent guide rendered into the
 * unreal-rql skill. The runtime classes it configures (UnrealAIAttributeSet, UnrealAIAbilitySystemComponent,
 * UnrealAIGameplayAbility, UnrealAIGASProjectile, UnrealAIAttributeTextComponent) are in the UnrealAIGASRuntime module.
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
		using namespace UnrealAIGASModuleLocal;
		Context.AddClassDefaultsSource(TEXT("gasAbilities"),
			TEXT("the class defaults of every Gameplay Ability class (native and the project's Blueprints), at most 200"),
			UGameplayAbility::StaticClass());
		Context.AddClassDefaultsSource(TEXT("gasEffects"),
			TEXT("the class defaults of every Gameplay Effect class (native and the project's Blueprints), at most 200"),
			UGameplayEffect::StaticClass());
		Context.AddClassDefaultsSource(TEXT("gasAttributeSets"),
			TEXT("the class defaults of every Attribute Set class (native and the project's Blueprints), at most 200"),
			UAttributeSet::StaticClass());
		Context.AddObjectListSource(TEXT("gasComponents"),
			TEXT("the Ability System Components of the running game (Play In Editor), or of the editor world when it is not running: Attributes, OwnedTags, ActiveEffects, GrantedAbilities"),
			UAbilitySystemComponent::StaticClass(), []() { return GameAbilitySystems(); });
		Context.AddApiSource(TEXT("gas"),
			TEXT("Gameplay Ability System authoring and testing: call EnableGameplayAbilities / CreateEffect / SetEffectDuration / AddModifier / RemoveModifiers / SetEffectTags / SetStacking / CreateAbility / SetCost / SetCooldown / SetAbilityTags / SetAbilityProjectile / SetDefault / AddAbilitySystem / AddStartingAbility / AddStartingEffect / BindAbilityInput / AddAttributeDisplay / GrantAbility / ActivateAbility / ApplyEffect / SetAttribute"),
			UUnrealAIGASRqlApi::StaticClass(), TEXT("GASOperations"));

		Context.AddJsonMember(UAbilitySystemComponent::StaticClass(), TEXT("GrantedAbilities"),
			TEXT("the abilities granted to this component: Ability (class), Level, Handle, Active"),
			[](UObject* Object) { return GrantedAbilities(Object); });
		Context.AddJsonMember(UAbilitySystemComponent::StaticClass(), TEXT("Attributes"),
			TEXT("the attributes of this component's attribute sets (created when the game runs): Name, Set, Current, Base"),
			[](UObject* Object) { return Attributes(Object); });
		Context.AddJsonMember(UAbilitySystemComponent::StaticClass(), TEXT("OwnedTags"),
			TEXT("the gameplay tags the owner has now (from active effects such as cooldowns, and running abilities)"),
			[](UObject* Object)
			{
				const UAbilitySystemComponent* Asc = Cast<UAbilitySystemComponent>(Object);
				return TagList(Asc ? Asc->GetOwnedGameplayTags() : FGameplayTagContainer());
			});
		Context.AddJsonMember(UAbilitySystemComponent::StaticClass(), TEXT("ActiveEffects"),
			TEXT("the effects active on this component: Effect (class), Duration (-1 infinite), Remaining (s), Period, Stacks"),
			[](UObject* Object) { return ActiveEffects(Object); });
		Context.AddJsonMember(UGameplayEffect::StaticClass(), TEXT("Summary"),
			TEXT("the effect at a glance: Duration, Period, Modifiers (\"Set.Attribute Op Magnitude\"), GrantedTags, AssetTags, Stacking"),
			[](UObject* Object) { return EffectSummary(Object); });
		Context.AddJsonMember(UGameplayAbility::StaticClass(), TEXT("Summary"),
			TEXT("the ability at a glance: Cost, Cooldown, CooldownTags, AbilityTags (and Projectile, HitEffect, HitMagnitude, EffectsOnSelf of an UnrealAIGameplayAbility)"),
			[](UObject* Object) { return AbilitySummary(Object); });

		Context.AddGuide(TEXT("Gameplay Ability System"), TEXT(R"MD(Author the Gameplay Ability System with the `gas` operations: **no project C++, no editor rebuild**. This extension's runtime
module ships what GAS otherwise needs C++ for, configured with data:

- `UnrealAIAttributeSet`: Health / MaxHealth, Mana / MaxMana, Stamina / MaxStamina (kept within 0 ... Max) and the meta
  attributes Damage and Healing (an effect adding to Damage subtracts it from Health). All start at 100.
- `UnrealAIAbilitySystemComponent` (`AddAbilitySystem`): creates the attribute sets with initial values, grants
  StartingAbilities, applies StartingEffects (regeneration), binds abilities to Enhanced Input actions for the player,
  and fires OnAttributeChanged / OnOutOfHealth for Blueprints. Actors need no C++ interface.
- `UnrealAIGameplayAbility` (`CreateAbility`'s default parent): on activation commits cost and cooldown (fails when it
  cannot pay or is cooling down), applies EffectsOnSelf, fires ProjectileClass, which applies HitEffect to what it hits
  with HitMagnitude as SetByCaller.Damage. No ActivateAbility graph needed.
- `UnrealAIGASProjectile` (a straight-flying sphere), `UnrealAIAttributeTextComponent` (`AddAttributeDisplay`: "HP {Health}").

A fireball (20 mana, 3 s cooldown, 25 damage on hit), mana regeneration, and a training dummy that shows its health:

```
gas | call CreateEffect("/Game/GAS/GE_Damage")                                 instant
gas | call AddModifier("/Game/GAS/GE_Damage", "Damage", "Add", "SetByCaller")   the amount comes from the ability
gas | call CreateEffect("/Game/GAS/GE_ManaRegen", "Infinite", 1)                infinite, executes every 1 s
gas | call AddModifier("/Game/GAS/GE_ManaRegen", "Mana", "Add", "5")
gas | call CreateAbility("/Game/GAS/GA_Fireball")
gas | call SetCost("/Game/GAS/GA_Fireball", "Mana", 20)                         makes GA_Fireball_Cost (Mana -20)
gas | call SetCooldown("/Game/GAS/GA_Fireball", 3)                              makes GA_Fireball_Cooldown granting Cooldown.Fireball
gas | call SetAbilityTags("/Game/GAS/GA_Fireball", "Ability", "Ability.Fireball")
gas | call SetAbilityProjectile("/Game/GAS/GA_Fireball", "/Game/GAS/GE_Damage", 25)
gas | call AddAbilitySystem("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter", "Health=100, MaxHealth=100, Mana=100, MaxMana=100")
gas | call AddStartingEffect("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter", "/Game/GAS/GE_ManaRegen")
gas | call BindAbilityInput("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter", "/Game/GAS/GA_Fireball", "/Game/GAS/IA_Fireball", "One")
gas | call AddAttributeDisplay("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter", "HP {Health}  MP {Mana}")
gas | call AddAbilitySystem("/Game/GAS/BP_Dummy", "Health=200, MaxHealth=200")      BP_Dummy: blueprint-create, parent Character
gas | call AddAttributeDisplay("/Game/GAS/BP_Dummy", "Dummy {Health} / {MaxHealth}")
editor | call SpawnActor("/Game/GAS/BP_Dummy", "Dummy", "(600, 0, 100)")
editor | call SaveAll()
```

Check and test in the running game (`ActivateAbility` / `ApplyEffect` / `SetAttribute` need Play In Editor; `""` is
the player's pawn):

```
gasAbilities | where $type == "GA_Fireball_C" | select Summary
gasEffects | where $type contains "GE_" | select $type Summary
editor | set IsPlaying = true
gas | call ActivateAbility("/Game/GAS/GA_Fireball")                  "activated: Mana 100 -> 80"
gas | call ActivateAbility("/Game/GAS/GA_Fireball")                  "not activated: on cooldown (Cooldown.Fireball, 2.9s left)"
gas | call ApplyEffect("/Game/GAS/GE_Damage", "Dummy", "Damage=25")   "GE_Damage_C applied to Dummy: Health 200 -> 175"
gasComponents | select Owner.Label Attributes OwnedTags ActiveEffects
editor | set IsPlaying = false
```

- **More abilities are data**: a heal is `CreateEffect` GE_Heal + `AddModifier(..., "Healing", "Add", "30")`,
  `CreateAbility` GA_Heal, `SetCost`, `SetCooldown`, `SetDefault("/Game/GAS/GA_Heal", "EffectsOnSelf", "/Game/GAS/GE_Heal")`,
  `BindAbilityInput(..., "Two")`. Custom logic: give the ability Blueprint an ActivateAbility event graph (Blueprint
  graph calls: `AddEvent("K2_ActivateAbility")`, then CommitAbility / EndAbility); a graph replaces the data-driven activation.
- **Effects**: Duration `"Instant"` (changes the base value once), `"Infinite"` or seconds (active while it lasts;
  with a Period it executes every Period seconds: regeneration, damage over time). Magnitudes: numbers, `"SetByCaller"`
  (SetByCaller.Damage) or `"SetByCaller.Heal"`, `"Attribute(Source.MaxMana, 0.05)"`. `SetEffectTags(effect, "Granted",
  "State.Burning")` (the target has the tags while active), `"BlockAbilities"`, `"ApplicationRequired"`;
  `SetStacking(effect, "AggregateByTarget", 3)`.
- **Tags** are text (`"A.B, C.D"`); missing tags are added to the project's tag ini (`gameplayTags` lists them).
- **Your own attribute set** works too: `AddAbilitySystem(blueprint, "Armor=5", "MyAttributeSet")`; attribute names
  are `"Mana"` or `"MyAttributeSet.Mana"`.
- Component changes recompile the Blueprint; effects, abilities and input assets stay unsaved until `editor | call SaveAll()`.
- Abilities a character should have in the game: `AddStartingAbility` / `BindAbilityInput` (granted at BeginPlay).
  `GrantAbility(actor, ability, level)` only grants to a placed actor of the editor world (not saved, not in Play In Editor).)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIGASRqlModule>, UnrealAIGAS)
