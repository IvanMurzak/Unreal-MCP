// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIControlRigRqlApi.generated.h"

class UAnimBlueprint;
class UControlRigBlueprint;

/**
 * The `controlRig` RQL source (type "ControlRigOperations"): author Control Rig blueprints (procedural animation: look-at,
 * aim, IK, traces, springs) and wire them into Anim Blueprints, e.g. `controlRig | call CreateRig("/Game/Rigs/CR_Head",
 * "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple")`. Every UFUNCTION here is callable from rql-mutate (Unreal-MCP's
 * IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see. BlueprintCallable only so UHT records
 * the C++ default arguments; BlueprintInternalUseOnly keeps them out of menus. A rig or Anim Blueprint is passed as its
 * asset path.
 */
UCLASS(Transient)
class UUnrealAIControlRigRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** create a Control Rig blueprint at Path ("/Game/Rigs/CR_Head"); Mesh (a SkeletalMesh or Skeleton path, optional) imports its bones and curves and becomes the preview, so the rig drives that character in an Anim Blueprint; returns it (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	UControlRigBlueprint* CreateRig(const FString& Path, const FString& Mesh = TEXT(""));

	/** add a bone named Name to a rig under the bone Parent ("" for a root), at Location relative to the parent (cm); returns the bone's name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddBone(UControlRigBlueprint* Rig, const FString& Name, const FString& Parent, FVector Location);

	/** add an animator control named Name to a rig at Location (cm; rig space, or relative to Parent when one is given: a control, null or bone, "" for none), as a transform control the animator moves; returns the control's name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddControl(UControlRigBlueprint* Rig, const FString& Name, FVector Location, const FString& Parent = TEXT(""));

	/** make a two-bone chain (BoneA -> BoneB -> Effector, e.g. thigh_l, calf_l, foot_l) follow the control Target with a Basic IK solver run on Forwards Solve; returns the IK node's name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddTwoBoneIK(UControlRigBlueprint* Rig, const FString& BoneA, const FString& BoneB, const FString& Effector, const FString& Target);

	/** add a rig variable Name of Type (float, bool, int, Vector, Rotator, Transform, ...; Default as text, optional); a public one is an input of the rig's Control Rig node in Anim Blueprints (AddToAnimBlueprint binds it); read it in the graph with "@Name"; returns its name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddVariable(UControlRigBlueprint* Rig, const FString& Name, const FString& Type, const FString& Default = TEXT(""), bool bPublic = true);

	/** add a rig unit node (a name from controlRigUnits: "AimItem", "TwoBoneIKSimplePerItem", "SphereTraceWorld", "SpringInterpVectorV2", "RigUnit_ToRigSpace_Location", ...) with Settings "Pin=Value, Sub.Pin=Value" (values: "@Variable" or "@Node.Pin" links, an element name for an item pin, "(0, 1, 0)", text); a unit that executes runs at the end of Forwards Solve, or after the node After; Name optional; returns the node's name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddUnit(UControlRigBlueprint* Rig, const FString& Unit, const FString& Settings = TEXT(""), const FString& After = TEXT(""), const FString& Name = TEXT(""));

	/** set the input pin Pin ("Aim.Weight", "Aim.Primary.Target") of a rig node from Value: "@Variable" / "@Node.Pin" (link), an element name for an item pin ("head", "Control:Target"), "(0, 1, 0)", or the pin's text form; returns the new value */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString SetPin(UControlRigBlueprint* Rig, const FString& Pin, const FString& Value);

	/** link the output pin From ("FromWorld.Global") to the input pin To ("Aim.Primary.Target"), or an execute output to a node's execute input; returns "From -> To" */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString Connect(UControlRigBlueprint* Rig, const FString& From, const FString& To);

	/** remove the node Node (name or title) from the rig graph; the execution chain through it is reconnected */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	void RemoveNode(UControlRigBlueprint* Rig, const FString& Node);

	/** make Bone ("head") look at Target: "@Variable" (a world-space Vector variable, e.g. a target location from the Anim Blueprint), a rig element (control / bone / null), or a rig-space location "(x, y, z)"; Weight a number or "@Variable" (0..1, blends the look); Axis the bone's local axis that faces the target ("" = the one facing the character's forward, +Y of the mesh); keeps the bone upright; runs at the end of Forwards Solve (or after After); returns the Aim node's name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddLookAt(UControlRigBlueprint* Rig, const FString& Bone, const FString& Target, const FString& Weight = TEXT("1"), const FString& Axis = TEXT(""), const FString& After = TEXT(""));

	/** run Rig in the Anim Blueprint: a Control Rig node in its AnimGraph right before Output Pose (after every pose node, e.g. after a foot IK rig), or after the node After (node name, title or rig name); compiles the rig first; bBindVariables binds each public rig variable to an Anim Blueprint variable of the same name (added when missing); returns what it did */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString AddToAnimBlueprint(UAnimBlueprint* AnimBlueprint, UControlRigBlueprint* Rig, const FString& After = TEXT(""), bool bBindVariables = true);

	/** set the input Input (a public rig variable or control) of the Control Rig node Node (node name or rig name; "" = the only one) of an Anim Blueprint: Value "@Variable" binds an Anim Blueprint variable (added when missing), else a constant ("0.5", "(0, 0, 100)"); returns the input's new source */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString SetRigInput(UAnimBlueprint* AnimBlueprint, const FString& Node, const FString& Input, const FString& Value);

	/** every frame (Event Blueprint Update Animation) set the Vector variable LocationVariable to the world location of the nearest actor of ActorClass ("BP_Orb") within Radius cm of the owner, smoothed at Speed (0 = snap), and blend the float variable WeightVariable ("" = none) to 1 in range and to 0 out of range; variables are added when missing; for look-at / aim at the nearest thing (the owner itself is never the target; without a weight variable a first target is approached from the variable's last value); returns what it added */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Control Rig", meta = (BlueprintInternalUseOnly = "true"))
	FString TrackNearestActor(UAnimBlueprint* AnimBlueprint, const FString& ActorClass, const FString& LocationVariable = TEXT("LookAtTarget"),
		const FString& WeightVariable = TEXT("LookAtWeight"), float Radius = 1000.0f, float Speed = 5.0f);
};
