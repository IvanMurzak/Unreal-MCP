// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAINiagaraRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "Engine/World.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

AActor* UUnrealAINiagaraRqlApi::SpawnSystem(UNiagaraSystem* System, FVector Location, const FString& Label, bool bAutoActivate)
{
	// The core spawn (current level, transactional, labelled): Ctrl+Z removes it with the rest of the mutation.
	AActor* Actor = UnrealMcpRql::SpawnActor(ANiagaraActor::StaticClass(), Location, FRotator::ZeroRotator, Label);
	if (!Actor)
		return nullptr;
	UNiagaraComponent* Component = Actor->FindComponentByClass<UNiagaraComponent>();
	if (!Component)
	{
		Actor->GetWorld()->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
		return UnrealMcpRql::Failed<AActor*>(FString::Printf(TEXT("could not place %s"), *System->GetName()));
	}
	Component->SetAutoActivate(bAutoActivate);
	Component->SetAsset(System);
	return Actor;
}
