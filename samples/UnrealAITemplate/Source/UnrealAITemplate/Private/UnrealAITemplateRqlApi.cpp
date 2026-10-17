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
	// Validate before changing anything: a failing call reports, and the RQL mutation changes nothing more.
	if (!Actor || Label.TrimStartAndEnd().IsEmpty())
	{
		UnrealMcpRql::Fail(TEXT("an actor and a non-empty label are required"));
		return false;
	}
	Actor->Modify();
	Actor->SetActorLabel(Label);
	return true;
}
