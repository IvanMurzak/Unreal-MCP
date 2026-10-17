// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "UObject/Object.h"

#include "UnrealAIGASRqlApi.generated.h"

class AActor;
class UGameplayAbility;

/**
 * The `gas` RQL source (type "GASOperations"): Gameplay Ability System operations without an owner object, e.g.
 * `gas | call GrantAbility("Hero", "GA_Fireball_C")`. Every UFUNCTION here is callable from rql-mutate (Unreal-MCP's
 * IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see. BlueprintCallable only so UHT
 * records the C++ default arguments; BlueprintInternalUseOnly keeps them out of Blueprint menus.
 */
UCLASS(Transient)
class UUnrealAIGASRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** grant a Gameplay Ability class to an actor's AbilitySystemComponent (the actor needs one) at Level; returns the spec handle */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|GAS", meta = (BlueprintInternalUseOnly = "true"))
	FString GrantAbility(AActor* Actor, TSubclassOf<UGameplayAbility> Ability, int32 Level = 1);
};
