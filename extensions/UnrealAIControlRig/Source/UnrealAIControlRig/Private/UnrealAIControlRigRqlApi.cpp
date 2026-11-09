// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIControlRigRqlApi.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIControlRigAnim.h"
#include "UnrealAIControlRigGraph.h"
#include "UnrealAIControlRigUnits.h"
#include "UnrealMcpRqlPlay.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "Components/SceneComponent.h"
#include "ControlRig.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyDefines.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "UObject/UObjectIterator.h"
#include "Units/Hierarchy/RigUnit_GetTransform.h"
#include "Units/Hierarchy/RigUnit_WorldSpace.h"
#include "Units/Highlevel/Hierarchy/RigUnit_AimBone.h"
#include "Units/Highlevel/Hierarchy/RigUnit_TwoBoneIKSimple.h"

using namespace UnrealAIControlRigGraph;

namespace UnrealAIControlRigRqlApiLocal
{
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

	/** The principal axis closest to @p Direction ("(X=0,Y=1,Z=0)"), or the direction itself when none is close. */
	FString AxisText(const FVector& Direction)
	{
		FVector Axis = Direction.GetSafeNormal();
		const FVector Abs = Axis.GetAbs();
		const double Max = Abs.GetMax();
		if (Max > 0.8)
			Axis = FVector(Abs.X == Max ? FMath::Sign(Axis.X) : 0.0, Abs.Y == Max ? FMath::Sign(Axis.Y) : 0.0, Abs.Z == Max ? FMath::Sign(Axis.Z) : 0.0);
		return FString::Printf(TEXT("(X=%g,Y=%g,Z=%g)"), Axis.X, Axis.Y, Axis.Z);
	}

	/** An axis argument as pin text: "X", "-Y", "+Z", "(0, 1, 0)", "0, 1, 0" or "(X=0,Y=1,Z=0)"; empty when it is none. */
	FString AxisArgument(const FString& Axis)
	{
		FString Text = Axis.TrimStartAndEnd().ToUpper();
		const double Sign = Text.StartsWith(TEXT("-")) ? -1.0 : 1.0;
		const FString Letter = Text.Replace(TEXT("-"), TEXT("")).Replace(TEXT("+"), TEXT(""));
		if (Letter == TEXT("X") || Letter == TEXT("Y") || Letter == TEXT("Z"))
			return AxisText(FVector(Letter == TEXT("X") ? Sign : 0.0, Letter == TEXT("Y") ? Sign : 0.0, Letter == TEXT("Z") ? Sign : 0.0));
		Text = Axis.TrimStartAndEnd();
		if (Text.Contains(TEXT("=")))
			return Text;
		Text.RemoveFromStart(TEXT("("));
		Text.RemoveFromEnd(TEXT(")"));
		TArray<FString> Parts;
		FVector Vector;
		if (Text.ParseIntoArray(Parts, TEXT(",")) != 3 || !LexTryParseString(Vector.X, *Parts[0].TrimStartAndEnd())
			|| !LexTryParseString(Vector.Y, *Parts[1].TrimStartAndEnd()) || !LexTryParseString(Vector.Z, *Parts[2].TrimStartAndEnd()) || Vector.IsNearlyZero())
			return FString();
		return FString::Printf(TEXT("(X=%g,Y=%g,Z=%g)"), Vector.X, Vector.Y, Vector.Z);
	}

	/** @p Base, or @p Base_1, _2 ... when the rig graph has a node of that name. */
	FString FreeNodeName(UControlRigBlueprint* Rig, const FString& Base)
	{
		const URigVMGraph* Graph = Rig->GetModel();
		FString Name = Base;
		for (int32 Index = 1; Graph && Graph->FindNodeByName(*Name); ++Index)
			Name = FString::Printf(TEXT("%s_%d"), *Base, Index);
		return Name;
	}

	/** The rig's asset name from its generated class: CR_Foot_C -> CR_Foot. */
	FString RunningRigName(const UControlRig* Rig)
	{
		FString Name = Rig->GetClass()->GetName();
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	/** Whether @p Rig is the running rig @p Wanted names: an asset name, or an asset path ("/Game/Rigs/CR_Foot", also with ".CR_Foot"). */
	bool IsRigNamed(const UControlRig* Rig, const FString& Wanted)
	{
		return Wanted.StartsWith(TEXT("/"))
			? FPackageName::ObjectPathToPackageName(Wanted).Equals(Rig->GetClass()->GetOutermost()->GetName(), ESearchCase::IgnoreCase)
			: RunningRigName(Rig).Equals(Wanted, ESearchCase::IgnoreCase);
	}

	/** Whether the rig's graphs (its functions too) run a world trace unit. */
	bool RunsWorldTrace(const UControlRig* Rig)
	{
		UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Rig->GetClass()->ClassGeneratedBy);
		if (!Blueprint)
			return false;
		for (const URigVMGraph* Graph : Blueprint->GetAllModels())
		{
			if (!Graph)
				continue;
			for (const URigVMNode* Node : Graph->GetNodes())
			{
				const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node);
				const UScriptStruct* Struct = Unit ? Unit->GetScriptStruct() : nullptr;
				if (Struct && Struct->GetName().Contains(TEXT("Trace")))
					return true;
			}
		}
		return false;
	}

	/** Recompile the rig's VM after a graph edit (the editor does it on its own when the rig is open). */
	void Recompile(UControlRigBlueprint* Rig)
	{
		static_cast<UObject*>(Rig)->MarkPackageDirty();
		Rig->RecompileVM();
	}
}

using namespace UnrealAIControlRigRqlApiLocal;

UControlRigBlueprint* UUnrealAIControlRigRqlApi::CreateRig(const FString& Path, const FString& Mesh)
{
	const FString Package = Path.TrimStartAndEnd();
	if (!FPackageName::IsValidLongPackageName(Package))
		return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("'%s' is not an asset path such as /Game/Rigs/CR_Arm"), *Path));
	if (FPackageName::DoesPackageExist(Package) || FindObject<UObject>(nullptr, *(Package + TEXT(".") + FPackageName::GetShortName(Package))))
		return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("'%s' already exists (copy an existing rig with editor | call CopyAsset(from, to))"), *Package));
	USkeletalMesh* SkeletalMesh = nullptr;
	USkeleton* Skeleton = nullptr;
	if (!Mesh.TrimStartAndEnd().IsEmpty())
	{
		SkeletalMesh = Cast<USkeletalMesh>(UnrealMcpRql::LoadObject(USkeletalMesh::StaticClass(), Mesh.TrimStartAndEnd()));
		Skeleton = SkeletalMesh ? SkeletalMesh->GetSkeleton() : Cast<USkeleton>(UnrealMcpRql::LoadObject(USkeleton::StaticClass(), Mesh.TrimStartAndEnd()));
		if (SkeletalMesh && !Skeleton)
			return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("the mesh %s has no skeleton: assign one in the mesh editor first"), *SkeletalMesh->GetName()));
		if (!Skeleton)
			return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("'%s' is not a SkeletalMesh or Skeleton asset (a character's mesh such as /Game/Characters/Mannequins/Meshes/SKM_Manny_Simple)"), *Mesh));
	}
	// Not UControlRigBlueprintFactory::CreateNewControlRigAsset: it makes the name unique first, and that step reads the
	// name's trailing digits as an int32, so CR_F9672969617062 lands at CR_F0002147483647 and the path asked for stays
	// empty. The check above already refuses a taken path, so the rig can take exactly the name asked for.
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(
		FPackageName::GetShortName(Package), FPackageName::GetLongPackagePath(Package), nullptr, Factory));
	if (!Rig)
		return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("could not create a Control Rig at %s"), *Package));
	if (Skeleton)
	{
		// As the Content Browser's "Create Control Rig" on a mesh does: its bones and curves, the preview mesh.
		URigHierarchyController* Controller = Rig->GetHierarchyController();
		if (!Controller)
			return UnrealMcpRql::Failed<UControlRigBlueprint*>(FString::Printf(TEXT("%s was created but has no hierarchy to import %s's bones into"), *Rig->GetName(), *Skeleton->GetName()));
		{
			if (SkeletalMesh)
			{
				Controller->ImportBones(SkeletalMesh->GetRefSkeleton(), NAME_None, false, false, false, false);
				Controller->ImportCurvesFromSkeletalMesh(SkeletalMesh, NAME_None, false, false);
			}
			else
			{
				Controller->ImportBones(Skeleton->GetReferenceSkeleton(), NAME_None, false, false, false, false);
				Controller->ImportCurves(Skeleton, NAME_None, false, false);
			}
		}
		Rig->GetSourceHierarchyImport() = Skeleton;
		Rig->GetSourceCurveImport() = Skeleton;
		if (SkeletalMesh)
			Rig->SetPreviewMesh(SkeletalMesh);
		Rig->RecompileVM();
	}
	return Rig;
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
	URigVMController* Controller = UnrealAIControlRigGraph::Controller(Rig);
	if (!Controller)
		return FString();

	static_cast<UObject*>(Rig)->Modify(); // UBlueprint::Modify, not IControlRigAssetInterface::Modify
	URigVMNode* Solve = ForwardsSolve(Controller);
	URigVMPin* Exec = Solve ? EndOfChain(Solve) : nullptr;
	if (!Exec)
		return UnrealMcpRql::Failed<FString>(TEXT("Forwards Solve branches (a Sequence or branch node): add the IK with AddUnit(rig, \"TwoBoneIKSimplePerItem\", settings, after)"));
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
		RemoveNodes(Controller, { IK, Get });
		return UnrealMcpRql::Failed<FString>(TEXT("could not add and wire the Basic IK nodes in the rig graph"));
	}
	Recompile(Rig);
	return IKName;
}

FString UUnrealAIControlRigRqlApi::AddVariable(UControlRigBlueprint* Rig, const FString& Name, const FString& Type, const FString& Default, bool bPublic)
{
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || !FName(*Trimmed).IsValidXName(INVALID_OBJECTNAME_CHARACTERS))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a variable name (one word: LookAtTarget)"), *Name));
	FRigVMGraphVariableDescription Existing;
	if (FindVariable(Rig, Trimmed, Existing))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s already has a variable %s"), *Rig->GetName(), *Existing.Name.ToString()));
	UObject* TypeObject = nullptr;
	const FString CPPType = VariableType(Type, TypeObject);
	if (CPPType.IsEmpty())
		return FString();
	const bool bArray = CPPType.StartsWith(TEXT("TArray<"));
	const FString DefaultText = Default.TrimStartAndEnd().IsEmpty() ? FString() : (bArray ? Default.TrimStartAndEnd() : ValueText(Default, CPPType, TypeObject));
	// A scalar default is checked here; struct defaults are the struct's text form ("(0, 50, 100)" for vectors).
	double Real = 0.0;
	int64 Whole = 0;
	const bool bValidDefault = DefaultText.IsEmpty() || bArray
		|| ((CPPType == TEXT("float") || CPPType == TEXT("double")) ? LexTryParseString(Real, *DefaultText)
			: CPPType == TEXT("int32") ? LexTryParseString(Whole, *DefaultText)
			: CPPType == TEXT("bool") ? (DefaultText == TEXT("True") || DefaultText == TEXT("False"))
			: true);
	if (!bValidDefault)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a %s default"), *Default, *Type));
	static_cast<UObject*>(Rig)->Modify();
	const FName Added = Rig->AddMemberVariable(FName(*Trimmed), CPPType, bPublic, /*bIsReadOnly*/ false, DefaultText);
	if (Added.IsNone())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not add the variable %s (%s) to %s"), *Trimmed, *Type, *Rig->GetName()));
	return Added.ToString();
}

FString UUnrealAIControlRigRqlApi::AddUnit(UControlRigBlueprint* Rig, const FString& Unit, const FString& Settings, const FString& After, const FString& Name)
{
	UScriptStruct* Struct = UnrealAIControlRigUnits::Resolve(Unit);
	if (!Struct)
		return FString();
	static_cast<UObject*>(Rig)->Modify();
	URigVMNode* Node = UnrealAIControlRigGraph::AddUnit(Rig, Struct, Settings, After, Name);
	if (!Node)
		return FString();
	Recompile(Rig);
	return Node->GetName();
}

FString UUnrealAIControlRigRqlApi::AddSetVariable(UControlRigBlueprint* Rig, const FString& Variable, const FString& Value, const FString& After, const FString& Name)
{
	static_cast<UObject*>(Rig)->Modify();
	URigVMNode* Node = UnrealAIControlRigGraph::AddSetVariable(Rig, Variable, Value, After, Name);
	if (!Node)
		return FString();
	Recompile(Rig);
	return Node->GetName();
}

TArray<FUnrealAIRunningRig> UUnrealAIControlRigRqlApi::RigValues(const FString& Rig)
{
	UWorld* World = UnrealMcpRql::PlayWorld();
	if (!World)
		return UnrealMcpRql::Failed<TArray<FUnrealAIRunningRig>>(TEXT("no Play In Editor session: a rig in an Anim Blueprint runs only while playing, the editor preview says nothing about the game: editor | call StartPlay(), editor | call AdvancePlay(1.0, 0), then RigValues"));
	const FString Wanted = Rig.TrimStartAndEnd();
	TArray<FUnrealAIRunningRig> Out;
	TSet<FString> Others;
	for (TObjectIterator<UControlRig> It; It; ++It)
	{
		UControlRig* Running = *It;
		const USceneComponent* Component = Running->GetTypedOuter<USceneComponent>();
		if (Running->HasAnyFlags(RF_ArchetypeObject) || !IsValid(Running) || !Component || Component->GetWorld() != World)
			continue;
		const FString RigName = RunningRigName(Running);
		if (!Wanted.IsEmpty() && !IsRigNamed(Running, Wanted))
		{
			Others.Add(RigName);
			continue;
		}
		FUnrealAIRunningRig& Row = Out.AddDefaulted_GetRef();
		Row.Rig = RigName;
		Row.Owner = Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : FString();
		Row.Component = Component->GetName();
		Row.RigOrigin = Component->GetComponentLocation();
		Row.Executions = static_cast<int32>(Running->GetRigVMExtendedExecuteContext().GetPublicData<>().GetNumExecutions());
		for (TFieldIterator<FProperty> Property(Running->GetClass()); Property; ++Property)
		{
			if (!Property->IsNative()) // the rig's variables, a parent rig's too; the rest is UControlRig itself
				Row.Variables.Add(Property->GetName(), Running->GetVariableAsString(Property->GetFName()));
		}
		if (Row.Executions == 0)
			Row.Notes.Add(TEXT("the rig has not run yet: advance the game (editor | call AdvancePlay(1.0, 0)); a mesh that is not rendered may skip its pose (the component's VisibilityBasedAnimTickOption)"));
		if (RunsWorldTrace(Running))
			Row.Notes.Add(TEXT("traces take and return rig space, relative to RigOrigin: a hit on the ground under the character has Z = ground - RigOrigin.Z, a small negative number. A miss returns (0, 0, 0) with bHit false, so read bHit too: AddSetVariable"));
	}
	if (Out.IsEmpty())
		return UnrealMcpRql::Failed<TArray<FUnrealAIRunningRig>>(Others.Num()
			? FString::Printf(TEXT("no running rig named '%s'; running: %s"), *Wanted, *FString::Join(Others.Array(), TEXT(", ")))
			: FString(TEXT("no Control Rig runs in the play world: is the rig in the AnimBlueprint of the mesh that plays (unreal.asset(\"<abp>\") | select ControlRigs), and was it compiled before StartPlay?")));
	return Out;
}

FString UUnrealAIControlRigRqlApi::SetPin(UControlRigBlueprint* Rig, const FString& Pin, const FString& Value)
{
	URigVMPin* Found = FindPin(Rig, Pin);
	if (!Found)
		return FString();
	static_cast<UObject*>(Rig)->Modify();
	TArray<URigVMNode*> Added;
	FString Result;
	if (!UnrealAIControlRigGraph::SetPin(Rig, Found, Value, Added, Result))
	{
		if (URigVMController* Controller = Rig->GetController())
			RemoveNodes(Controller, Added);
		return FString();
	}
	Recompile(Rig);
	return Result;
}

FString UUnrealAIControlRigRqlApi::Connect(UControlRigBlueprint* Rig, const FString& From, const FString& To)
{
	URigVMPin* Source = FindPin(Rig, From);
	URigVMPin* Target = Source ? FindPin(Rig, To) : nullptr;
	URigVMController* Controller = Target ? UnrealAIControlRigGraph::Controller(Rig) : nullptr;
	if (!Controller)
		return FString();
	static_cast<UObject*>(Rig)->Modify();
	if (Source->IsExecuteContext() || Target->IsExecuteContext())
	{
		// An execute link: the target's node runs after the source (replacing what ran there before).
		if (!Controller->AddLink(Source->GetPinPath(), Target->GetPinPath()))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not link the execute pin %s to %s"), *Source->GetPinPath(), *Target->GetPinPath()));
	}
	else
	{
		TArray<URigVMNode*> Added;
		FString Ignored;
		if (!UnrealAIControlRigGraph::SetPin(Rig, Target, TEXT("@") + Source->GetPinPath(), Added, Ignored))
			return FString();
	}
	Recompile(Rig);
	return FString::Printf(TEXT("%s -> %s"), *Source->GetPinPath(), *Target->GetPinPath());
}

void UUnrealAIControlRigRqlApi::RemoveNode(UControlRigBlueprint* Rig, const FString& Node)
{
	URigVMNode* Found = FindNode(Rig, Node);
	URigVMController* Controller = Found ? UnrealAIControlRigGraph::Controller(Rig) : nullptr;
	if (!Controller)
		return;
	// What ran before it keeps running what ran after it.
	URigVMPin* In = ExecuteInput(Found);
	URigVMPin* Out = ContinueOutput(Found);
	TArray<FString> SourcePaths;
	TArray<FString> TargetPaths;
	for (const URigVMPin* Pin : In ? In->GetLinkedSourcePins() : TArray<URigVMPin*>())
		SourcePaths.Add(Pin->GetPinPath());
	for (const URigVMPin* Pin : Out ? Out->GetLinkedTargetPins() : TArray<URigVMPin*>())
		TargetPaths.Add(Pin->GetPinPath());
	static_cast<UObject*>(Rig)->Modify();
	if (!Controller->RemoveNode(Found))
		return UnrealMcpRql::Failed(FString::Printf(TEXT("could not remove %s"), *Node));
	TArray<FString> Lost;
	for (const FString& Target : TargetPaths)
	{
		if (SourcePaths.Num() != 1 || !Controller->AddLink(SourcePaths[0], Target))
			Lost.Add(Target);
	}
	Recompile(Rig);
	if (Lost.Num())
		UnrealMcpRql::Fail(FString::Printf(TEXT("removed %s, but what ran after it (%s) is no longer in the execution chain: Connect it (see the Nodes member)"),
			*Node, *FString::Join(Lost, TEXT(", "))));
}

FString UUnrealAIControlRigRqlApi::AddLookAt(UControlRigBlueprint* Rig, const FString& Bone, const FString& Target, const FString& Weight, const FString& Axis, const FString& After)
{
	const FRigElementKey Key = Element(Rig, Bone, { ERigElementType::Bone, ERigElementType::Control, ERigElementType::Null }, TEXT("bone"));
	URigVMController* Controller = Key.IsValid() ? UnrealAIControlRigGraph::Controller(Rig) : nullptr;
	if (!Controller)
		return FString();
	// The bone's local axes that face the character's forward (+Y of a UE skeletal mesh) and up (+Z) in the reference pose.
	const FTransform Pose = Rig->GetHierarchy()->GetInitialGlobalTransform(Key);
	const FString Facing = Axis.TrimStartAndEnd().IsEmpty() ? AxisText(Pose.InverseTransformVectorNoScale(FVector::YAxisVector)) : AxisArgument(Axis);
	if (Facing.IsEmpty())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not an axis: X, -Y, +Z or (x, y, z)"), *Axis));
	const FString Up = AxisText(Pose.InverseTransformVectorNoScale(FVector::ZAxisVector));

	static_cast<UObject*>(Rig)->Modify();
	FString Goal = Target.TrimStartAndEnd();
	FString Space;
	TArray<URigVMNode*> Added; // everything the call adds, removed again when a later step fails
	if (Goal.StartsWith(TEXT("@")) && !Goal.Contains(TEXT(".")))
	{
		// A world-space location from outside (the Anim Blueprint): into rig space first.
		URigVMNode* FromWorld = UnrealAIControlRigGraph::AddUnit(Rig, FRigUnit_ToRigSpace_Location::StaticStruct(), TEXT("Value=") + Goal, FString(),
			FreeNodeName(Rig, FString::Printf(TEXT("LookAt_%s_Target"), *Key.Name.ToString())), &Added);
		if (!FromWorld)
			return FString();
		Goal = FString::Printf(TEXT("@%s.Global"), *FromWorld->GetName());
	}
	else if (!Goal.StartsWith(TEXT("@")) && !Goal.StartsWith(TEXT("(")) && !Goal.StartsWith(TEXT("[")))
	{
		// An element: aim at its location (the target expressed in its space at the origin).
		const FRigElementKey GoalKey = Element(Rig, Goal, { ERigElementType::Control, ERigElementType::Bone, ERigElementType::Null }, TEXT("target"));
		if (!GoalKey.IsValid())
			return FString();
		Space = FString::Printf(TEXT(", Primary.Space=%s:%s"), *StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(GoalKey.Type)), *GoalKey.Name.ToString());
		Goal = TEXT("(0, 0, 0)");
	}
	const FString Settings = FString::Printf(TEXT("Item=%s:%s, Primary.Axis=%s, Primary.Target=%s, Primary.Kind=Location%s, Secondary.Axis=%s, Secondary.Target=(X=0,Y=0,Z=1), Secondary.Kind=Direction, Weight=%s"),
		*StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Key.Type)), *Key.Name.ToString(), *Facing, *Goal, *Space, *Up,
		Weight.TrimStartAndEnd().IsEmpty() ? TEXT("1") : *Weight.TrimStartAndEnd());
	URigVMNode* Aim = UnrealAIControlRigGraph::AddUnit(Rig, FRigUnit_AimItem::StaticStruct(), Settings, After,
		FreeNodeName(Rig, FString::Printf(TEXT("LookAt_%s"), *Key.Name.ToString())));
	if (!Aim)
	{
		RemoveNodes(Controller, Added);
		return FString();
	}
	Recompile(Rig);
	return Aim->GetName();
}

FString UUnrealAIControlRigRqlApi::AddToAnimBlueprint(UAnimBlueprint* AnimBlueprint, UControlRigBlueprint* Rig, const FString& After, bool bBindVariables)
{
	AnimBlueprint->Modify();
	return UnrealAIControlRigAnim::AddRig(AnimBlueprint, Rig, After, bBindVariables);
}

FString UUnrealAIControlRigRqlApi::SetRigInput(UAnimBlueprint* AnimBlueprint, const FString& Node, const FString& Input, const FString& Value)
{
	AnimBlueprint->Modify();
	return UnrealAIControlRigAnim::SetInput(AnimBlueprint, Node, Input, Value);
}

FString UUnrealAIControlRigRqlApi::TrackNearestActor(UAnimBlueprint* AnimBlueprint, const FString& ActorClass, const FString& LocationVariable,
	const FString& WeightVariable, float Radius, float Speed)
{
	AnimBlueprint->Modify();
	return UnrealAIControlRigAnim::TrackNearestActor(AnimBlueprint, ActorClass, LocationVariable, WeightVariable, Radius, Speed);
}
