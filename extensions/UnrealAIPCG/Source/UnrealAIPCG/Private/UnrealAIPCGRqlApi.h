// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIPCGRqlApi.generated.h"

class AActor;
class UPCGComponent;
class UPCGGraph;

/**
 * The `pcg` RQL source (type "PCGOperations"): PCG operations on editor-world actors, e.g.
 * `pcg | call AddComponent("Forest", "/Game/PCG/PCG_Trees")`. Every UFUNCTION here is callable from rql-mutate
 * (Unreal-MCP's IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see. BlueprintCallable
 * only so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps them out of Blueprint menus.
 */
UCLASS(Transient)
class UUnrealAIPCGRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** add a PCG component to an actor (label, name or ref), bound to a PCG graph when one is given (undoable); returns the component */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	UPCGComponent* AddComponent(AActor* Actor, UPCGGraph* Graph = nullptr);

	/** run PCG generation on the PCG component of an actor */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	bool Generate(AActor* Actor);
};
