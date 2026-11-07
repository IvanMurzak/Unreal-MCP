// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIGameplayAbility.h"

#include "UnrealAIGASAttributes.h"
#include "UnrealAIGASProjectile.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameplayEffect.h"

UUnrealAIGameplayAbility::UUnrealAIGameplayAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
}

void UUnrealAIGameplayAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (bHasBlueprintActivate || bHasBlueprintActivateFromEvent)
	{
		// A Blueprint child's own ActivateAbility graph (it commits and ends the ability itself).
		Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
		return;
	}
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const float Level = GetAbilityLevel(Handle, ActorInfo);
	for (const TSubclassOf<UGameplayEffect>& Effect : EffectsOnSelf)
	{
		if (Effect)
			ApplyGameplayEffectToOwner(Handle, ActorInfo, ActivationInfo, Effect->GetDefaultObject<UGameplayEffect>(), Level);
	}
	AActor* Projectile = SpawnProjectile();
	OnActivated(Projectile);
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

AActor* UUnrealAIGameplayAbility::SpawnProjectile()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	UWorld* World = Avatar ? Avatar->GetWorld() : nullptr;
	if (!ProjectileClass || !World || !Avatar->HasAuthority())
		return nullptr;

	APawn* Pawn = Cast<APawn>(Avatar);
	FRotator Aim = Avatar->GetActorRotation();
	if (bAimWithControlRotation && Pawn && Pawn->GetController())
		Aim = Pawn->GetControlRotation();
	const FVector Location = Avatar->GetActorLocation() + FRotator(0.f, Aim.Yaw, 0.f).RotateVector(ProjectileOffset);
	const FTransform Transform(Aim, Location);

	AActor* Projectile = World->SpawnActorDeferred<AActor>(ProjectileClass, Transform, Avatar, Pawn, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Projectile)
		return nullptr;
	if (AUnrealAIGASProjectile* Typed = Cast<AUnrealAIGASProjectile>(Projectile))
	{
		if (HitEffect)
			Typed->HitEffectSpec = MakeHitEffectSpec();
		if (ProjectileSpeed > 0.f && Typed->Movement)
		{
			Typed->Movement->InitialSpeed = ProjectileSpeed;
			Typed->Movement->MaxSpeed = ProjectileSpeed;
		}
	}
	Projectile->FinishSpawning(Transform);
	return Projectile;
}

FGameplayEffectSpecHandle UUnrealAIGameplayAbility::MakeHitEffectSpec() const
{
	if (!HitEffect)
		return FGameplayEffectSpecHandle();
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(HitEffect, GetAbilityLevel());
	if (Spec.IsValid())
		Spec.Data->SetSetByCallerMagnitude(TAG_UnrealAI_SetByCaller_Damage, HitMagnitude);
	return Spec;
}
