// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "UObject/Object.h"

#include "UnrealAIGASRqlApi.generated.h"

class AActor;
class UActorComponent;
class UAttributeSet;
class UBlueprint;
class UGameplayAbility;
class UGameplayEffect;
class UInputMappingContext;

/**
 * The `gas` RQL source (type "GASOperations"): Gameplay Ability System authoring and testing without project C++, e.g.
 * `gas | call CreateEffect("/Game/GAS/GE_ManaRegen", "Infinite", 1)`. Every UFUNCTION here is callable from rql-mutate
 * (Unreal-MCP's IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see. BlueprintCallable only
 * so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps them out of Blueprint menus.
 *
 * Effects and abilities are Blueprint classes; their parameters take the asset path ("/Game/GAS/GE_Damage") or class
 * name (GE_Damage_C). Tags are text ("Cooldown.Fireball" or "A.B, C.D"); tags that do not exist yet are added to the
 * project's tag ini. Attributes are names ("Mana", or "UnrealAIAttributeSet.Mana" when two sets share one).
 */
UCLASS(Transient)
class UUnrealAIGASRqlApi : public UObject
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------- setup

	/** make the project file enable the GameplayAbilities plugin (this extension already loads it; the project then keeps it for game code and packaging); not undoable; returns what changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString EnableGameplayAbilities();

	// ---------------------------------------------------------------- effects

	/** create a Gameplay Effect Blueprint at Path ("/Game/GAS/GE_ManaRegen"): Duration "Instant", "Infinite" or seconds ("3"); Period (seconds, 0 = none) makes a duration / infinite effect execute its modifiers periodically (regeneration, damage over time); returns the Blueprint (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	UBlueprint* CreateEffect(const FString& Path, const FString& Duration = TEXT("Instant"), float Period = 0.f);

	/** set an effect's Duration ("Instant", "Infinite" or seconds) and Period (seconds, 0 = none; bExecuteOnApplication: also execute when applied); returns the new settings */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString SetEffectDuration(TSubclassOf<UGameplayEffect> Effect, const FString& Duration, float Period = 0.f, bool bExecuteOnApplication = true);

	/** add a modifier to Effect: Attribute ("Mana", "Damage", "UnrealAIAttributeSet.Health"), Operation "Add" | "Multiply" | "Divide" | "Override", Magnitude "-20", "SetByCaller" (SetByCaller.Damage) or "SetByCaller.<Tag>", or "Attribute(Source.AttackPower, 1.5)" (Source / Target attribute times a coefficient); returns the modifier count */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	int32 AddModifier(TSubclassOf<UGameplayEffect> Effect, const FString& Attribute, const FString& Operation = TEXT("Add"), const FString& Magnitude = TEXT("0"));

	/** remove the modifiers of Effect on Attribute ("" = every modifier); returns how many were removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	int32 RemoveModifiers(TSubclassOf<UGameplayEffect> Effect, const FString& Attribute = TEXT(""));

	/** set a tag list of Effect (replacing it; "" clears): Kind "Granted" (the target has these tags while the effect is active, e.g. a cooldown tag), "Asset" (tags the effect itself has), "BlockAbilities" (abilities with these tags cannot activate while active), "ApplicationRequired" / "ApplicationBlocked" (the target must have / must not have them to receive it); returns the tags */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString SetEffectTags(TSubclassOf<UGameplayEffect> Effect, const FString& Kind, const FString& Tags);

	/** set how Effect stacks: Type "None", "AggregateBySource", "AggregateByTarget"; Limit (0 = none); bRefreshDuration / bResetPeriod on each new stack; returns the settings */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString SetStacking(TSubclassOf<UGameplayEffect> Effect, const FString& Type = TEXT("AggregateByTarget"), int32 Limit = 0,
		bool bRefreshDuration = true, bool bResetPeriod = false);

	// ---------------------------------------------------------------- abilities

	/** create a Gameplay Ability Blueprint at Path ("/Game/GAS/GA_Fireball") from Parent (default UnrealAIGameplayAbility: commits cost and cooldown, applies EffectsOnSelf, spawns ProjectileClass carrying HitEffect, no graph needed); returns the Blueprint (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	UBlueprint* CreateAbility(const FString& Path, TSubclassOf<UGameplayAbility> Parent = nullptr);

	/** make Ability cost Amount of Attribute ("Mana") per activation: creates or updates the instant cost effect Effect (default "<ability path>_Cost") and sets it as the ability's cost; activation fails when the attribute is too low; returns the effect path */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString SetCost(TSubclassOf<UGameplayAbility> Ability, const FString& Attribute, float Amount, const FString& Effect = TEXT(""));

	/** give Ability a cooldown of Seconds: creates or updates the duration effect Effect (default "<ability path>_Cooldown") granting Tag (default "Cooldown.<ability name>", added to the project's tags) and sets it as the ability's cooldown; returns the effect path and tag */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString SetCooldown(TSubclassOf<UGameplayAbility> Ability, float Seconds, const FString& Tag = TEXT(""), const FString& Effect = TEXT(""));

	/** set a tag list of Ability (replacing it; "" clears): Kind "Ability" (its own tags), "ActivationOwned" (the owner has them while it runs), "ActivationRequired", "ActivationBlocked", "CancelAbilitiesWithTag", "BlockAbilitiesWithTag"; returns the tags */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString SetAbilityTags(TSubclassOf<UGameplayAbility> Ability, const FString& Kind, const FString& Tags);

	/** make an UnrealAIGameplayAbility fire a projectile that applies HitEffect with Magnitude as its SetByCaller.Damage: Projectile (default UnrealAIGASProjectile, a sphere; or a Blueprint child of it), Speed (cm/s, 0 = the projectile's own); returns the settings */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString SetAbilityProjectile(TSubclassOf<UGameplayAbility> Ability, TSubclassOf<UGameplayEffect> HitEffect, float Magnitude = 0.f,
		TSubclassOf<AActor> Projectile = nullptr, float Speed = 0.f);

	/** set a class default (a property of an ability, effect, projectile or other Blueprint class of the project) from text: numbers, enum entries, true / false, asset or class paths, tag lists "A.B, C.D", lists of assets or classes "/Game/GAS/GE_Heal, /Game/GAS/GE_Haste", UE struct text "(X=1,Y=2,Z=3)"; a failed conversion changes nothing; returns the new value */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString SetDefault(UClass* Class, const FString& Property, const FString& Value);

	// ---------------------------------------------------------------- actors

	/** give a Blueprint actor or character an UnrealAIAbilitySystemComponent ("AbilitySystem"; kept when it has one) with AttributeSet (default UnrealAIAttributeSet: Health, MaxHealth, Mana, MaxMana, Stamina, MaxStamina, Damage, Healing; another set replaces it on a new component and is added to an existing one) and initial values Attributes ("Health=100, MaxHealth=100, Mana=50"; merged); compiles it; returns the component template (its members are writable) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	UActorComponent* AddAbilitySystem(UBlueprint* Blueprint, const FString& Attributes = TEXT(""), TSubclassOf<UAttributeSet> AttributeSet = nullptr);

	/** grant Ability when an actor of Blueprint begins play (it needs AddAbilitySystem first); returns the starting abilities */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString AddStartingAbility(UBlueprint* Blueprint, TSubclassOf<UGameplayAbility> Ability);

	/** apply Effect to an actor of Blueprint when it begins play (e.g. an infinite periodic regeneration effect); returns the starting effects */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString AddStartingEffect(UBlueprint* Blueprint, TSubclassOf<UGameplayEffect> Effect);

	/** activate Ability with the Enhanced Input action InputAction (an asset path; created when missing) for the player controlling a pawn of Blueprint; Key ("One", "Q", "LeftMouseButton", "Gamepad_FaceButton_Top") is mapped in MappingContext (default "<action folder>/IMC_Abilities", created when missing, added for the player at BeginPlay); grants the ability; returns the binding */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString BindAbilityInput(UBlueprint* Blueprint, TSubclassOf<UGameplayAbility> Ability, const FString& InputAction, const FString& Key = TEXT(""),
		UInputMappingContext* MappingContext = nullptr);

	/** show the owner's attributes as world text on actors of Blueprint while the game runs: Format "HP {Health} / {MaxHealth}", Offset from the root (cm); returns the component template */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	UActorComponent* AddAttributeDisplay(UBlueprint* Blueprint, const FString& Format = TEXT("HP {Health} / {MaxHealth}"), FVector Offset = FVector(0.f, 0.f, 120.f));

	/** grant a Gameplay Ability class to a placed actor's AbilitySystemComponent (the actor needs one) at Level, in the editor world only: not saved and not copied into Play In Editor (use AddStartingAbility / BindAbilityInput for that); returns the spec handle */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString GrantAbility(AActor* Actor, TSubclassOf<UGameplayAbility> Ability, int32 Level = 1);

	// ---------------------------------------------------------------- the running game (Play In Editor)

	/** activate Ability on Actor in the running game (a label or name; "" = the player's pawn; needs Play In Editor); returns "activated" or why not (cooldown, cost, tags, not granted) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString ActivateAbility(TSubclassOf<UGameplayAbility> Ability, const FString& Actor = TEXT(""));

	/** apply Effect to Actor in the running game ("" = the player's pawn) at Level; SetByCaller "Damage=25" (SetByCaller.Damage) or "<Tag>=<value>, ..."; returns the attribute changes */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString ApplyEffect(TSubclassOf<UGameplayEffect> Effect, const FString& Actor = TEXT(""), const FString& SetByCaller = TEXT(""), float Level = 1.f);

	/** set the base value of Attribute ("Mana") on Actor in the running game ("" = the player's pawn); returns the new current value */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	float SetAttribute(const FString& Attribute, float Value, const FString& Actor = TEXT(""));
};
