// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIPCGRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "GameFramework/Actor.h"
#include "PCGComponent.h"
#include "PCGGraph.h"

UPCGComponent* UUnrealAIPCGRqlApi::AddComponent(AActor* Actor, UPCGGraph* Graph)
{
	if (!Actor)
	{
		UnrealMcpRql::Fail(TEXT("no actor (a label, name or ref)"));
		return nullptr;
	}
	Actor->Modify();
	UPCGComponent* Component = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(), NAME_None, RF_Transactional);
	if (!Component)
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not add a PCG component to %s"), *Actor->GetActorNameOrLabel()));
		return nullptr;
	}
	Actor->AddInstanceComponent(Component);
	Component->RegisterComponent();
	if (Graph)
		Component->SetGraph(Graph);
	return Component;
}

bool UUnrealAIPCGRqlApi::Generate(AActor* Actor)
{
	UPCGComponent* Component = Actor ? Actor->FindComponentByClass<UPCGComponent>() : nullptr;
	if (!Component)
	{
		UnrealMcpRql::Fail(Actor ? FString::Printf(TEXT("%s has no PCG component"), *Actor->GetActorNameOrLabel()) : FString(TEXT("no actor")));
		return false;
	}
	Component->Generate();
	return true;
}
