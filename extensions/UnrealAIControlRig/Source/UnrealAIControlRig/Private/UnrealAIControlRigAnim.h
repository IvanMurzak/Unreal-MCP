// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class UAnimBlueprint;
class UControlRigBlueprint;

/**
 * Control Rigs in Anim Blueprints, as the `controlRig` operations wire them: a Control Rig node in the AnimGraph (spliced
 * into the pose chain before Output Pose or after a given node), its rig variables exposed as pins and bound to Anim
 * Blueprint variables, and the event-graph logic that sets those variables every frame. Every function that fails calls
 * UnrealMcpRql::Fail and returns an empty string.
 */
namespace UnrealAIControlRigAnim
{
	/** AddToAnimBlueprint: see UUnrealAIControlRigRqlApi::AddToAnimBlueprint. Returns what it did. */
	FString AddRig(UAnimBlueprint* AnimBlueprint, UControlRigBlueprint* Rig, const FString& After, bool bBindVariables);

	/** SetRigInput: see UUnrealAIControlRigRqlApi::SetRigInput. Returns the input's new source. */
	FString SetInput(UAnimBlueprint* AnimBlueprint, const FString& Node, const FString& Input, const FString& Value);

	/** TrackNearestActor: see UUnrealAIControlRigRqlApi::TrackNearestActor. Returns what it added. */
	FString TrackNearestActor(UAnimBlueprint* AnimBlueprint, const FString& ActorClass, const FString& LocationVariable,
		const FString& WeightVariable, float Radius, float Speed);

	/** The ControlRigs member of an Anim Blueprint: its Control Rig nodes with their rig, pose neighbours and inputs. */
	TSharedPtr<FJsonValue> ControlRigsJson(UObject* Object);
}
