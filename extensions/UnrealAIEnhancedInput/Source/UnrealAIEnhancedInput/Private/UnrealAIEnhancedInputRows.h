// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "UObject/Object.h"

#include "UnrealAIEnhancedInputRows.generated.h"

struct FEnhancedActionKeyMapping;
class UInputAction;
class UInputMappingContext;

/** One row of the `inputMappings` source: a key mapping of a project mapping context, with what shapes and exposes it. */
UCLASS(Transient)
class UUnrealAIEnhancedInputMappingRow : public UObject
{
	GENERATED_BODY()

public:
	/** the mapping context's asset name, e.g. IMC_OnFoot */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString Context;

	/** the mapping context's path, e.g. /Game/Input/IMC_OnFoot */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString ContextPath;

	/** the action's asset name, e.g. IA_Jump */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString Action;

	/** the action's path */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString ActionPath;

	/** the key (FKey name): SpaceBar, Gamepad_FaceButton_Bottom, Gamepad_Left2D */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString Key;

	/** the mapping's own triggers as text, as SetMappingTriggers takes them (e.g. "Hold(HoldTimeThreshold=0.5)"); the action's triggers apply too */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString Triggers;

	/** the mapping's own modifiers as text, in order (e.g. "SwizzleAxis, Negate"); the action's modifiers apply after them */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString Modifiers;

	/** whether players can rebind this mapping (Enhanced Input user settings) */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	bool PlayerMappable = false;

	/** where its player-mappable settings come from: Inherit (the action's), Override (its own), Ignore (never mappable) */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString Settings;

	/** the unique name the player's binding is saved under (empty when not player mappable) */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString MappingName;

	/** the name a key-binding menu shows */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString DisplayName;

	/** the key-binding menu's category, e.g. On Foot, Vehicle */
	UPROPERTY(VisibleAnywhere, Category = "Mapping")
	FString DisplayCategory;
};

/** One row of the `inputTriggerTypes` / `inputModifierTypes` sources: a trigger or modifier class agents can name. */
UCLASS(Transient)
class UUnrealAIEnhancedInputTypeRow : public UObject
{
	GENERATED_BODY()

public:
	/** the name the operations take, e.g. Hold, HoldAndRelease, ChordAction, DeadZone, SwizzleAxis */
	UPROPERTY(VisibleAnywhere, Category = "Type")
	FString Name;

	/** Trigger or Modifier */
	UPROPERTY(VisibleAnywhere, Category = "Type")
	FString Kind;

	/** the class, e.g. InputTriggerHold */
	UPROPERTY(VisibleAnywhere, Category = "Type")
	FString Class;

	/** its settings with their defaults, the main (positional) one first: HoldTimeThreshold=1 (first argument), bIsOneShot=false, ... */
	UPROPERTY(VisibleAnywhere, Category = "Type")
	FString Settings;

	/** what it does (the class tooltip) */
	UPROPERTY(VisibleAnywhere, Category = "Type")
	FString Description;
};

namespace UnrealAIEnhancedInputRows
{
	/** The rows of every key mapping of the project's mapping contexts (at most 200 contexts, loaded), by context then order. */
	TArray<UObject*> Mappings();

	/** The rows of the trigger (@p bTriggers) or modifier classes. */
	TArray<UObject*> Types(bool bTriggers);

	/** A mapping as the KeyMappings member lists it: Action, Key, Triggers / Modifiers (when any), PlayerMappable settings (when mappable). */
	TSharedPtr<FJsonValue> MappingJson(const FEnhancedActionKeyMapping& Mapping);

	/** An action's player-mappable settings as JSON ({Name, DisplayName, DisplayCategory}), or null when it is not player mappable. */
	TSharedPtr<FJsonValue> PlayerMappableJson(const UInputAction* Action);

	/** "Inherit" / "Override" / "Ignore": where a mapping's player-mappable settings come from. */
	FString SettingBehavior(const FEnhancedActionKeyMapping& Mapping);
}
