// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIControlRigRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "Misc/PackageName.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyDefines.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Hierarchy/RigUnit_GetTransform.h"
#include "Units/Highlevel/Hierarchy/RigUnit_TwoBoneIKSimple.h"

namespace UnrealAIControlRigRqlApiLocal
{
	/** The first element named @p Name among @p Types, in that order of preference; an invalid key after Fail. */
	FRigElementKey Element(UControlRigBlueprint* Rig, const FString& Name, std::initializer_list<ERigElementType> Types, const TCHAR* What)
	{
		const URigHierarchy* Hierarchy = Rig->GetHierarchy();
		const FName Wanted(*Name.TrimStartAndEnd());
		for (const ERigElementType Type : Types)
		{
			const FRigElementKey Key(Wanted, Type);
			if (Hierarchy && Hierarchy->Contains(Key))
				return Key;
		}
		UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no %s named '%s'"), *Rig->GetName(), What, *Name));
		return FRigElementKey();
	}

	/** The pin default text of an element key, as the Control Rig editor writes it: (Type=Bone,Name="Hand"). */
	FString KeyText(const FRigElementKey& Key)
	{
		FString Text;
		FRigElementKey::StaticStruct()->ExportText(Text, &Key, nullptr, nullptr, PPF_None, nullptr);
		return Text;
	}

	/**
	 * Checks a new element's name (one word, free for @p Type) and resolves its optional parent; returns the rig's
	 * hierarchy controller with the rig marked for undo, or null after Fail.
	 */
	URigHierarchyController* BeginAdd(UControlRigBlueprint* Rig, const FString& Name, ERigElementType Type, const FString& Parent,
		std::initializer_list<ERigElementType> ParentTypes, FRigElementKey& OutParent)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.Contains(TEXT(" ")) || Trimmed.Contains(TEXT(".")))
			return UnrealMcpRql::Failed<URigHierarchyController*>(FString::Printf(TEXT("'%s' is not an element name (one word, no dots: UpperArm)"), *Name));
		const URigHierarchy* Hierarchy = Rig->GetHierarchy();
		if (Hierarchy && Hierarchy->Contains(FRigElementKey(FName(*Trimmed), Type)))
			return UnrealMcpRql::Failed<URigHierarchyController*>(FString::Printf(TEXT("%s already has an element named '%s'"), *Rig->GetName(), *Trimmed));
		if (!Parent.TrimStartAndEnd().IsEmpty())
		{
			OutParent = Element(Rig, Parent, ParentTypes, TEXT("parent"));
			if (!OutParent.IsValid())
				return nullptr;
		}
		URigHierarchyController* Controller = Rig->GetHierarchyController();
		if (!Controller)
			return UnrealMcpRql::Failed<URigHierarchyController*>(FString::Printf(TEXT("%s has no hierarchy to edit"), *Rig->GetName()));
		static_cast<UObject*>(Rig)->Modify(); // UBlueprint::Modify, not IControlRigAssetInterface::Modify
		return Controller;
	}

	/** The Forwards Solve event node of the rig's graph, added when it has none. */
	URigVMNode* ForwardsSolve(URigVMController* Controller)
	{
		for (URigVMNode* Node : Controller->GetGraph()->GetNodes())
		{
			const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node);
			if (Unit && Unit->GetScriptStruct() == FRigUnit_BeginExecution::StaticStruct())
				return Node;
		}
		return Controller->AddUnitNode(FRigUnit_BeginExecution::StaticStruct(), TEXT("Execute"), FVector2D(-400.0, 0.0), TEXT("BeginExecution"));
	}

	/** The node's only execute output pin (null for none, or for a Sequence / branch with several). */
	URigVMPin* ExecuteOutput(URigVMNode* Node)
	{
		URigVMPin* Found = nullptr;
		for (URigVMPin* Pin : Node->GetPins())
		{
			if (Pin->IsExecuteContext() && (Pin->GetDirection() == ERigVMPinDirection::Output || Pin->GetDirection() == ERigVMPinDirection::IO))
			{
				if (Found)
					return nullptr;
				Found = Pin;
			}
		}
		return Found;
	}

	/** The execute output pin at the end of the straight chain that starts at @p Start: where a new solver node goes. */
	URigVMPin* EndOfChain(URigVMNode* Start)
	{
		URigVMPin* Exec = ExecuteOutput(Start);
		for (int32 Guard = 0; Exec && Guard < 1000; ++Guard)
		{
			const TArray<URigVMPin*> Targets = Exec->GetLinkedTargetPins();
			if (Targets.Num() == 0)
				return Exec;
			Exec = ExecuteOutput(Targets[0]->GetNode());
		}
		return nullptr;
	}
}

using namespace UnrealAIControlRigRqlApiLocal;

UControlRigBlueprint* UUnrealAIControlRigRqlApi::CreateRig(const FString& Path)
{
	const FString Package = Path.TrimStartAndEnd();
	if (!FPackageName::IsValidLongPackageName(Package))
		return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("'%s' is not an asset path such as /Game/Rigs/CR_Arm"), *Path));
	if (FPackageName::DoesPackageExist(Package) || FindObject<UObject>(nullptr, *(Package + TEXT(".") + FPackageName::GetShortName(Package))))
		return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("'%s' already exists"), *Package));
	UControlRigBlueprint* Rig = UControlRigBlueprintFactory::CreateNewControlRigAsset(Package);
	return Rig ? Rig : UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("could not create a Control Rig at %s"), *Package));
}

FString UUnrealAIControlRigRqlApi::AddBone(UControlRigBlueprint* Rig, const FString& Name, const FString& Parent, FVector Location)
{
	FRigElementKey ParentKey;
	URigHierarchyController* Controller = BeginAdd(Rig, Name, ERigElementType::Bone, Parent, { ERigElementType::Bone }, ParentKey);
	if (!Controller)
		return FString();
	const FRigElementKey Key = Controller->AddBone(FName(*Name.TrimStartAndEnd()), ParentKey, FTransform(Location),
		/*bTransformInGlobal*/ false, ERigBoneType::User, /*bSetupUndo*/ true);
	return Key.IsValid() ? Key.Name.ToString() : UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not add the bone %s"), *Name));
}

FString UUnrealAIControlRigRqlApi::AddControl(UControlRigBlueprint* Rig, const FString& Name, FVector Location, const FString& Parent)
{
	FRigElementKey ParentKey;
	URigHierarchyController* Controller = BeginAdd(Rig, Name, ERigElementType::Control, Parent,
		{ ERigElementType::Control, ERigElementType::Null, ERigElementType::Bone }, ParentKey);
	if (!Controller)
		return FString();
	FRigControlSettings Settings;
	Settings.ControlType = ERigControlType::EulerTransform;
	Settings.ShapeName = TEXT("Box_Thick");
	const FRigControlValue Value = URigHierarchy::MakeControlValueFromEulerTransform(FEulerTransform(Location, FRotator::ZeroRotator, FVector::OneVector));
	const FRigElementKey Key = Controller->AddControl(FName(*Name.TrimStartAndEnd()), ParentKey, Settings, Value, FTransform::Identity, FTransform::Identity,
		/*bSetupUndo*/ true);
	return Key.IsValid() ? Key.Name.ToString() : UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not add the control %s"), *Name));
}

FString UUnrealAIControlRigRqlApi::AddTwoBoneIK(UControlRigBlueprint* Rig, const FString& BoneA, const FString& BoneB, const FString& Effector, const FString& Target)
{
	// Every element, the chain and the graph are checked before the graph changes.
	const FRigElementKey A = Element(Rig, BoneA, { ERigElementType::Bone }, TEXT("bone"));
	const FRigElementKey B = A.IsValid() ? Element(Rig, BoneB, { ERigElementType::Bone }, TEXT("bone")) : FRigElementKey();
	const FRigElementKey End = B.IsValid() ? Element(Rig, Effector, { ERigElementType::Bone }, TEXT("bone")) : FRigElementKey();
	const FRigElementKey Goal = End.IsValid()
		? Element(Rig, Target, { ERigElementType::Control, ERigElementType::Null, ERigElementType::Bone }, TEXT("control"))
		: FRigElementKey();
	if (!Goal.IsValid())
		return FString();
	const URigHierarchy* Hierarchy = Rig->GetHierarchy();
	if (Hierarchy->GetFirstParent(B) != A || Hierarchy->GetFirstParent(End) != B)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s, %s, %s is not a bone chain: each bone must be the parent of the next"),
			*BoneA, *BoneB, *Effector));
	URigVMController* Controller = Rig->GetController();
	if (!Controller || !Controller->GetGraph())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no rig graph to edit"), *Rig->GetName()));

	static_cast<UObject*>(Rig)->Modify(); // UBlueprint::Modify, not IControlRigAssetInterface::Modify
	URigVMNode* Solve = ForwardsSolve(Controller);
	URigVMPin* Exec = Solve ? EndOfChain(Solve) : nullptr;
	if (!Exec)
		return UnrealMcpRql::Failed<FString>(TEXT("Forwards Solve branches (a Sequence or branch node): add the IK in the Control Rig editor"));
	const FVector2D At = Exec->GetNode()->GetPosition() + FVector2D(400.0, 0.0);
	URigVMUnitNode* IK = Controller->AddUnitNode(FRigUnit_TwoBoneIKSimplePerItem::StaticStruct(), TEXT("Execute"), At, TEXT("TwoBoneIK"));
	URigVMUnitNode* Get = Controller->AddUnitNode(FRigUnit_GetTransform::StaticStruct(), TEXT("Execute"), At + FVector2D(-250.0, 250.0), TEXT("GetIKTarget"));
	const FString IKName = IK ? IK->GetName() : FString();
	const bool bWired = IK && Get
		&& Controller->SetPinDefaultValue(IKName + TEXT(".ItemA"), KeyText(A))
		&& Controller->SetPinDefaultValue(IKName + TEXT(".ItemB"), KeyText(B))
		&& Controller->SetPinDefaultValue(IKName + TEXT(".EffectorItem"), KeyText(End))
		&& Controller->SetPinDefaultValue(Get->GetName() + TEXT(".Item"), KeyText(Goal))
		&& Controller->AddLink(Get->GetName() + TEXT(".Transform"), IKName + TEXT(".Effector"))
		&& Controller->AddLink(Exec->GetPinPath(), IKName + TEXT(".ExecuteContext"));
	if (!bWired)
	{
		for (URigVMNode* Added : { static_cast<URigVMNode*>(IK), static_cast<URigVMNode*>(Get) })
		{
			if (Added)
				Controller->RemoveNode(Added);
		}
		return UnrealMcpRql::Failed<FString>(TEXT("could not add and wire the Basic IK nodes in the rig graph"));
	}
	Rig->RecompileVM();
	return IKName;
}
