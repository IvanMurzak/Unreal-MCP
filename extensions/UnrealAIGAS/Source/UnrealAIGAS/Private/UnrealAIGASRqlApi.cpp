// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIGASRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Actor.h"
#include "GameplayAbilitySpec.h"

FString UUnrealAIGASRqlApi::GrantAbility(AActor* Actor, TSubclassOf<UGameplayAbility> Ability, int32 Level)
{
	using UnrealMcpRql::Failed;
	UAbilitySystemComponent* Asc = nullptr;
	if (const IAbilitySystemInterface* WithAsc = Cast<IAbilitySystemInterface>(Actor))
		Asc = WithAsc->GetAbilitySystemComponent();
	if (!Asc)
		Asc = Actor->FindComponentByClass<UAbilitySystemComponent>();
	if (!Asc)
		return Failed<FString>(FString::Printf(TEXT("%s has no AbilitySystemComponent"), *Actor->GetActorNameOrLabel()));
	if (!Asc->IsOwnerActorAuthoritative())
		return Failed<FString>(FString::Printf(TEXT("the AbilitySystemComponent of %s is not authoritative"), *Actor->GetActorNameOrLabel()));
	const FGameplayAbilitySpecHandle Handle = Asc->GiveAbility(FGameplayAbilitySpec(Ability, FMath::Max(Level, 1)));
	if (!Handle.IsValid())
		return Failed<FString>(FString::Printf(TEXT("could not grant %s"), *Ability->GetName()));
	return Handle.ToString();
}
