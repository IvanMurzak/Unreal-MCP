// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIPCGRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "GameFramework/Actor.h"
#include "PCGComponent.h"
#include "PCGGraph.h"

UPCGComponent* UUnrealAIPCGRqlApi::AddComponent(AActor* Actor, UPCGGraph* Graph)
{
	// The core AddComponent (instance component, transactional); the graph is bound before the component registers.
	return Cast<UPCGComponent>(UnrealMcpRql::AddComponent(Actor, UPCGComponent::StaticClass(), FString(),
		[Graph](UActorComponent* Component)
		{
			if (Graph)
				CastChecked<UPCGComponent>(Component)->SetGraph(Graph);
		}));
}

bool UUnrealAIPCGRqlApi::Generate(AActor* Actor)
{
	UPCGComponent* Component = Actor->FindComponentByClass<UPCGComponent>();
	if (!Component)
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no PCG component"), *Actor->GetActorNameOrLabel()));
	Component->Generate();
	return true;
}
