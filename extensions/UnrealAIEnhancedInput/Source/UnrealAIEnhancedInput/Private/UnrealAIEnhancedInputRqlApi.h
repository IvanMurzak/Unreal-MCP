// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "InputActionValue.h"
#include "UObject/Object.h"

#include "UnrealAIEnhancedInputRqlApi.generated.h"

class UEdGraphNode;
class UInputAction;
class UInputMappingContext;

/**
 * The `enhancedInput` RQL source (type "EnhancedInputOperations"): Enhanced Input authoring without an owner object,
 * e.g. `enhancedInput | call CreateAction("/Game/Input/IA_Jump", "Boolean")`. Every UFUNCTION here is callable from
 * rql-mutate (Unreal-MCP's IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see.
 * BlueprintCallable only so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps them out of menus.
 * Triggers and modifiers are text lists (UnrealAIEnhancedInputText.h): "Hold(0.5), Pressed", "DeadZone(0.2), Negate(Y)".
 */
UCLASS(Transient)
class UUnrealAIEnhancedInputRqlApi : public UObject
{
	GENERATED_BODY()

public:
	// ------------------------------------------------------------------ assets

	/** create an Input Action asset at Path ("/Game/Input/IA_Jump") with a value type (Boolean, Axis1D, Axis2D, Axis3D) and optional Triggers / Modifiers as text ("Hold(0.5)", "DeadZone(0.2)"); returns it (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UInputAction* CreateAction(const FString& Path, EInputActionValueType ValueType = EInputActionValueType::Boolean, const FString& Triggers = TEXT(""), const FString& Modifiers = TEXT(""));

	/** create an Input Mapping Context asset at Path ("/Game/Input/IMC_OnFoot"); one per control scheme, added at runtime with a priority (AddMappingContextOnPossess / OnEvent); returns it (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UInputMappingContext* CreateContext(const FString& Path);

	// ------------------------------------------------------------------ mappings

	/** map a key ("SpaceBar", "W", "Gamepad_FaceButton_Bottom", "Gamepad_Left2D") to an action in a mapping context; Modifiers and Triggers (optional text lists) apply to this key only: WASD into a 2D Move action: W "Swizzle", S "Swizzle, Negate", A "Negate", D none; a Hold just for this key: Triggers "Hold(0.4)" */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	bool AddMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Modifiers = TEXT(""), const FString& Triggers = TEXT(""));

	/** remove every mapping of a key to an action from a mapping context; returns how many were removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	int32 RemoveMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key);

	/** replace the triggers of the mappings of Key to Action in Context (text list, "" = none: the action's triggers decide); returns how many mappings changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	int32 SetMappingTriggers(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Triggers);

	/** replace the modifiers of the mappings of Key to Action in Context (text list in order, "" = none); returns how many mappings changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	int32 SetMappingModifiers(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Modifiers);

	// ------------------------------------------------------------------ actions

	/** replace an action's triggers (every key mapped to it): "Pressed", "Released", "Down", "Hold(0.5)", "HoldAndRelease(0.5)", "Tap(0.2)", "Pulse(0.25)", "ChordAction(/Game/Input/IA_Modifier)", "Combo(/Game/Input/IA_A, /Game/Input/IA_B)", "" = none (fires while actuated); returns them as text */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	FString SetActionTriggers(UInputAction* Action, const FString& Triggers);

	/** replace an action's modifiers (applied after each mapping's own): "DeadZone(0.2)", "Scalar(2)", "Negate", "Negate(Y)", "Swizzle", "Smooth", "ToWorldSpace", "ResponseCurveExponential(2)", "" = none; returns them as text */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	FString SetActionModifiers(UInputAction* Action, const FString& Modifiers);

	// ------------------------------------------------------------------ player-mappable keys (rebinding)

	/** let players rebind the keys of an action (its Player Mappable Key Settings): DisplayName and DisplayCategory are what a key-binding menu shows (defaults: the action name, ""), Name the unique name bindings are saved under (default: the action's asset name); every mapping of the action inherits it; returns Name. Rebinding at runtime also needs EnableUserSettings */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	FString SetPlayerMappable(UInputAction* Action, const FString& DisplayName = TEXT(""), const FString& DisplayCategory = TEXT(""), const FString& Name = TEXT(""));

	/** choose where one key mapping's player-mappable settings come from: Mode "Override" (its own: DisplayName, DisplayCategory, Name, default Name <Action>_<Key>), "Inherit" (the action's), "Ignore" (never rebindable); returns how many mappings changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	int32 SetMappingPlayerMappable(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Mode = TEXT("Override"),
		const FString& DisplayName = TEXT(""), const FString& DisplayCategory = TEXT(""), const FString& Name = TEXT(""));

	/** turn the Enhanced Input user settings on (Project Settings > Engine > Enhanced Input > Enable User Settings, saved to DefaultInput.ini): each local player gets UEnhancedInputUserSettings, which saves the player's rebound keys; returns what changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	FString EnableUserSettings(bool Enable = true);

	// ------------------------------------------------------------------ Blueprint wiring

	/** add an Enhanced Input action event for Action to an actor Blueprint's event graph (the Blueprint, its path, or unreal.asset(path).EventGraph), as the Blueprint editor's "EnhancedInputAction IA_Jump" node: exec pins Triggered, Started, Ongoing, Canceled, Completed and the ActionValue (BindAction wires them in one call); one per action; returns the node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* AddActionEvent(UObject* Graph, UInputAction* Action);

	/** when Action fires Event (Started, Triggered, Ongoing, Canceled, Completed) call Function of the Blueprint (its function or custom event, inherited such as Jump / StopJumping / Crouch / UnCrouch, or "Class.Function"), after what that pin already runs; the action event is added when missing; Value: the parameter that gets the ActionValue ("" = the first of the action's value type, for axis actions; "none"); returns the call node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* BindAction(UObject* Blueprint, UInputAction* Action, const FString& Event, const FString& Function, const FString& Value = TEXT(""));

	/** a toggle: each time Action fires Event (default Started: each press) it alternately calls OnFunction and OffFunction (a FlipFlop), e.g. crouch on C: BindActionToggle(bp, IA_Crouch, "Crouch", "UnCrouch"); returns the FlipFlop node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* BindActionToggle(UObject* Blueprint, UInputAction* Action, const FString& OnFunction, const FString& OffFunction, const FString& Event = TEXT("Started"));

	/** make an actor Blueprint (or its path, or its event graph) add a mapping context when play starts: Event BeginPlay -> Get Player Controller (0) -> Enhanced Input Local Player Subsystem -> Add Mapping Context (Context, Priority), placed before what BeginPlay already runs; without it no action event fires; returns the Add Mapping Context node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* AddMappingContextOnBeginPlay(UObject* Blueprint, UInputMappingContext* Context, int32 Priority = 0);

	/** a pawn's own control scheme: a Pawn / Character / vehicle Blueprint adds Context (with Priority) to the player controlling it while it is possessed and removes it when unpossessed (On Controller Changed: works in multiplayer too), so entering a vehicle swaps on-foot for vehicle controls; returns the Add Mapping Context node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* AddMappingContextOnPossess(UObject* Blueprint, UInputMappingContext* Context, int32 Priority = 0);

	/** add Context (with Priority) when Event fires: "BeginPlay", "Possessed" / "Unpossessed" / "ControllerChanged" (pawns), "OnPossess" / "OnUnpossess" (controllers), or a custom event (found, or created): the subsystem comes from the event's controller, the PlayerController Blueprint itself, the pawn's controller, or player 0; returns the Add Mapping Context node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* AddMappingContextOnEvent(UObject* Blueprint, UInputMappingContext* Context, const FString& Event, int32 Priority = 0);

	/** remove Context when Event fires (the events of AddMappingContextOnEvent); the context's keys stay registered for rebinding; returns the Remove Mapping Context node */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Enhanced Input", meta = (BlueprintInternalUseOnly = "true"))
	UEdGraphNode* RemoveMappingContextOnEvent(UObject* Blueprint, UInputMappingContext* Context, const FString& Event);
};
