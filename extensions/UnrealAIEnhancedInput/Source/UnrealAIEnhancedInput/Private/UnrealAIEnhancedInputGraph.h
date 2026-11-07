// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UInputAction;
class UInputMappingContext;
class UObject;

/**
 * Enhanced Input wiring in actor Blueprints (Pawn, Character, PlayerController, ...): the nodes the Blueprint editor
 * places by hand, created and connected the way it does. Every function validates before it changes the graph and
 * reports failures with UnrealMcpRql::Fail (then returns null).
 */
namespace UnrealAIEnhancedInputGraph
{
	/** The actor Blueprint and event graph @p Target names (the Blueprint, its path or one of its event graphs); null after Fail. */
	UEdGraph* EventGraphOf(UObject* Target, UBlueprint*& OutBlueprint);

	/** One Add / Remove Mapping Context call to place after an event. */
	struct FContextStep
	{
		bool bAdd = true;
		UInputMappingContext* Context = nullptr;
		int32 Priority = 0;
		/** The event's controller parameter the step uses (NewController / OldController); None: the Blueprint's usual player controller. */
		FName ControllerParameter;
	};

	/**
	 * Run @p Steps first when @p Event fires: "BeginPlay", "Possessed", "Unpossessed", "ControllerChanged", "OnPossess",
	 * "OnUnpossess" (controllers), another overridable event of the parent class, or a custom event (found, or created).
	 * Each step gets the local player's Enhanced Input subsystem from the event's controller parameter (cast to a
	 * PlayerController; when the cast fails the step is skipped), from the PlayerController Blueprint itself, from the
	 * pawn's controller, or from player 0. What the event ran before runs after the steps. Returns the last call node.
	 */
	UEdGraphNode* AddContextSteps(UBlueprint* Blueprint, UEdGraph* EventGraph, const FString& Event, TArrayView<const FContextStep> Steps);

	/** The Add / Remove Mapping Context call of @p Blueprint for @p Context (on @p OnlyEvent when given), with the event it runs on; null when none. */
	UEdGraphNode* FindContextCall(UBlueprint* Blueprint, const UInputMappingContext* Context, bool bAdd, const FString& OnlyEvent = FString(), FString* OutEvent = nullptr);

	/** The Blueprint's action event for @p Action, or a new one (the caller marks the Blueprint modified); null after Fail. @p bOutCreated tells which. */
	UEdGraphNode* FindOrAddActionEvent(UBlueprint* Blueprint, UEdGraph* EventGraph, UInputAction* Action, bool& bOutCreated);

	/**
	 * Call @p Function (of the Blueprint, a custom event, inherited such as Jump, or "Class.Function") when @p Action fires
	 * @p TriggerEvent (Started, Triggered, Ongoing, Canceled, Completed), after what that pin already runs. @p ValueParameter
	 * names the parameter that gets the ActionValue ("" = the first parameter of the action's value type, for axis
	 * actions; "none" = no value). Returns the call node.
	 */
	UEdGraphNode* BindAction(UBlueprint* Blueprint, UEdGraph* EventGraph, UInputAction* Action, const FString& TriggerEvent,
		const FString& Function, const FString& ValueParameter);

	/** @p TriggerEvent alternates between calling @p OnFunction and @p OffFunction (a FlipFlop): a toggle. Returns the FlipFlop. */
	UEdGraphNode* BindActionToggle(UBlueprint* Blueprint, UEdGraph* EventGraph, UInputAction* Action, const FString& TriggerEvent,
		const FString& OnFunction, const FString& OffFunction);

	/** The Blueprint's Enhanced Input setup: {Contexts: [{Event, Op, Context, Priority}], Actions: [{Action, Started: [...], ...}]}. */
	TSharedPtr<FJsonValue> InputSetupJson(const UBlueprint* Blueprint);
}
