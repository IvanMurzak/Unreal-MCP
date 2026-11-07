// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Rigs/RigHierarchyDefines.h"

class UControlRigBlueprint;
class URigVMController;
class URigVMNode;
class URigVMPin;
class UScriptStruct;

/**
 * The Control Rig graph (the RigVM model of a Control Rig blueprint's Rig graph) as the `controlRig` operations author it:
 * nodes by name or title, pins by path ("Aim.Primary.Target"), values from text with links ("@LookAtTarget" a rig
 * variable, "@FromWorld.Global" another node's output), the Forwards Solve execution chain, and the read-backs (Nodes,
 * Variables). Every function that fails calls UnrealMcpRql::Fail and returns null / false.
 */
namespace UnrealAIControlRigGraph
{
	/** The first element named @p Name among @p Types, in that order of preference; an invalid key after Fail. */
	FRigElementKey Element(UControlRigBlueprint* Rig, const FString& Name, std::initializer_list<ERigElementType> Types, const TCHAR* What);

	/** The pin default text of an element key, as the Control Rig editor writes it: (Type=Bone,Name="Hand"). */
	FString KeyText(const FRigElementKey& Key);

	/** The rig's graph controller (null after Fail when the rig has no graph). */
	URigVMController* Controller(UControlRigBlueprint* Rig);

	/** The node named @p Name, or titled so ("Forwards Solve", "Aim" when one node has the title); null after Fail listing the nodes. */
	URigVMNode* FindNode(UControlRigBlueprint* Rig, const FString& Name);

	/** The pin at @p Path ("Aim.Weight", "Aim.Primary.Target"; names or display names); null after Fail listing the node's pins. */
	URigVMPin* FindPin(UControlRigBlueprint* Rig, const FString& Path);

	/** The Forwards Solve event node of the rig's graph, added when it has none. */
	URigVMNode* ForwardsSolve(URigVMController* Controller);

	/** The execute output pin at the end of the straight chain that starts at @p Start (null when it branches). */
	URigVMPin* EndOfChain(URigVMNode* Start);

	/** The node's only execute output pin (null for none, or for a Sequence / branch with several). */
	URigVMPin* ExecuteOutput(URigVMNode* Node);

	/** The node's execute input pin (null for a pure node or an event). */
	URigVMPin* ExecuteInput(URigVMNode* Node);

	/**
	 * Set @p Pin from text, recording each node it adds in @p OutAdded: "@Variable" links a getter of that rig variable,
	 * "@Node.Pin" links another node's output; an element key pin takes an element name ("head", "Bone:head"); a struct of
	 * numbers takes "(0, 1, 0)"; anything else is the pin's own text form ("(X=0,Y=1,Z=0)", "Direction", "True"). Returns
	 * the value as the pin now holds it in @p OutValue ("@LookAtTarget" for a link); false after Fail.
	 */
	bool SetPin(UControlRigBlueprint* Rig, URigVMPin* Pin, const FString& Value, TArray<URigVMNode*>& OutAdded, FString& OutValue);

	/** Split "A=1, B.C=(X=0,Y=1,Z=0), D=@Var" at its top-level commas into (pin, value) pairs; false after Fail. */
	bool ParseSettings(const FString& Settings, TArray<TPair<FString, FString>>& OutPairs);

	/** Remove @p Nodes from the graph (the nodes a failing operation added). */
	void RemoveNodes(URigVMController* Controller, const TArray<URigVMNode*>& Nodes);

	/**
	 * Add a node of the rig unit @p Unit, set @p Settings (ParseSettings form) on it and, when it has an execute pin, link it
	 * into the execution chain after @p After ("" = the end of Forwards Solve; a node name or "Node.Pin" of an execute
	 * output, such as a Sequence's "Sequence.B"). Returns the node; null after Fail with nothing added.
	 */
	URigVMNode* AddUnit(UControlRigBlueprint* Rig, UScriptStruct* Unit, const FString& Settings, const FString& After, const FString& Name);

	/** The CPP type path AddMemberVariable takes for an agent's type name ("Vector", "float", "bool", "Transform", ...); empty after Fail. */
	FString VariableType(const FString& Type);

	/** @p Value in the text form of @p CPPType / @p TypeObject ("(0, 0, 100)" -> "(X=0,Y=0,Z=100)" for a vector). */
	FString ValueText(const FString& Value, const FString& CPPType, UObject* TypeObject);

	/** The Nodes member: every node of the rig graph with its unit, execution order and pin values / links. */
	TSharedPtr<FJsonValue> NodesJson(UObject* Object);

	/** The Variables member: the rig's variables (Name, Type, Public, Default). */
	TSharedPtr<FJsonValue> VariablesJson(UObject* Object);
}
