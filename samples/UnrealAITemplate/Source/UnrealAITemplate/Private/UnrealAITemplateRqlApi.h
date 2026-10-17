// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAITemplateRqlApi.generated.h"

class AActor;

/**
 * The sample's operations class: the `hello` RQL source (type "HelloOperations"). Every UFUNCTION declared here is
 * callable from rql-mutate once the module calls AddApiClass (`hello | call Greet("Ada")`); parameters and the return
 * value convert like members, the doc comment above each function is the description agents see, and a failure is
 * reported with UnrealMcpRql::Fail. BlueprintCallable only so UHT records the C++ default arguments;
 * BlueprintInternalUseOnly keeps the functions out of Blueprint menus.
 */
UCLASS(Transient)
class UUnrealAITemplateRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** a friendly greeting, addressed to Name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Template", meta = (BlueprintInternalUseOnly = "true"))
	FString Greet(const FString& Name = TEXT("world"));

	/** rename an actor's label (label, name or ref) — an operation that changes the level, undoable */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Template", meta = (BlueprintInternalUseOnly = "true"))
	bool Relabel(AActor* Actor, const FString& Label);
};
