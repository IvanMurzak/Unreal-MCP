// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIControlRigAnim.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIControlRigUnits.h"

#include "AnimGraphNode_ControlRig.h"
#include "AnimGraphNode_Root.h"
#include "AnimationGraphSchema.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "K2Node.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ITransaction.h"
#include "Rigs/RigHierarchy.h"

namespace UnrealAIControlRigAnimLocal
{
	using UnrealAIControlRigUnits::Compact;

	FString Title(const UEdGraphNode* Node)
	{
		return Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}

	/** The Control Rig blueprint (or class) a Control Rig node runs, by name. */
	FString RigName(const UAnimGraphNode_ControlRig* Node)
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		// 5.8: a node runs a rig blueprint's class or a Control Rig runtime asset.
		const FControlRigAssetStrongReference Reference = Node->Node.GetControlRigAssetReference();
		UClass* Class = Reference.GetBlueprintClass();
		if (!Class)
		{
			const UObject* Asset = Reference.Get();
			return Asset ? Asset->GetName() : FString(TEXT("none"));
		}
#else
		UClass* Class = Node->Node.GetControlRigClass();
#endif
		if (const UBlueprint* Blueprint = Class ? UBlueprint::GetBlueprintFromClass(Class) : nullptr)
			return Blueprint->GetName();
		return Class ? Class->GetName() : FString(TEXT("none"));
	}

	FString NodeLabel(const UEdGraphNode* Node)
	{
		if (const UAnimGraphNode_ControlRig* Rig = Cast<UAnimGraphNode_ControlRig>(Node))
			return FString::Printf(TEXT("%s (Control Rig %s)"), *Node->GetName(), *RigName(Rig));
		return FString::Printf(TEXT("%s (%s)"), *Node->GetName(), *Title(Node));
	}

	UEdGraph* AnimGraph(UAnimBlueprint* AnimBlueprint)
	{
		for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph && Graph->GetSchema()->IsA<UAnimationGraphSchema>())
				return Graph;
		}
		return UnrealMcpRql::Failed<UEdGraph*>(FString::Printf(TEXT("%s has no AnimGraph (a template or an animation layer interface)"), *AnimBlueprint->GetName()));
	}

	UEdGraphPin* PosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && !Pin->bHidden && UAnimationGraphSchema::IsPosePin(Pin->PinType))
				return Pin;
		}
		return nullptr;
	}

	/** A node of @p Class created in @p Graph at @p X, @p Y (@p Configure runs before its pins exist), as FGraphNodeCreator does. */
	UEdGraphNode* CreateNode(UEdGraph* Graph, UClass* Class, double X, double Y, TFunctionRef<void(UEdGraphNode*)> Configure)
	{
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, Class, NAME_None, RF_Transactional);
		Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		Node->NodePosX = FMath::RoundToInt(X);
		Node->NodePosY = FMath::RoundToInt(Y);
		Configure(Node);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		if (Node->Pins.Num() == 0)
			Node->AllocateDefaultPins();
		return Node;
	}

	UK2Node_CallFunction* CallNode(UEdGraph* Graph, UClass* Owner, FName Function, double X, double Y)
	{
		UFunction* Found = Owner->FindFunctionByName(Function);
		if (!Found)
			return UnrealMcpRql::Failed<UK2Node_CallFunction*>(FString::Printf(TEXT("this engine has no %s.%s"), *Owner->GetName(), *Function.ToString()));
		return CastChecked<UK2Node_CallFunction>(CreateNode(Graph, UK2Node_CallFunction::StaticClass(), X, Y,
			[Found](UEdGraphNode* Node) { CastChecked<UK2Node_CallFunction>(Node)->SetFromFunction(Found); }));
	}

	UEdGraphPin* PinOf(UEdGraphNode* Node, const FName Name, EEdGraphPinDirection Direction = EGPD_MAX)
	{
		UEdGraphPin* Pin = Node ? Node->FindPin(Name, Direction) : nullptr;
		if (Node && !Pin)
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no pin %s"), *Title(Node), *Name.ToString()));
		return Pin;
	}

	bool Link(UEdGraphPin* A, UEdGraphPin* B)
	{
		if (!A || !B)
			return false;
		const UEdGraphSchema* Schema = A->GetOwningNode()->GetGraph()->GetSchema();
		if (Schema->TryCreateConnection(A, B))
			return true;
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not connect %s.%s to %s.%s: %s"), *Title(A->GetOwningNode()), *A->PinName.ToString(),
			*Title(B->GetOwningNode()), *B->PinName.ToString(), *Schema->CanCreateConnection(A, B).Message.ToString()));
		return false;
	}

	void SetDefault(UEdGraphPin* Pin, const FString& Value)
	{
		if (Pin)
			Pin->GetSchema()->TrySetDefaultValue(*Pin, Value);
	}

	/** Compile the rig outside the mutation's transaction (a compile rebuilds classes, it is no edit to undo); false after Fail. */
	bool CompileRig(UControlRigBlueprint* Rig)
	{
		if (Rig->GeneratedClass && (Rig->Status == BS_UpToDate || Rig->Status == BS_UpToDateWithWarnings))
			return true;
		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		{
			TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
			FKismetEditorUtilities::CompileBlueprint(Rig, EBlueprintCompileOptions::None, &Results);
		}
		if (!Rig->GeneratedClass || Rig->Status == BS_Error || Results.NumErrors > 0)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s does not compile: see unreal.asset(\"%s\") | call Compile()"), *Rig->GetName(), *Rig->GetPathName()));
		return true;
	}

	/** Whether the Blueprint has a graph (function, macro, event graph) named @p Name: a variable cannot take that name. */
	bool HasGraphNamed(UBlueprint* Blueprint, const FName Name)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		return Graphs.ContainsByPredicate([Name](const UEdGraph* Graph) { return Graph && Graph->GetFName() == Name; });
	}

	/**
	 * The Anim Blueprint variable @p Name of type @p Type: reused when it exists with that type, added otherwise (only checked
	 * when !@p bApply); false after Fail.
	 */
	bool EnsureVariable(UBlueprint* Blueprint, const FName Name, const FEdGraphPinType& Type, FString& OutNote, bool bApply = true)
	{
		const FString TypeText = UEdGraphSchema_K2::TypeToText(Type).ToString();
		FEdGraphPinType Existing;
		bool bExists = false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
		if (Index != INDEX_NONE)
		{
			Existing = Blueprint->NewVariables[Index].VarType;
			bExists = true;
		}
		else if (const FProperty* Inherited = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass->FindPropertyByName(Name) : nullptr)
		{
			bExists = GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Inherited, Existing);
		}
		if (bExists)
		{
			if (Existing.PinCategory != Type.PinCategory || Existing.PinSubCategoryObject != Type.PinSubCategoryObject || Existing.ContainerType != Type.ContainerType)
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s already has a variable %s of type %s, not %s: rename it, or pick another variable name"),
					*Blueprint->GetName(), *Name.ToString(), *UEdGraphSchema_K2::TypeToText(Existing).ToString(), *TypeText));
			OutNote = TEXT("existing variable");
			return true;
		}
		if (HasGraphNamed(Blueprint, Name) || !FName(Name).IsValidXName(INVALID_OBJECTNAME_CHARACTERS))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s cannot be a variable name of %s"), *Name.ToString(), *Blueprint->GetName()));
		if (!bApply)
			return true;
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, Type))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("could not add the variable %s to %s"), *Name.ToString(), *Blueprint->GetName()));
		OutNote = FString::Printf(TEXT("new %s variable"), *TypeText);
		return true;
	}

	UEdGraphNode* VariableNode(UBlueprint* Blueprint, UEdGraph* Graph, const FName Name, bool bGetter, double X, double Y)
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		UEdGraphNode* Node = bGetter
			? static_cast<UEdGraphNode*>(Schema->SpawnVariableGetNode(FVector2D(X, Y), Graph, Name, Blueprint->SkeletonGeneratedClass))
			: static_cast<UEdGraphNode*>(Schema->SpawnVariableSetNode(FVector2D(X, Y), Graph, Name, Blueprint->SkeletonGeneratedClass));
		return Node ? Node : UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("could not add a %s node of %s"), bGetter ? TEXT("Get") : TEXT("Set"), *Name.ToString()));
	}

	/** Remove the nodes of @p Graph that are not in @p Before: what a failing operation added. */
	void RemoveAddedNodes(UBlueprint* Blueprint, UEdGraph* Graph, const TSet<UEdGraphNode*>& Before)
	{
		for (UEdGraphNode* Node : TArray<UEdGraphNode*>(Graph->Nodes))
		{
			if (Node && !Before.Contains(Node))
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile*/ true);
		}
	}

	/** An actor class by name: a class ("StaticMeshActor"), a Blueprint by name ("BP_Orb", "BP_Orb_C") or path. */
	UClass* ActorClass(const FString& Name)
	{
		UClass* Class = UnrealMcpRql::ResolveClass(Name);
		Class = Class ? Class : UnrealMcpRql::ResolveClass(Name + TEXT("_C"));
		if (!Class && !Name.Contains(TEXT("/")))
		{
			// A Blueprint that is not loaded yet: by its asset name.
			TArray<FAssetData> Assets;
			FARFilter Filter;
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			Filter.PackagePaths.Add(TEXT("/Game"));
			Filter.bRecursivePaths = true;
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
					Class = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
					break;
				}
			}
		}
		return Class && Class->IsChildOf(AActor::StaticClass()) ? Class : nullptr;
	}

	TArray<UAnimGraphNode_ControlRig*> RigNodes(UAnimBlueprint* AnimBlueprint)
	{
		TArray<UAnimGraphNode_ControlRig*> Nodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(AnimBlueprint, Nodes);
		return Nodes;
	}

	/** The Control Rig node named @p Name (node name or the rig it runs; "" = the only one); null after Fail. */
	UAnimGraphNode_ControlRig* FindRigNode(UAnimBlueprint* AnimBlueprint, const FString& Name)
	{
		const TArray<UAnimGraphNode_ControlRig*> Nodes = RigNodes(AnimBlueprint);
		if (Nodes.Num() == 0)
			return UnrealMcpRql::Failed<UAnimGraphNode_ControlRig*>(FString::Printf(TEXT("%s has no Control Rig node: add one with AddToAnimBlueprint"), *AnimBlueprint->GetName()));
		const FString Wanted = Name.TrimStartAndEnd();
		TArray<UAnimGraphNode_ControlRig*> Matches;
		for (UAnimGraphNode_ControlRig* Node : Nodes)
		{
			if (Wanted.IsEmpty() || Node->GetName().Equals(Wanted, ESearchCase::IgnoreCase) || RigName(Node).Equals(Wanted, ESearchCase::IgnoreCase))
				Matches.Add(Node);
		}
		if (Matches.Num() == 1)
			return Matches[0];
		TArray<FString> Labels;
		for (const UAnimGraphNode_ControlRig* Node : Matches.Num() ? Matches : Nodes)
			Labels.Add(NodeLabel(Node));
		return UnrealMcpRql::Failed<UAnimGraphNode_ControlRig*>(FString::Printf(TEXT("%s: %s (Control Rig nodes: %s)"), *AnimBlueprint->GetName(),
			Matches.Num() > 1 ? TEXT("several Control Rig nodes match: pass the node name") : *FString::Printf(TEXT("no Control Rig node '%s'"), *Name),
			*FString::Join(Labels, TEXT(", "))));
	}

	/** The AnimGraph node named @p Name: node name, title ("Output Pose", "Locomotion") or, for a Control Rig node, its rig. */
	UEdGraphNode* FindAnimNode(UEdGraph* Graph, const FString& Name)
	{
		TArray<UEdGraphNode*> Matches;
		const FString WantedTitle = Compact(Name);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UAnimGraphNode_ControlRig* Rig = Cast<UAnimGraphNode_ControlRig>(Node);
			if (Node->GetName().Equals(Name, ESearchCase::IgnoreCase) || Compact(Title(Node)) == WantedTitle || (Rig && RigName(Rig).Equals(Name, ESearchCase::IgnoreCase)))
				Matches.Add(Node);
		}
		if (Matches.Num() == 1)
			return Matches[0];
		TArray<FString> Labels;
		for (const UEdGraphNode* Node : Matches.Num() ? Matches : Graph->Nodes)
		{
			if (PosePin(const_cast<UEdGraphNode*>(Node), EGPD_Output))
				Labels.Add(NodeLabel(Node));
		}
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s: %s '%s' (pose nodes: %s)"), *Graph->GetName(),
			Matches.Num() > 1 ? TEXT("several nodes match") : TEXT("no node"), *Name, *FString::Join(Labels, TEXT(", "))));
	}

	/** Show the rig input @p Input as a pin of @p Node (the Details panel's "Use Pin" box); the pin, or null after Fail. */
	UEdGraphPin* ExposeInput(UAnimGraphNode_ControlRig* Node, const FString& Input)
	{
		// CustomPinProperties is protected; it is a UPROPERTY, so reflection reaches it.
		const FArrayProperty* Property = FindFProperty<FArrayProperty>(UAnimGraphNode_CustomProperty::StaticClass(), TEXT("CustomPinProperties"));
		const FStructProperty* Inner = Property ? CastField<FStructProperty>(Property->Inner) : nullptr;
		if (!Inner || Inner->Struct != FOptionalPinFromProperty::StaticStruct())
			return UnrealMcpRql::Failed<UEdGraphPin*>(TEXT("this engine's Control Rig node has no exposable inputs list"));
		FScriptArrayHelper Pins(Property, Property->ContainerPtrToValuePtr<void>(Node));
		TArray<FString> Names;
		for (int32 Index = 0; Index < Pins.Num(); ++Index)
		{
			FOptionalPinFromProperty& Pin = *reinterpret_cast<FOptionalPinFromProperty*>(Pins.GetRawPtr(Index));
			Names.Add(Pin.PropertyName.ToString());
			if (!Pin.PropertyName.ToString().Equals(Input.TrimStartAndEnd(), ESearchCase::IgnoreCase))
				continue;
			if (!Pin.bShowPin)
			{
				Node->Modify();
				Pin.bShowPin = true;
				// As the Details panel's checkbox does: a pin replaces a curve mapping of the input (FAnimNode_ControlRig::InputMapping).
				const FStructProperty* NodeProperty = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node"));
				const FMapProperty* Mapping = NodeProperty ? FindFProperty<FMapProperty>(NodeProperty->Struct, TEXT("InputMapping")) : nullptr;
				if (Mapping && Mapping->KeyProp->IsA<FNameProperty>())
				{
					FScriptMapHelper Map(Mapping, Mapping->ContainerPtrToValuePtr<void>(NodeProperty->ContainerPtrToValuePtr<void>(Node)));
					const FName Key = Pin.PropertyName;
					const int32 Found = Map.FindMapIndexWithKey(&Key);
					if (Found != INDEX_NONE)
						Map.RemoveAt(Found);
				}
				const FName Name = Pin.PropertyName; // ReconstructNode rebuilds the list
				Node->ReconstructNode();
				return PinOf(Node, Name, EGPD_Input);
			}
			return PinOf(Node, Pin.PropertyName, EGPD_Input);
		}
		return UnrealMcpRql::Failed<UEdGraphPin*>(FString::Printf(TEXT("%s's rig %s has no input '%s' (inputs: %s; a rig variable is an input when it is public: AddVariable(..., true))"),
			*Node->GetName(), *RigName(Node), *Input, Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none")));
	}

	/** Bind @p Pin to the Anim Blueprint variable @p Variable (added with the pin's type when missing); a note, or empty after Fail. */
	FString BindInput(UAnimBlueprint* AnimBlueprint, UEdGraphPin* Pin, const FName Variable)
	{
		FString Note;
		if (!EnsureVariable(AnimBlueprint, Variable, Pin->PinType, Note))
			return FString();
		UEdGraphNode* Owner = Pin->GetOwningNode();
		Owner->Modify();
		Pin->GetSchema()->BreakPinLinks(*Pin, /*bSendsNodeNotifcation*/ true);
		const int32 Row = Owner->Pins.IndexOfByKey(Pin);
		UEdGraphNode* Getter = VariableNode(AnimBlueprint, Owner->GetGraph(), Variable, /*bGetter*/ true, Owner->NodePosX - 320, Owner->NodePosY + 40 * Row);
		if (!Getter || !Link(PinOf(Getter, Variable, EGPD_Output), Pin))
			return FString();
		return FString::Printf(TEXT("%s <- %s (%s)"), *Pin->PinName.ToString(), *Variable.ToString(), *Note);
	}

	/** "(1, 2, 3)" -> "1,2,3": the text form of a vector / rotator default on a Blueprint pin. */
	FString PinDefaultText(const UEdGraphPin* Pin, const FString& Value)
	{
		FString Text = Value.TrimStartAndEnd();
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && !Text.Contains(TEXT("=")))
		{
			Text.RemoveFromStart(TEXT("("));
			Text.RemoveFromEnd(TEXT(")"));
			Text.ReplaceInline(TEXT(" "), TEXT(""));
		}
		return Text;
	}

	FString PinValue(const UEdGraphPin* Pin)
	{
		if (Pin->LinkedTo.Num())
		{
			const UEdGraphNode* Source = Pin->LinkedTo[0]->GetOwningNode();
			if (const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Source))
				return TEXT("@") + Getter->GetVarName().ToString();
			return FString::Printf(TEXT("@%s.%s"), *Source->GetName(), *Pin->LinkedTo[0]->PinName.ToString());
		}
		return Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : Pin->DefaultValue;
	}
}

using namespace UnrealAIControlRigAnimLocal;

FString UnrealAIControlRigAnim::AddRig(UAnimBlueprint* AnimBlueprint, UControlRigBlueprint* Rig, const FString& After, bool bBindVariables)
{
	UEdGraph* Graph = AnimGraph(AnimBlueprint);
	if (!Graph)
		return FString();
	const URigHierarchy* Hierarchy = Rig->GetHierarchy();
	TArray<FRigElementKey> Bones = Hierarchy ? Hierarchy->GetAllKeys(/*bTraverse*/ false, ERigElementType::Bone) : TArray<FRigElementKey>();
	if (Bones.Num() == 0)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no bones: create it from the character's mesh (CreateRig(path, mesh)) so it drives the skeleton's bones"), *Rig->GetName()));
	if (const USkeleton* Skeleton = AnimBlueprint->TargetSkeleton)
	{
		const bool bShared = Bones.ContainsByPredicate([Skeleton](const FRigElementKey& Bone) { return Skeleton->GetReferenceSkeleton().FindBoneIndex(Bone.Name) != INDEX_NONE; });
		if (!bShared)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("none of %s's bones (%s, ...) is a bone of %s's skeleton %s: create the rig from that skeleton's mesh"),
				*Rig->GetName(), *Bones[0].Name.ToString(), *AnimBlueprint->GetName(), *Skeleton->GetName()));
	}

	// The rig compiles first: a compile reconstructs the nodes that run it, so pins are read after it.
	if (!CompileRig(Rig))
		return FString();
	// Where it goes: between the node After names and what it feeds, else right before Output Pose. Resolved before any change.
	UEdGraphPin* Into = nullptr;
	UEdGraphPin* From = nullptr;
	if (After.TrimStartAndEnd().IsEmpty())
	{
		TArray<UAnimGraphNode_Root*> Roots;
		Graph->GetNodesOfClass(Roots);
		Into = Roots.Num() ? PosePin(Roots[0], EGPD_Input) : nullptr;
		if (!Into)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s's AnimGraph has no Output Pose"), *AnimBlueprint->GetName()));
		From = Into->LinkedTo.Num() ? Into->LinkedTo[0] : nullptr;
	}
	else
	{
		UEdGraphNode* Previous = FindAnimNode(Graph, After.TrimStartAndEnd());
		if (!Previous)
			return FString();
		From = PosePin(Previous, EGPD_Output);
		Into = From && From->LinkedTo.Num() ? From->LinkedTo[0] : nullptr;
		if (!Into)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s %s"), *NodeLabel(Previous), From ? TEXT("feeds no pose: connect it first") : TEXT("has no output pose")));
	}
	// The inputs' Anim Blueprint variables are checked before the graph changes (a name taken by another type, a function).
	TArray<FName> Inputs;
	for (const FRigVMGraphVariableDescription& Variable : Rig->GetMemberVariables())
	{
		if (!Variable.bPublic || !bBindVariables)
			continue;
		Inputs.Add(Variable.Name);
		if (HasGraphNamed(AnimBlueprint, Variable.Name))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has a function named %s, the name of a rig input: rename one, or pass bBindVariables false and bind with SetRigInput"),
				*AnimBlueprint->GetName(), *Variable.Name.ToString()));
	}

	Graph->Modify();
	TSet<UEdGraphNode*> Before(Graph->Nodes);
	UEdGraphNode* const Source0 = From ? From->GetOwningNode() : nullptr;
	const FName FromName = From ? From->PinName : NAME_None;
	UEdGraphNode* const Into0 = Into->GetOwningNode();
	const FName IntoName = Into->PinName;
	auto Undo = [&]()
	{
		// What a failure leaves: the pose chain as it was, none of the new nodes.
		RemoveAddedNodes(AnimBlueprint, Graph, Before);
		UEdGraphPin* OldFrom = Source0 ? Source0->FindPin(FromName, EGPD_Output) : nullptr;
		UEdGraphPin* OldInto = Into0->FindPin(IntoName, EGPD_Input);
		if (OldFrom && OldInto && !OldInto->LinkedTo.Contains(OldFrom))
			Graph->GetSchema()->TryCreateConnection(OldFrom, OldInto);
		return FString();
	};
	UEdGraphNode* Target = Into->GetOwningNode();
	const UEdGraphNode* Source = From ? From->GetOwningNode() : nullptr;
	const double X = Source ? (Source->NodePosX + Target->NodePosX) / 2.0 : Target->NodePosX - 320.0;
	UClass* RigClass = Rig->GeneratedClass;
	UAnimGraphNode_ControlRig* Node = CastChecked<UAnimGraphNode_ControlRig>(CreateNode(Graph, UAnimGraphNode_ControlRig::StaticClass(), X, Target->NodePosY + 160.0,
		[RigClass](UEdGraphNode* Created) { CastChecked<UAnimGraphNode_ControlRig>(Created)->Node.SetControlRigClass(RigClass); }));
	UEdGraphPin* NodeIn = PosePin(Node, EGPD_Input);
	UEdGraphPin* NodeOut = PosePin(Node, EGPD_Output);
	if (!NodeIn || !NodeOut)
	{
		Undo();
		return UnrealMcpRql::Failed<FString>(TEXT("the Control Rig node has no pose pins"));
	}
	if (From)
	{
		Graph->GetSchema()->BreakSinglePinLink(From, Into);
		if (!Link(From, NodeIn))
			return Undo();
	}
	if (!Link(NodeOut, Into))
		return Undo();

	TArray<FString> Bound;
	for (const FName& Input : Inputs)
	{
		UEdGraphPin* Pin = ExposeInput(Node, Input.ToString());
		const FString Binding = Pin ? BindInput(AnimBlueprint, Pin, Input) : FString();
		if (Binding.IsEmpty())
			return Undo();
		Bound.Add(Binding);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	return FString::Printf(TEXT("%s runs %s between %s and %s%s%s"), *Node->GetName(), *Rig->GetName(), Source ? *Title(Source) : TEXT("the reference pose"),
		*Title(Target), Bound.Num() ? TEXT("; inputs: ") : TEXT(""), *FString::Join(Bound, TEXT(", ")));
}

FString UnrealAIControlRigAnim::SetInput(UAnimBlueprint* AnimBlueprint, const FString& NodeName, const FString& Input, const FString& Value)
{
	UAnimGraphNode_ControlRig* Node = FindRigNode(AnimBlueprint, NodeName);
	UEdGraphPin* Pin = Node ? ExposeInput(Node, Input) : nullptr;
	if (!Pin)
		return FString();
	const FString Trimmed = Value.TrimStartAndEnd();
	if (Trimmed.StartsWith(TEXT("@")))
	{
		const FString Bound = BindInput(AnimBlueprint, Pin, FName(*Trimmed.RightChop(1).TrimStartAndEnd()));
		if (!Bound.IsEmpty())
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
		return Bound;
	}
	const UEdGraphSchema* Schema = Pin->GetSchema();
	const FString Text = PinDefaultText(Pin, Trimmed);
	const FString Invalid = Schema->IsPinDefaultValid(Pin, Text, nullptr, FText::GetEmpty());
	if (!Invalid.IsEmpty())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a %s value: %s"), *Value, *UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString(), *Invalid));
	Node->Modify();
	Schema->BreakPinLinks(*Pin, /*bSendsNodeNotifcation*/ true); // a constant replaces a binding
	Schema->TrySetDefaultValue(*Pin, Text);
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBlueprint);
	return FString::Printf(TEXT("%s = %s"), *Pin->PinName.ToString(), *PinValue(Pin));
}

FString UnrealAIControlRigAnim::TrackNearestActor(UAnimBlueprint* AnimBlueprint, const FString& ActorClass, const FString& LocationVariable,
	const FString& WeightVariable, float Radius, float Speed)
{
	// Everything is checked first; the new nodes are built and wired among themselves; the event changes last.
	UClass* Class = UnrealAIControlRigAnimLocal::ActorClass(ActorClass.TrimStartAndEnd());
	if (!Class)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not an actor class (a Blueprint such as BP_Orb, or a class such as StaticMeshActor)"), *ActorClass));
	if (Radius <= 0.0f || Speed < 0.0f)
		return UnrealMcpRql::Failed<FString>(TEXT("Radius must be above 0 (cm) and Speed 0 or more (0 = no smoothing)"));
	const FName Location(*LocationVariable.TrimStartAndEnd());
	const FName Weight(*WeightVariable.TrimStartAndEnd());
	if (Location.IsNone())
		return UnrealMcpRql::Failed<FString>(TEXT("name the Vector variable that gets the target's location (e.g. LookAtTarget)"));
	if (Location == Weight)
		return UnrealMcpRql::Failed<FString>(TEXT("the location and the weight need two variables"));
	UK2Node_Event* Update = FBlueprintEditorUtils::FindOverrideForFunction(AnimBlueprint, UAnimInstance::StaticClass(), GET_FUNCTION_NAME_CHECKED(UAnimInstance, BlueprintUpdateAnimation));
	if (Update && !Update->IsNodeEnabled() && !Update->IsAutomaticallyPlacedGhostNode())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s's Event Blueprint Update Animation is disabled: enable it first"), *AnimBlueprint->GetName()));
	UEdGraph* Graph = Update ? Update->GetGraph() : FBlueprintEditorUtils::FindEventGraph(AnimBlueprint);
	if (!Graph)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no event graph"), *AnimBlueprint->GetName()));
	TArray<UK2Node_VariableSet*> Setters;
	FBlueprintEditorUtils::GetAllNodesOfClass(AnimBlueprint, Setters);
	for (const UK2Node_VariableSet* Setter : Setters)
	{
		if (Setter->GetVarName() == Location)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s's event graph already sets %s: remove those nodes first or pass another variable"), *AnimBlueprint->GetName(), *Location.ToString()));
	}
	FEdGraphPinType VectorType;
	VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	FEdGraphPinType RealType;
	RealType.PinCategory = UEdGraphSchema_K2::PC_Real;
	RealType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	FString LocationNote;
	FString WeightNote;
	const bool bVariablesFit = EnsureVariable(AnimBlueprint, Location, VectorType, LocationNote, /*bApply*/ false)
		&& (Weight.IsNone() || EnsureVariable(AnimBlueprint, Weight, RealType, WeightNote, /*bApply*/ false));
	if (!bVariablesFit || !EnsureVariable(AnimBlueprint, Location, VectorType, LocationNote)
		|| (!Weight.IsNone() && !EnsureVariable(AnimBlueprint, Weight, RealType, WeightNote)))
		return FString();

	Graph->Modify();
	const TSet<UEdGraphNode*> Before(Graph->Nodes);
	auto Undo = [AnimBlueprint, Graph, &Before]()
	{
		RemoveAddedNodes(AnimBlueprint, Graph, Before);
		return FString();
	};
	const FVector2D Origin = Update ? FVector2D(Update->NodePosX, Update->NodePosY) : FVector2D(Graph->GetGoodPlaceForNewNode());
	const double X = Origin.X;
	double Y = Origin.Y;
	for (const UEdGraphNode* Node : Graph->Nodes)
		Y = FMath::Max<double>(Y, Node->NodePosY + 400.0);
	const FString SpeedText = FString::SanitizeFloat(Speed, 0);
	const FString RadiusText = FString::SanitizeFloat(Radius, 0);

	// The tracking: every actor of the class but the owner, the nearest one in range, its location smoothed, the weight blended.
	const double X0 = X + 560;
	UK2Node_CallFunction* GetAll = CallNode(Graph, UGameplayStatics::StaticClass(), GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetAllActorsOfClass), X0, Y);
	UFunction* RemoveItem = UKismetArrayLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_RemoveItem));
	UK2Node_CallFunction* NotOwner = RemoveItem ? CastChecked<UK2Node_CallFunction>(CreateNode(Graph, UK2Node_CallArrayFunction::StaticClass(), X0 + 300, Y,
		[RemoveItem](UEdGraphNode* Node) { CastChecked<UK2Node_CallFunction>(Node)->SetFromFunction(RemoveItem); })) : nullptr;
	UK2Node_CallFunction* Owner = CallNode(Graph, UAnimInstance::StaticClass(), GET_FUNCTION_NAME_CHECKED(UAnimInstance, GetOwningActor), X0, Y + 260);
	UK2Node_CallFunction* OwnerLocation = CallNode(Graph, AActor::StaticClass(), GET_FUNCTION_NAME_CHECKED(AActor, K2_GetActorLocation), X0 + 260, Y + 260);
	UK2Node_CallFunction* Nearest = CallNode(Graph, UGameplayStatics::StaticClass(), GET_FUNCTION_NAME_CHECKED(UGameplayStatics, FindNearestActor), X0 + 560, Y + 200);
	UK2Node_CallFunction* InRange = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_DoubleDouble), X0 + 860, Y + 260);
	UEdGraphNode* Branch = CreateNode(Graph, UK2Node_IfThenElse::StaticClass(), X0 + 1100, Y, [](UEdGraphNode*) {});
	UK2Node_CallFunction* TargetLocation = CallNode(Graph, AActor::StaticClass(), GET_FUNCTION_NAME_CHECKED(AActor, K2_GetActorLocation), X0 + 1100, Y + 300);
	UEdGraphNode* GetLocation = VariableNode(AnimBlueprint, Graph, Location, true, X0 + 1100, Y + 440);
	UK2Node_CallFunction* Smooth = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, VInterpTo), X0 + 1400, Y + 300);
	UEdGraphNode* SetLocation = VariableNode(AnimBlueprint, Graph, Location, false, X0 + 1960, Y);
	// The owner as the class (null when it is not one): the item Array_RemoveItem takes for that class's array.
	UK2Node_DynamicCast* OwnerAsClass = Class == AActor::StaticClass() ? nullptr : CastChecked<UK2Node_DynamicCast>(CreateNode(Graph,
		UK2Node_DynamicCast::StaticClass(), X0 + 260, Y + 120, [Class](UEdGraphNode* Node)
		{
			UK2Node_DynamicCast* CastNode = CastChecked<UK2Node_DynamicCast>(Node);
			CastNode->TargetType = Class;
			CastNode->SetPurity(true);
		}));
	UEdGraphPin* ClassPin = GetAll ? PinOf(GetAll, TEXT("ActorClass")) : nullptr;
	if (!GetAll || !NotOwner || !Owner || !OwnerLocation || !Nearest || !InRange || !TargetLocation || !GetLocation || !Smooth || !SetLocation || !ClassPin)
	{
		Undo();
		return RemoveItem ? FString() : UnrealMcpRql::Failed<FString>(TEXT("this engine has no Array_RemoveItem"));
	}
	GetAll->GetSchema()->TrySetDefaultObject(*ClassPin, Class);
	SetDefault(PinOf(InRange, TEXT("B")), RadiusText);
	SetDefault(PinOf(Smooth, TEXT("InterpSpeed")), SpeedText);
	UEdGraphPin* DeltaTime = nullptr; // the event's, linked once the event is known (below)
	bool bLinked = Link(PinOf(GetAll, UEdGraphSchema_K2::PN_Then), PinOf(NotOwner, UEdGraphSchema_K2::PN_Execute))
		&& Link(PinOf(GetAll, TEXT("OutActors")), PinOf(NotOwner, TEXT("TargetArray")))
		&& (OwnerAsClass
			? Link(PinOf(Owner, UEdGraphSchema_K2::PN_ReturnValue), OwnerAsClass->GetCastSourcePin()) && Link(OwnerAsClass->GetCastResultPin(), PinOf(NotOwner, TEXT("Item")))
			: Link(PinOf(Owner, UEdGraphSchema_K2::PN_ReturnValue), PinOf(NotOwner, TEXT("Item"))))
		&& Link(PinOf(Owner, UEdGraphSchema_K2::PN_ReturnValue), PinOf(OwnerLocation, UEdGraphSchema_K2::PN_Self))
		&& Link(PinOf(OwnerLocation, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Nearest, TEXT("Origin")))
		&& Link(PinOf(GetAll, TEXT("OutActors")), PinOf(Nearest, TEXT("ActorsToCheck")))
		&& Link(PinOf(Nearest, TEXT("Distance")), PinOf(InRange, TEXT("A")))
		&& Link(PinOf(NotOwner, UEdGraphSchema_K2::PN_Then), PinOf(Branch, UEdGraphSchema_K2::PN_Execute))
		&& Link(PinOf(InRange, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Branch, UEdGraphSchema_K2::PN_Condition))
		&& Link(PinOf(Nearest, UEdGraphSchema_K2::PN_ReturnValue), PinOf(TargetLocation, UEdGraphSchema_K2::PN_Self))
		&& Link(PinOf(GetLocation, Location, EGPD_Output), PinOf(Smooth, TEXT("Current")))
		&& Link(PinOf(TargetLocation, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Smooth, TEXT("Target")))
		&& Link(PinOf(Branch, UEdGraphSchema_K2::PN_Then), PinOf(SetLocation, UEdGraphSchema_K2::PN_Execute));
	TArray<UEdGraphPin*> DeltaTimeInputs = { PinOf(Smooth, TEXT("DeltaTime")) };
	if (bLinked && Weight.IsNone())
	{
		bLinked = Link(PinOf(Smooth, UEdGraphSchema_K2::PN_ReturnValue), PinOf(SetLocation, Location, EGPD_Input));
	}
	else if (bLinked)
	{
		// The weight blends to 1 in range and to 0 out of it; a target picked up from weight ~0 starts at its location (no sweep from the last one).
		UEdGraphNode* GetWeight = VariableNode(AnimBlueprint, Graph, Weight, true, X0 + 1100, Y + 560);
		UK2Node_CallFunction* Fresh = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_DoubleDouble), X0 + 1400, Y + 560);
		UK2Node_CallFunction* Pick = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectVector), X0 + 1680, Y + 300);
		UK2Node_CallFunction* BlendIn = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, FInterpTo), X0 + 1960, Y + 300);
		UK2Node_CallFunction* BlendOut = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, FInterpTo), X0 + 1960, Y + 520);
		UEdGraphNode* SetWeightIn = VariableNode(AnimBlueprint, Graph, Weight, false, X0 + 2260, Y);
		UEdGraphNode* SetWeightOut = VariableNode(AnimBlueprint, Graph, Weight, false, X0 + 2260, Y + 200);
		bLinked = GetWeight && Fresh && Pick && BlendIn && BlendOut && SetWeightIn && SetWeightOut;
		if (bLinked)
		{
			SetDefault(PinOf(Fresh, TEXT("B")), TEXT("0.05"));
			SetDefault(PinOf(BlendIn, TEXT("Target")), TEXT("1.0"));
			SetDefault(PinOf(BlendOut, TEXT("Target")), TEXT("0.0"));
			SetDefault(PinOf(BlendIn, TEXT("InterpSpeed")), SpeedText);
			SetDefault(PinOf(BlendOut, TEXT("InterpSpeed")), SpeedText);
			DeltaTimeInputs.Append({ PinOf(BlendIn, TEXT("DeltaTime")), PinOf(BlendOut, TEXT("DeltaTime")) });
			bLinked = Link(PinOf(GetWeight, Weight, EGPD_Output), PinOf(Fresh, TEXT("A")))
				&& Link(PinOf(TargetLocation, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Pick, TEXT("A")))
				&& Link(PinOf(Smooth, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Pick, TEXT("B")))
				&& Link(PinOf(Fresh, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Pick, TEXT("bPickA")))
				&& Link(PinOf(Pick, UEdGraphSchema_K2::PN_ReturnValue), PinOf(SetLocation, Location, EGPD_Input))
				&& Link(PinOf(GetWeight, Weight, EGPD_Output), PinOf(BlendIn, TEXT("Current")))
				&& Link(PinOf(GetWeight, Weight, EGPD_Output), PinOf(BlendOut, TEXT("Current")))
				&& Link(PinOf(SetLocation, UEdGraphSchema_K2::PN_Then), PinOf(SetWeightIn, UEdGraphSchema_K2::PN_Execute))
				&& Link(PinOf(BlendIn, UEdGraphSchema_K2::PN_ReturnValue), PinOf(SetWeightIn, Weight, EGPD_Input))
				&& Link(PinOf(Branch, UEdGraphSchema_K2::PN_Else), PinOf(SetWeightOut, UEdGraphSchema_K2::PN_Execute))
				&& Link(PinOf(BlendOut, UEdGraphSchema_K2::PN_ReturnValue), PinOf(SetWeightOut, Weight, EGPD_Input));
		}
	}
	if (!bLinked)
		return Undo();

	// The event, last: the Blueprint's own, the editor's placeholder (enabled now), or a new one. What it ran before keeps
	// running first (a Sequence); a parent Anim Blueprint's update runs first when the event starts overriding now.
	UEdGraphPin* Entry = PinOf(GetAll, UEdGraphSchema_K2::PN_Execute);
	if (!Update)
	{
		Update = CastChecked<UK2Node_Event>(CreateNode(Graph, UK2Node_Event::StaticClass(), X, Y, [](UEdGraphNode* Node)
		{
			UK2Node_Event* Event = CastChecked<UK2Node_Event>(Node);
			Event->EventReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UAnimInstance, BlueprintUpdateAnimation), UAnimInstance::StaticClass());
			Event->bOverrideFunction = true;
		}));
	}
	UEdGraphPin* Then = PinOf(Update, UEdGraphSchema_K2::PN_Then);
	DeltaTime = PinOf(Update, TEXT("DeltaTimeX"));
	if (!Then || !DeltaTime)
		return Undo();
	for (UEdGraphPin* Input : DeltaTimeInputs)
	{
		if (!Link(DeltaTime, Input))
			return Undo();
	}
	const bool bStartsOverriding = !Update->IsNodeEnabled() || Update->IsAutomaticallyPlacedGhostNode() || !Before.Contains(Update);
	UFunction* Inherited = AnimBlueprint->ParentClass ? AnimBlueprint->ParentClass->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UAnimInstance, BlueprintUpdateAnimation)) : nullptr;
	UEdGraphPin* Cursor = Then;
	if (Then->LinkedTo.Num())
	{
		// By node and name: a node can rebuild its pins when its execution input is disconnected.
		TArray<TPair<UEdGraphNode*, FName>> Next;
		for (const UEdGraphPin* Pin : Then->LinkedTo)
			Next.Emplace(Pin->GetOwningNode(), Pin->PinName);
		UEdGraphNode* Sequence = CreateNode(Graph, UK2Node_ExecutionSequence::StaticClass(), X + 260, Y, [](UEdGraphNode*) {});
		Update->Modify();
		Then->GetSchema()->BreakPinLinks(*Then, /*bSendsNodeNotifcation*/ true);
		bool bSpliced = Link(Then, PinOf(Sequence, UEdGraphSchema_K2::PN_Execute));
		for (const TPair<UEdGraphNode*, FName>& Pin : Next)
			bSpliced = bSpliced && Link(PinOf(Sequence, TEXT("then_0")), PinOf(Pin.Key, Pin.Value, EGPD_Input));
		if (!bSpliced)
		{
			Then->GetSchema()->BreakPinLinks(*Then, /*bSendsNodeNotifcation*/ true);
			for (const TPair<UEdGraphNode*, FName>& Pin : Next)
			{
				if (UEdGraphPin* Restored = Pin.Key->FindPin(Pin.Value, EGPD_Input))
					Then->GetSchema()->TryCreateConnection(Then, Restored);
			}
			return Undo();
		}
		Cursor = PinOf(Sequence, TEXT("then_1"));
	}
	else if (bStartsOverriding && Inherited && Cast<UBlueprintGeneratedClass>(Inherited->GetOuterUClass()))
	{
		UK2Node_CallFunction* Parent = CastChecked<UK2Node_CallFunction>(CreateNode(Graph, UK2Node_CallParentFunction::StaticClass(), X + 260, Y,
			[Inherited](UEdGraphNode* Node) { CastChecked<UK2Node_CallFunction>(Node)->SetFromFunction(Inherited); }));
		if (!Link(Then, PinOf(Parent, UEdGraphSchema_K2::PN_Execute)) || !Link(DeltaTime, PinOf(Parent, TEXT("DeltaTimeX"))))
			return Undo();
		Cursor = PinOf(Parent, UEdGraphSchema_K2::PN_Then);
	}
	if (!Link(Cursor, Entry))
		return Undo();
	if (!Update->IsNodeEnabled() || Update->IsAutomaticallyPlacedGhostNode())
	{
		Update->Modify();
		Update->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
		Update->NodeComment.Empty(); // the placeholder's "This node is disabled ..." comment
		Update->bCommentBubbleVisible = false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	const FString Smoothing = Speed > 0.0f ? FString::Printf(TEXT("smoothed at speed %s"), *SpeedText) : FString(TEXT("not smoothed"));
	return FString::Printf(TEXT("Event Blueprint Update Animation: %s (%s) follows the nearest %s within %s cm of the owner (%s)%s"),
		*Location.ToString(), *LocationNote, *Class->GetName(), *RadiusText, *Smoothing,
		Weight.IsNone() ? TEXT("") : *FString::Printf(TEXT("; %s (%s) blends to 1 in range and to 0 out of range"), *Weight.ToString(), *WeightNote));
}

TSharedPtr<FJsonValue> UnrealAIControlRigAnim::ControlRigsJson(UObject* Object)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Object);
	if (!AnimBlueprint)
		return MakeShared<FJsonValueArray>(Rows);
	for (UAnimGraphNode_ControlRig* Node : RigNodes(AnimBlueprint))
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Node"), Node->GetName());
		Row->SetStringField(TEXT("Rig"), RigName(Node));
		Row->SetStringField(TEXT("Graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
		const UEdGraphPin* In = PosePin(Node, EGPD_Input);
		const UEdGraphPin* Out = PosePin(Node, EGPD_Output);
		Row->SetStringField(TEXT("After"), In && In->LinkedTo.Num() ? Title(In->LinkedTo[0]->GetOwningNode()) : FString());
		Row->SetStringField(TEXT("Before"), Out && Out->LinkedTo.Num() ? Title(Out->LinkedTo[0]->GetOwningNode()) : FString());
		TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && !Pin->bHidden && !UAnimationGraphSchema::IsPosePin(Pin->PinType))
				Inputs->SetStringField(Pin->PinName.ToString(), PinValue(Pin));
		}
		Row->SetObjectField(TEXT("Inputs"), Inputs);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return MakeShared<FJsonValueArray>(Rows);
}
