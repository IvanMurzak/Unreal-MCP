// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayEffectTypes.h"
#include "Templates/SubclassOf.h"

#include "UnrealAIGASProjectile.generated.h"

class UGameplayEffect;
class UProjectileMovementComponent;
class USphereComponent;
class UStaticMeshComponent;

/**
 * A projectile that applies a Gameplay Effect to the first actor with an Ability System Component it hits, then is
 * destroyed (on any blocking hit). A sphere that flies straight (no gravity); restyle it in a Blueprint child (mesh,
 * material, a Niagara component) or change its components' defaults.
 */
UCLASS(Blueprintable, meta = (DisplayName = "Projectile (Unreal AI GAS)"))
class UNREALAIGASRUNTIME_API AUnrealAIGASProjectile : public AActor
{
	GENERATED_BODY()

public:
	AUnrealAIGASProjectile();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Projectile")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Projectile")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Projectile")
	TObjectPtr<UProjectileMovementComponent> Movement;

	/** The effect to apply on hit (set by the ability that spawns it); when invalid, HitEffect is used */
	UPROPERTY(BlueprintReadWrite, Category = "Projectile", meta = (ExposeOnSpawn = "true"))
	FGameplayEffectSpecHandle HitEffectSpec;

	/** The effect to apply on hit when no spec was given (from the instigator's Ability System Component) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	TSubclassOf<UGameplayEffect> HitEffect;

	/** The SetByCaller.Damage magnitude for HitEffect */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	float HitMagnitude = 0.f;

	/** Destroy the projectile on its first blocking hit */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	bool bDestroyOnHit = true;

	/** Runs on the server when the projectile hits something (HitActor may have no Ability System Component) */
	UFUNCTION(BlueprintImplementableEvent, Category = "Projectile")
	void OnProjectileHit(AActor* HitActor, const FHitResult& Hit);

	/** Apply the hit effect to Target's Ability System Component (the effect context carries Hit and this projectile); false when it has none */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Projectile")
	bool ApplyHitEffect(AActor* Target, const FHitResult& Hit);

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION()
	void HandleStop(const FHitResult& Hit);
};
