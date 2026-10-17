// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAITemplateRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "GameFramework/Actor.h"

FString UUnrealAITemplateRqlApi::Greet(const FString& Name)
{
	return FString::Printf(TEXT("Hello, %s! - from the Unreal-MCP extension sample."), *Name);
}

bool UUnrealAITemplateRqlApi::Relabel(AActor* Actor, const FString& Label)
{
	// Object parameters are required (the call is refused for null); validate the rest before changing anything: a failing
	// call reports, and the RQL mutation changes nothing more.
	if (Label.TrimStartAndEnd().IsEmpty())
		return UnrealMcpRql::Failed<bool>(TEXT("a non-empty label is required"));
	Actor->Modify();
	Actor->SetActorLabel(Label);
	return true;
}
