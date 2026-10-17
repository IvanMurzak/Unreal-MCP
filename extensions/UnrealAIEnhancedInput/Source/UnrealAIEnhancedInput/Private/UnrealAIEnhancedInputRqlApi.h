// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "InputActionValue.h"
#include "UObject/Object.h"

#include "UnrealAIEnhancedInputRqlApi.generated.h"

class UInputAction;
class UInputMappingContext;

/**
 * The `enhancedInput` RQL source (type "EnhancedInputOperations"): Enhanced Input authoring without an owner object,
 * e.g. `enhancedInput | call CreateAction("/Game/Input/IA_Jump", "Boolean")`. Every UFUNCTION here is callable from
 * rql-mutate (Unreal-MCP's IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see.
 * BlueprintCallable only so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAIEnhancedInputRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** create an Input Action asset at Path ("/Game/Input/IA_Jump") with a value type (Boolean, Axis1D, Axis2D, Axis3D); returns it (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UInputAction* CreateAction(const FString& Path, EInputActionValueType ValueType = EInputActionValueType::Boolean);

	/** create an Input Mapping Context asset at Path ("/Game/Input/IMC_Default"); returns it (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UInputMappingContext* CreateContext(const FString& Path);

	/** map a key ("SpaceBar", "W", "Gamepad_FaceButton_Bottom") to an action in a mapping context */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	bool AddMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key);

	/** remove every mapping of a key to an action from a mapping context; returns how many were removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	int32 RemoveMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key);
};
