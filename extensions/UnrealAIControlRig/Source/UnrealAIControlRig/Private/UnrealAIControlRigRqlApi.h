// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIControlRigRqlApi.generated.h"

class UControlRigBlueprint;

/**
 * The `controlRig` RQL source (type "ControlRigOperations"): author Control Rig blueprints, e.g.
 * `controlRig | call CreateRig("/Game/Rigs/CR_Arm")`. Every UFUNCTION here is callable from rql-mutate (Unreal-MCP's
 * IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see. BlueprintCallable only so UHT records
 * the C++ default arguments; BlueprintInternalUseOnly keeps them out of menus.
 */
UCLASS(Transient)
class UUnrealAIControlRigRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** create an empty Control Rig blueprint at Path ("/Game/Rigs/CR_Arm"); add bones, controls and solvers to it with AddBone / AddControl / AddTwoBoneIK; returns it (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	UControlRigBlueprint* CreateRig(const FString& Path);

	/** add a bone named Name to a rig under the bone Parent ("" for a root), at Location relative to the parent (cm); returns the bone's name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddBone(UControlRigBlueprint* Rig, const FString& Name, const FString& Parent, FVector Location);

	/** add an animator control named Name to a rig at Location (cm; rig space, or relative to Parent when one is given: a control, null or bone, "" for none), as a transform control the animator moves; returns the control's name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddControl(UControlRigBlueprint* Rig, const FString& Name, FVector Location, const FString& Parent = TEXT(""));

	/** make a two-bone chain (BoneA -> BoneB -> Effector, e.g. upper arm, forearm, hand) follow the control Target with a Basic IK solver run on Forwards Solve; returns the IK node's name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddTwoBoneIK(UControlRigBlueprint* Rig, const FString& BoneA, const FString& BoneB, const FString& Effector, const FString& Target);
};
