// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "InputActionValue.h"
#include "UObject/Object.h"
#include "UnrealMcpRqlPlay.h"

#include "UnrealAIEnhancedInputRqlApi.generated.h"

class APlayerController;
class UEdGraphNode;
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

	/** map a key ("SpaceBar", "W", "Gamepad_FaceButton_Bottom") to an action in a mapping context; Modifiers (optional, comma-separated, applied in order) shape that key's value: "Negate", "Swizzle" (X and Y swapped), "DeadZone", "Scalar", "Smooth", or another InputModifier class name. WASD into a 2D Move action: W "Swizzle", S "Swizzle, Negate", A "Negate", D none (Gamepad_Left2D needs none) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	bool AddMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Modifiers = TEXT(""));

	/** remove every mapping of a key to an action from a mapping context; returns how many were removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	int32 RemoveMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key);

	/** An InputModifier class as the operations and KeyMappings name it: "Negate" for InputModifierNegate. */
	static FString ModifierDisplayName(const UClass* Class);

	/** add an Enhanced Input action event for Action to an actor Blueprint's event graph (the Blueprint, its path, or unreal.asset(path).EventGraph), as the Blueprint editor's "EnhancedInputAction IA_Jump" node: exec pins Triggered, Started, Ongoing, Canceled, Completed and the ActionValue; wire it with ref(node) | call Connect("Triggered", ref(other), "execute"); fires on the pawn the player controls once its mapping context is added (AddMappingContextOnBeginPlay); returns the node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* AddActionEvent(UObject* Graph, UInputAction* Action);

	/** make an actor Blueprint (or its path, or its event graph) add a mapping context when play starts: Event BeginPlay -> Get Player Controller (0) -> Enhanced Input Local Player Subsystem -> Add Mapping Context (Context, Priority), placed before what BeginPlay already runs; without it no action event fires; returns the Add Mapping Context node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* AddMappingContextOnBeginPlay(UObject* Blueprint, UInputMappingContext* Context, int32 Priority = 0);

	/** play test: inject Action (its path) into player 0 of the running Play In Editor session for Seconds and / or Frames (fixed 1/60 s steps, simulated INSIDE this call; at most 60 s), as if its keys were held: Value is the raw value (1 presses a Boolean action; [0, 1, 0] is a 2D Move forward; X, Y, Z for Axis1D / 2D / 3D); Triggers (optional, comma-separated: "Pressed", "Hold", "Tap", ...) add triggers as a key mapping's would; released on one extra frame; returns the play state, its Notes listing the action's trigger events frame by frame ("IA_Jump: Triggered x30, Completed x1 (30 frame(s) held + 1 released)") and its value on the last held frame. Start the session first: editor | call StartPlay() */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealMcpPlayState InjectInput(UInputAction* Action, FVector Value = FVector(1.0, 0.0, 0.0), double Seconds = 0.0, int32 Frames = 0,
		const FString& Triggers = TEXT(""));

	/** The Enhanced Input mapping contexts applied to @p Controller's player, highest priority first ("IMC_Default (priority 0)"). */
	static TArray<FString> AppliedContexts(const APlayerController* Controller);
};
