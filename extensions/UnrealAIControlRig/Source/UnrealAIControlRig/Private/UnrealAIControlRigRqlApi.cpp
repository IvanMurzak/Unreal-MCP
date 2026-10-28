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
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Hierarchy/RigUnit_GetTransform.h"
#include "Units/Highlevel/Hierarchy/RigUnit_TwoBoneIKSimple.h"

namespace UnrealAIControlRigRqlApiLocal
{
	/** The element of the rig named @p Name, of @p Type (ERigElementType::All: any type); an invalid key after Fail. */
	FRigElementKey Element(UControlRigBlueprint* Rig, const FString& Name, ERigElementType Type, const TCHAR* What)
	{
		URigHierarchy* Hierarchy = Rig->GetHierarchy();
		const FName Wanted(*Name.TrimStartAndEnd());
		for (const FRigElementKey& Key : Hierarchy ? Hierarchy->GetAllKeys() : TArray<FRigElementKey>())
		{
			if (Key.Name == Wanted && (Type == ERigElementType::All || Key.Type == Type))
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

	bool ValidName(const FString& Name)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		if (!Trimmed.IsEmpty() && !Trimmed.Contains(TEXT(" ")) && !Trimmed.Contains(TEXT(".")))
			return true;
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("'%s' is not an element name (one word, no dots: UpperArm)"), *Name));
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

	/** The last node of the execution chain that starts at @p Start (its ExecuteContext output links to nothing). */
	URigVMNode* EndOfChain(URigVMNode* Start)
	{
		URigVMNode* Node = Start;
		for (int32 Guard = 0; Node && Guard < 1000; ++Guard)
		{
			const URigVMPin* Exec = Node->FindPin(TEXT("ExecuteContext"));
			const TArray<URigVMPin*> Targets = Exec ? Exec->GetLinkedTargetPins() : TArray<URigVMPin*>();
			if (Targets.Num() == 0)
				return Node;
			Node = Targets[0]->GetNode();
		}
		return Node;
	}
}

using namespace UnrealAIControlRigRqlApiLocal;

UControlRigBlueprint* UUnrealAIControlRigRqlApi::CreateRig(const FString& Path)
{
	const FString Package = Path.TrimStartAndEnd();
	if (!FPackageName::IsValidLongPackageName(Package) || !Package.StartsWith(TEXT("/")))
		return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("'%s' is not an asset path such as /Game/Rigs/CR_Arm"), *Path));
	if (FPackageName::DoesPackageExist(Package) || FindObject<UObject>(nullptr, *(Package + TEXT(".") + FPackageName::GetShortName(Package))))
		return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("'%s' already exists"), *Package));
	UControlRigBlueprint* Rig = UControlRigBlueprintFactory::CreateNewControlRigAsset(Package);
	if (!Rig)
		return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("could not create a Control Rig at %s"), *Package));
	return Rig;
}

FString UUnrealAIControlRigRqlApi::AddBone(UControlRigBlueprint* Rig, const FString& Name, const FString& Parent, FVector Location)
{
	if (!ValidName(Name))
		return FString();
	FRigElementKey ParentKey;
	if (!Parent.TrimStartAndEnd().IsEmpty())
	{
		ParentKey = Element(Rig, Parent, ERigElementType::Bone, TEXT("bone"));
		if (!ParentKey.IsValid())
			return FString();
	}
	URigHierarchyController* Controller = Rig->GetHierarchyController();
	if (!Controller)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no hierarchy to edit"), *Rig->GetName()));
	static_cast<UObject*>(Rig)->Modify(); // UBlueprint::Modify, not IControlRigAssetInterface::Modify
	const FRigElementKey Key = Controller->AddBone(FName(*Name.TrimStartAndEnd()), ParentKey, FTransform(Location),
		/*bTransformInGlobal*/ false, ERigBoneType::User, /*bSetupUndo*/ true);
	return Key.IsValid() ? Key.Name.ToString() : UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not add the bone %s"), *Name));
}

FString UUnrealAIControlRigRqlApi::AddControl(UControlRigBlueprint* Rig, const FString& Name, FVector Location, const FString& Parent)
{
	if (!ValidName(Name))
		return FString();
	FRigElementKey ParentKey;
	if (!Parent.TrimStartAndEnd().IsEmpty())
	{
		ParentKey = Element(Rig, Parent, ERigElementType::All, TEXT("element"));
		if (!ParentKey.IsValid())
			return FString();
	}
	URigHierarchyController* Controller = Rig->GetHierarchyController();
	if (!Controller)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no hierarchy to edit"), *Rig->GetName()));
	FRigControlSettings Settings;
	Settings.ControlType = ERigControlType::EulerTransform;
	Settings.ShapeName = TEXT("Box_Thick");
	const FRigControlValue Value = URigHierarchy::MakeControlValueFromEulerTransform(FEulerTransform(Location, FRotator::ZeroRotator, FVector::OneVector));
	static_cast<UObject*>(Rig)->Modify(); // UBlueprint::Modify, not IControlRigAssetInterface::Modify
	const FRigElementKey Key = Controller->AddControl(FName(*Name.TrimStartAndEnd()), ParentKey, Settings, Value, FTransform::Identity, FTransform::Identity,
		/*bSetupUndo*/ true);
	return Key.IsValid() ? Key.Name.ToString() : UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not add the control %s"), *Name));
}

FString UUnrealAIControlRigRqlApi::AddTwoBoneIK(UControlRigBlueprint* Rig, const FString& BoneA, const FString& BoneB, const FString& Effector, const FString& Target)
{
	// Every element is checked before the graph changes.
	const FRigElementKey A = Element(Rig, BoneA, ERigElementType::Bone, TEXT("bone"));
	const FRigElementKey B = A.IsValid() ? Element(Rig, BoneB, ERigElementType::Bone, TEXT("bone")) : FRigElementKey();
	const FRigElementKey End = B.IsValid() ? Element(Rig, Effector, ERigElementType::Bone, TEXT("bone")) : FRigElementKey();
	const FRigElementKey Goal = End.IsValid() ? Element(Rig, Target, ERigElementType::All, TEXT("control or element")) : FRigElementKey();
	if (!Goal.IsValid())
		return FString();
	URigVMController* Controller = Rig->GetController();
	if (!Controller || !Controller->GetGraph())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no rig graph to edit"), *Rig->GetName()));
	static_cast<UObject*>(Rig)->Modify(); // UBlueprint::Modify, not IControlRigAssetInterface::Modify
	URigVMNode* Solve = ForwardsSolve(Controller);
	const URigVMNode* Previous = EndOfChain(Solve);
	const FVector2D At = Previous ? Previous->GetPosition() + FVector2D(400.0, 0.0) : FVector2D::ZeroVector;
	URigVMUnitNode* IK = Controller->AddUnitNode(FRigUnit_TwoBoneIKSimplePerItem::StaticStruct(), TEXT("Execute"), At, TEXT("TwoBoneIK"));
	URigVMUnitNode* Get = Controller->AddUnitNode(FRigUnit_GetTransform::StaticStruct(), TEXT("Execute"), At + FVector2D(-250.0, 250.0), TEXT("GetIKTarget"));
	if (!Solve || !IK || !Get)
		return UnrealMcpRql::Failed<FString>(TEXT("could not add the Basic IK nodes to the rig graph"));
	const FString IKName = IK->GetName();
	const bool bWired = Controller->SetPinDefaultValue(IKName + TEXT(".ItemA"), KeyText(A))
		&& Controller->SetPinDefaultValue(IKName + TEXT(".ItemB"), KeyText(B))
		&& Controller->SetPinDefaultValue(IKName + TEXT(".EffectorItem"), KeyText(End))
		&& Controller->SetPinDefaultValue(Get->GetName() + TEXT(".Item"), KeyText(Goal))
		&& Controller->AddLink(Get->GetName() + TEXT(".Transform"), IKName + TEXT(".Effector"))
		&& Controller->AddLink(Previous->GetName() + TEXT(".ExecuteContext"), IKName + TEXT(".ExecuteContext"));
	if (!bWired)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("added %s but could not wire it (check the bones form a chain)"), *IKName));
	Rig->RecompileVM();
	return IKName;
}
