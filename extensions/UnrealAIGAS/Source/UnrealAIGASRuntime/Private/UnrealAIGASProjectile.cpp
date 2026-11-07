// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIGASProjectile.h"

#include "UnrealAIGASAttributes.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameplayEffect.h"
#include "UObject/ConstructorHelpers.h"

AUnrealAIGASProjectile::AUnrealAIGASProjectile()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicatingMovement(true);
	InitialLifeSpan = 5.f;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(15.f);
	Collision->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	Collision->SetCanEverAffectNavigation(false);
	RootComponent = Collision;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetRelativeScale3D(FVector(0.3f));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded())
		Mesh->SetStaticMesh(Sphere.Object);

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->UpdatedComponent = Collision;
	Movement->InitialSpeed = 2000.f;
	Movement->MaxSpeed = 2000.f;
	Movement->ProjectileGravityScale = 0.f;
	Movement->bRotationFollowsVelocity = true;
	Movement->bShouldBounce = false;
}

void AUnrealAIGASProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (AActor* Shooter = GetInstigator())
		Collision->IgnoreActorWhenMoving(Shooter, true);
	if (AActor* Shooter = GetOwner())
		Collision->IgnoreActorWhenMoving(Shooter, true);
	Movement->OnProjectileStop.AddDynamic(this, &AUnrealAIGASProjectile::HandleStop);
}

void AUnrealAIGASProjectile::HandleStop(const FHitResult& Hit)
{
	if (!HasAuthority())
		return;
	AActor* Other = Hit.GetActor();
	if (Other && Other != GetInstigator() && Other != GetOwner())
		ApplyHitEffect(Other);
	OnProjectileHit(Other, Hit);
	if (bDestroyOnHit)
		Destroy();
}

bool AUnrealAIGASProjectile::ApplyHitEffect(AActor* Target)
{
	UAbilitySystemComponent* TargetAsc = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Target);
	if (!TargetAsc)
		return false;
	AActor* Shooter = GetInstigator() ? static_cast<AActor*>(GetInstigator()) : GetOwner();
	UAbilitySystemComponent* SourceAsc = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Shooter);

	FGameplayEffectSpecHandle Spec = HitEffectSpec;
	if (!Spec.IsValid() && HitEffect)
	{
		UAbilitySystemComponent* Maker = SourceAsc ? SourceAsc : TargetAsc;
		FGameplayEffectContextHandle Context = Maker->MakeEffectContext();
		Context.AddInstigator(Shooter, this);
		Context.AddSourceObject(this);
		Spec = Maker->MakeOutgoingSpec(HitEffect, 1.f, Context);
		if (Spec.IsValid())
			Spec.Data->SetSetByCallerMagnitude(TAG_UnrealAI_SetByCaller_Damage, HitMagnitude);
	}
	if (!Spec.IsValid())
		return false;
	if (SourceAsc)
		SourceAsc->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetAsc);
	else
		TargetAsc->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	return true;
}
