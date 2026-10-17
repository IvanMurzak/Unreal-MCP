// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAINiagaraRqlApi.generated.h"

class AActor;
class UNiagaraSystem;

/**
 * The `niagara` RQL source (type "NiagaraOperations"): Niagara operations without an owner object, e.g.
 * `niagara | call SpawnSystem("/Game/VFX/NS_Fire", "(0, 0, 100)", "Campfire")`. Every UFUNCTION here is callable from
 * rql-mutate (Unreal-MCP's IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see.
 * BlueprintCallable only so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAINiagaraRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** place a Niagara system in the current level as a Niagara actor at Location (Label optional, undoable); returns the actor */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	AActor* SpawnSystem(UNiagaraSystem* System, FVector Location = FVector::ZeroVector, const FString& Label = TEXT(""),
		bool bAutoActivate = true);
};
