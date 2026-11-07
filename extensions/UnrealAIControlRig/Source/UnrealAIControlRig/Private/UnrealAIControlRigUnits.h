// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIControlRigUnits.generated.h"

class UScriptStruct;

/**
 * One row of the `controlRigUnits` source: a rig unit (a node of the Control Rig graph, e.g. Aim, Basic IK, Sphere Trace,
 * Spring Interpolate) that the engine and the enabled plugins register and a Control Rig can run.
 */
UCLASS(Transient)
class UUnrealAIControlRigUnit : public UObject
{
	GENERATED_BODY()

public:
	/** the name to pass to AddUnit, e.g. AimItem (the struct name without its RigUnit_ / RigVMFunction_ prefix) */
	UPROPERTY(VisibleAnywhere, Category = "Unit")
	FString Name;

	/** the unit's struct, e.g. RigUnit_AimItem (AddUnit takes it too) */
	UPROPERTY(VisibleAnywhere, Category = "Unit")
	FString Struct;

	/** the node title in the Control Rig editor, e.g. Aim */
	UPROPERTY(VisibleAnywhere, Category = "Unit")
	FString DisplayName;

	/** the editor's node category, e.g. Hierarchy, Math|Vector, Simulation|Springs */
	UPROPERTY(VisibleAnywhere, Category = "Unit")
	FString Category;

	/** search keywords, e.g. Lookat */
	UPROPERTY(VisibleAnywhere, Category = "Unit")
	FString Keywords;

	/** what it does (the unit's tooltip) */
	UPROPERTY(VisibleAnywhere, Category = "Unit")
	FString Description;

	/** true when it runs in the execution chain (AddUnit appends it to Forwards Solve); false for a pure value node */
	UPROPERTY(VisibleAnywhere, Category = "Unit")
	bool Execute = false;

	/** its pins: "Name: Type (in|out)"; a struct pin lists the members its settings reach as Name.Member */
	UPROPERTY(VisibleAnywhere, Category = "Unit")
	TArray<FString> Pins;
};

namespace UnrealAIControlRigUnits
{
	/** The unit rows, sorted by category and display name (deprecated units left out). */
	TArray<UObject*> List();

	/**
	 * The unit named @p Name: its struct ("RigUnit_AimItem", "FRigUnit_AimItem"), the struct without its prefix ("AimItem")
	 * or its display name ("Aim"); case, spaces and underscores ignored. Null after UnrealMcpRql::Fail (unknown or
	 * ambiguous, naming the candidates or the engine plugin that has it).
	 */
	UScriptStruct* Resolve(const FString& Name);
}
