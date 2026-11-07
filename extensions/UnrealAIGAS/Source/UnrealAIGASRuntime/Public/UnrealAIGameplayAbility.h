// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "Templates/SubclassOf.h"

#include "UnrealAIGameplayAbility.generated.h"

class UGameplayEffect;

/**
 * A Gameplay Ability configured with data, so common abilities need neither C++ nor an ActivateAbility graph. On
 * activation it commits (pays CostGameplayEffectClass, starts CooldownGameplayEffectClass; fails when it cannot), applies
 * EffectsOnSelf, spawns ProjectileClass (a UnrealAIGASProjectile carries HitEffect to what it hits) and ends.
 *
 * A Blueprint child that implements its own ActivateAbility event runs that graph instead (SpawnProjectile and
 * MakeHitEffectSpec are callable there); OnActivated runs after the data-driven activation.
 */
UCLASS(Blueprintable, meta = (DisplayName = "Gameplay Ability (Unreal AI)"))
class UNREALAIGASRUNTIME_API UUnrealAIGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UUnrealAIGameplayAbility();

	/** Effects applied to the owner on activation (a heal, a buff, ...) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Unreal AI|Self")
	TArray<TSubclassOf<UGameplayEffect>> EffectsOnSelf;

	/** An actor spawned on activation in front of the avatar, facing its aim (UnrealAIGASProjectile or a Blueprint child) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Unreal AI|Projectile")
	TSubclassOf<AActor> ProjectileClass;

	/** Where the projectile spawns relative to the avatar: X forward, Y right, Z up (cm) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Unreal AI|Projectile")
	FVector ProjectileOffset = FVector(100.f, 0.f, 40.f);

	/** The projectile's speed (cm/s); 0 keeps its own */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Unreal AI|Projectile")
	float ProjectileSpeed = 0.f;

	/** Aim along the controller's view (camera) instead of the avatar's facing */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Unreal AI|Projectile")
	bool bAimWithControlRotation = false;

	/** The effect the projectile applies to what it hits (e.g. damage that adds to UnrealAIAttributeSet.Damage) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Unreal AI|Hit")
	TSubclassOf<UGameplayEffect> HitEffect;

	/** The SetByCaller.Damage magnitude given to HitEffect (for modifiers whose magnitude is SetByCaller) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Unreal AI|Hit")
	float HitMagnitude = 0.f;

	/** Spawn ProjectileClass in front of the avatar (server); it carries the hit effect spec. Null when there is none */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Ability")
	AActor* SpawnProjectile();

	/** The spec of HitEffect from this ability, with HitMagnitude as SetByCaller.Damage */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Ability")
	FGameplayEffectSpecHandle MakeHitEffectSpec() const;

	/** Runs after the data-driven activation (Projectile is null when none was spawned) */
	UFUNCTION(BlueprintImplementableEvent, Category = "Unreal AI|Ability")
	void OnActivated(AActor* Projectile);

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
