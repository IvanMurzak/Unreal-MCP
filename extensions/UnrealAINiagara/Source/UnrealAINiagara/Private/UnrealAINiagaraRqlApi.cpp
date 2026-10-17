// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAINiagaraRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "Editor.h"
#include "Engine/World.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

AActor* UUnrealAINiagaraRqlApi::SpawnSystem(UNiagaraSystem* System, FVector Location, const FString& Label, bool bAutoActivate)
{
	if (!System)
	{
		UnrealMcpRql::Fail(TEXT("no Niagara system (pass niagaraSystems | ... or its path \"/Game/VFX/NS_Fire\")"));
		return nullptr;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UnrealMcpRql::Fail(TEXT("no editor world"));
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transactional; // recorded by the RQL mutation's transaction: Ctrl+Z removes it
	ANiagaraActor* Actor = World->SpawnActor<ANiagaraActor>(ANiagaraActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
	UNiagaraComponent* Component = Actor ? Actor->FindComponentByClass<UNiagaraComponent>() : nullptr;
	if (!Component)
	{
		if (Actor)
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not place %s"), *System->GetName()));
		return nullptr;
	}
	Component->SetAutoActivate(bAutoActivate);
	Component->SetAsset(System);
	if (!Label.IsEmpty())
		FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
	return Actor;
}
