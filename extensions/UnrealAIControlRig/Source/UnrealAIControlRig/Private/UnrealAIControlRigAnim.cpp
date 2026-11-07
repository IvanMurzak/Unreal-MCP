// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIControlRigAnim.h"

#include "IUnrealMcpRqlModule.h"

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
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ITransaction.h"
#include "Rigs/RigHierarchy.h"

namespace UnrealAIControlRigAnimLocal
{
	FString Squash(const FString& Text)
	{
		FString Out;
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsWhitespace(Char) && Char != TEXT('_'))
				Out.AppendChar(FChar::ToLower(Char));
		}
		return Out;
	}

	FString Title(const UEdGraphNode* Node)
	{
		return Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}

	/** The Control Rig blueprint (or class) a Control Rig node runs, by name. */
	FString RigName(const UAnimGraphNode_ControlRig* Node)
	{
		UClass* Class = Node->Node.GetControlRigClass();
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

	/** The Anim Blueprint variable @p Name of type @p Type: reused when it exists with that type, added otherwise; false after Fail. */
	bool EnsureVariable(UBlueprint* Blueprint, const FName Name, const FEdGraphPinType& Type, FString& OutNote)
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
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s already has a variable %s of type %s, not %s: pass another name"),
					*Blueprint->GetName(), *Name.ToString(), *UEdGraphSchema_K2::TypeToText(Existing).ToString(), *TypeText));
			OutNote = TEXT("existing variable");
			return true;
		}
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
		if (Nodes.Num() == 0)
			return UnrealMcpRql::Failed<UAnimGraphNode_ControlRig*>(FString::Printf(TEXT("%s has no Control Rig node: add one with AddToAnimBlueprint"), *AnimBlueprint->GetName()));
		return UnrealMcpRql::Failed<UAnimGraphNode_ControlRig*>(FString::Printf(TEXT("%s: %s (Control Rig nodes: %s)"), *AnimBlueprint->GetName(),
			Matches.Num() > 1 ? TEXT("several Control Rig nodes match: pass the node name") : *FString::Printf(TEXT("no Control Rig node '%s'"), *Name),
			*FString::Join(Labels, TEXT(", "))));
	}

	/** The AnimGraph node named @p Name: node name, title ("Output Pose", "Locomotion") or, for a Control Rig node, its rig. */
	UEdGraphNode* FindAnimNode(UEdGraph* Graph, const FString& Name)
	{
		TArray<UEdGraphNode*> Matches;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UAnimGraphNode_ControlRig* Rig = Cast<UAnimGraphNode_ControlRig>(Node);
			if (Node->GetName().Equals(Name, ESearchCase::IgnoreCase) || Squash(Title(Node)) == Squash(Name) || (Rig && RigName(Rig).Equals(Name, ESearchCase::IgnoreCase)))
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
	if (!CompileRig(Rig))
		return FString();

	Graph->Modify();
	UEdGraphNode* Target = Into->GetOwningNode();
	const UEdGraphNode* Source = From ? From->GetOwningNode() : nullptr;
	const double X = Source ? (Source->NodePosX + Target->NodePosX) / 2.0 : Target->NodePosX - 320.0;
	UClass* RigClass = Rig->GeneratedClass;
	UAnimGraphNode_ControlRig* Node = CastChecked<UAnimGraphNode_ControlRig>(CreateNode(Graph, UAnimGraphNode_ControlRig::StaticClass(), X, Target->NodePosY + 160.0,
		[RigClass](UEdGraphNode* Created) { CastChecked<UAnimGraphNode_ControlRig>(Created)->Node.SetControlRigClass(RigClass); }));
	UEdGraphPin* NodeIn = PosePin(Node, EGPD_Input);
	UEdGraphPin* NodeOut = PosePin(Node, EGPD_Output);
	if (!NodeIn || !NodeOut)
		return UnrealMcpRql::Failed<FString>(TEXT("the Control Rig node has no pose pins"));
	if (From)
	{
		Graph->GetSchema()->BreakSinglePinLink(From, Into);
		if (!Link(From, NodeIn))
			return FString();
	}
	if (!Link(NodeOut, Into))
		return FString();

	TArray<FString> Inputs;
	if (bBindVariables)
	{
		for (const FRigVMGraphVariableDescription& Variable : Rig->GetMemberVariables())
		{
			if (!Variable.bPublic)
				continue;
			UEdGraphPin* Pin = ExposeInput(Node, Variable.Name.ToString());
			const FString Bound = Pin ? BindInput(AnimBlueprint, Pin, Variable.Name) : FString();
			if (Bound.IsEmpty())
				return FString();
			Inputs.Add(Bound);
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	return FString::Printf(TEXT("%s runs %s between %s and %s%s%s"), *Node->GetName(), *Rig->GetName(), Source ? *Title(Source) : TEXT("the reference pose"),
		*Title(Target), Inputs.Num() ? TEXT("; inputs: ") : TEXT(""), *FString::Join(Inputs, TEXT(", ")));
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
	Node->Modify();
	const UEdGraphSchema* Schema = Pin->GetSchema();
	Schema->BreakPinLinks(*Pin, /*bSendsNodeNotifcation*/ true);
	const FString Text = PinDefaultText(Pin, Trimmed);
	const FString Invalid = Schema->IsPinDefaultValid(Pin, Text, nullptr, FText::GetEmpty());
	if (!Invalid.IsEmpty())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a %s value: %s"), *Value, *UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString(), *Invalid));
	Schema->TrySetDefaultValue(*Pin, Text);
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBlueprint);
	return FString::Printf(TEXT("%s = %s"), *Pin->PinName.ToString(), *PinValue(Pin));
}

FString UnrealAIControlRigAnim::TrackNearestActor(UAnimBlueprint* AnimBlueprint, const FString& ActorClass, const FString& LocationVariable,
	const FString& WeightVariable, float Radius, float Speed)
{
	UClass* Class = UnrealMcpRql::ResolveClass(ActorClass.TrimStartAndEnd());
	if (!Class || !Class->IsChildOf(AActor::StaticClass()))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not an actor class (a Blueprint such as BP_Orb, or a class such as StaticMeshActor)"), *ActorClass));
	if (Radius <= 0.0f || Speed < 0.0f)
		return UnrealMcpRql::Failed<FString>(TEXT("Radius must be above 0 (cm) and Speed 0 or more (0 = no smoothing)"));
	const FName Location(*LocationVariable.TrimStartAndEnd());
	const FName Weight(*WeightVariable.TrimStartAndEnd());
	if (Location.IsNone())
		return UnrealMcpRql::Failed<FString>(TEXT("name the Vector variable that gets the target's location (e.g. LookAtTarget)"));
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(AnimBlueprint);
	if (!Graph)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no event graph"), *AnimBlueprint->GetName()));
	TArray<UK2Node_VariableSet*> Setters;
	FBlueprintEditorUtils::GetAllNodesOfClass(AnimBlueprint, Setters);
	for (const UK2Node_VariableSet* Setter : Setters)
	{
		if (Setter->GetVarName() == Location)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s's event graph already sets %s: remove those nodes first or pass another variable"), *AnimBlueprint->GetName(), *Location.ToString()));
	}
	UK2Node_Event* Update = FBlueprintEditorUtils::FindOverrideForFunction(AnimBlueprint, UAnimInstance::StaticClass(), GET_FUNCTION_NAME_CHECKED(UAnimInstance, BlueprintUpdateAnimation));
	if (Update && !Update->IsNodeEnabled() && !Update->IsAutomaticallyPlacedGhostNode())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s's Event Blueprint Update Animation is disabled: enable it first"), *AnimBlueprint->GetName()));

	FEdGraphPinType VectorType;
	VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	FEdGraphPinType RealType;
	RealType.PinCategory = UEdGraphSchema_K2::PC_Real;
	RealType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	FString LocationNote;
	FString WeightNote;
	if (!EnsureVariable(AnimBlueprint, Location, VectorType, LocationNote) || (!Weight.IsNone() && !EnsureVariable(AnimBlueprint, Weight, RealType, WeightNote)))
		return FString();

	// The event: the Blueprint's own, the editor's placeholder (enabled now), or a new one.
	Graph->Modify();
	if (!Update)
	{
		const FVector2D At = Graph->GetGoodPlaceForNewNode();
		Update = CastChecked<UK2Node_Event>(CreateNode(Graph, UK2Node_Event::StaticClass(), At.X, At.Y, [](UEdGraphNode* Node)
		{
			UK2Node_Event* Event = CastChecked<UK2Node_Event>(Node);
			Event->EventReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UAnimInstance, BlueprintUpdateAnimation), UAnimInstance::StaticClass());
			Event->bOverrideFunction = true;
		}));
	}
	else if (!Update->IsNodeEnabled() || Update->IsAutomaticallyPlacedGhostNode())
	{
		Update->Modify();
		Update->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
		Update->NodeComment.Empty(); // the placeholder's "This node is disabled ..." comment
		Update->bCommentBubbleVisible = false;
	}
	UEdGraphPin* Then = PinOf(Update, UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* DeltaTime = PinOf(Update, TEXT("DeltaTimeX"));
	if (!Then || !DeltaTime)
		return FString();
	const double X = Update->NodePosX;
	double Y = Update->NodePosY;
	for (const UEdGraphNode* Node : Graph->Nodes)
		Y = FMath::Max<double>(Y, Node->NodePosY + 250.0);

	// What the event ran before keeps running first (a Sequence), then the tracking.
	UEdGraphPin* Cursor = Then;
	if (Then->LinkedTo.Num())
	{
		UEdGraphNode* Sequence = CreateNode(Graph, UK2Node_ExecutionSequence::StaticClass(), X + 260, Y, [](UEdGraphNode*) {});
		// By node and name: a node can rebuild its pins when its execution input is disconnected.
		TArray<TPair<UEdGraphNode*, FName>> Before;
		for (const UEdGraphPin* Next : Then->LinkedTo)
			Before.Emplace(Next->GetOwningNode(), Next->PinName);
		Update->Modify();
		Then->GetSchema()->BreakPinLinks(*Then, /*bSendsNodeNotifcation*/ true);
		if (!Link(Then, PinOf(Sequence, UEdGraphSchema_K2::PN_Execute)))
			return FString();
		for (const TPair<UEdGraphNode*, FName>& Next : Before)
		{
			if (!Link(PinOf(Sequence, TEXT("then_0")), PinOf(Next.Key, Next.Value, EGPD_Input)))
				return FString();
		}
		Cursor = PinOf(Sequence, TEXT("then_1"));
	}
	else if (UFunction* Inherited = AnimBlueprint->ParentClass ? AnimBlueprint->ParentClass->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UAnimInstance, BlueprintUpdateAnimation)) : nullptr;
		Inherited && Cast<UBlueprintGeneratedClass>(Inherited->GetOuterUClass()))
	{
		// A parent Anim Blueprint's update keeps running: Parent: Blueprint Update Animation first.
		UK2Node_CallFunction* Parent = CastChecked<UK2Node_CallFunction>(CreateNode(Graph, UK2Node_CallParentFunction::StaticClass(), X + 260, Y,
			[Inherited](UEdGraphNode* Node) { CastChecked<UK2Node_CallFunction>(Node)->SetFromFunction(Inherited); }));
		if (!Link(Then, PinOf(Parent, UEdGraphSchema_K2::PN_Execute)) || !Link(DeltaTime, PinOf(Parent, TEXT("DeltaTimeX"))))
			return FString();
		Cursor = PinOf(Parent, UEdGraphSchema_K2::PN_Then);
	}

	const double X0 = X + 560;
	UK2Node_CallFunction* GetAll = CallNode(Graph, UGameplayStatics::StaticClass(), GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetAllActorsOfClass), X0, Y);
	UK2Node_CallFunction* Owner = CallNode(Graph, UAnimInstance::StaticClass(), GET_FUNCTION_NAME_CHECKED(UAnimInstance, GetOwningActor), X0, Y + 260);
	UK2Node_CallFunction* OwnerLocation = CallNode(Graph, AActor::StaticClass(), GET_FUNCTION_NAME_CHECKED(AActor, K2_GetActorLocation), X0 + 260, Y + 260);
	UK2Node_CallFunction* Nearest = CallNode(Graph, UGameplayStatics::StaticClass(), GET_FUNCTION_NAME_CHECKED(UGameplayStatics, FindNearestActor), X0 + 560, Y + 200);
	UK2Node_CallFunction* InRange = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_DoubleDouble), X0 + 860, Y + 260);
	UEdGraphNode* Branch = CreateNode(Graph, UK2Node_IfThenElse::StaticClass(), X0 + 1100, Y, [](UEdGraphNode*) {});
	UK2Node_CallFunction* TargetLocation = CallNode(Graph, AActor::StaticClass(), GET_FUNCTION_NAME_CHECKED(AActor, K2_GetActorLocation), X0 + 1100, Y + 300);
	UEdGraphNode* GetLocation = VariableNode(AnimBlueprint, Graph, Location, true, X0 + 1100, Y + 440);
	UK2Node_CallFunction* Smooth = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, VInterpTo), X0 + 1400, Y + 300);
	UEdGraphNode* SetLocation = VariableNode(AnimBlueprint, Graph, Location, false, X0 + 1960, Y - 60);
	if (!GetAll || !Owner || !OwnerLocation || !Nearest || !InRange || !TargetLocation || !GetLocation || !Smooth || !SetLocation)
		return FString();
	GetAll->GetSchema()->TrySetDefaultObject(*PinOf(GetAll, TEXT("ActorClass")), Class);
	SetDefault(PinOf(InRange, TEXT("B")), FString::SanitizeFloat(Radius, 0));
	SetDefault(PinOf(Smooth, TEXT("InterpSpeed")), FString::SanitizeFloat(Speed, 0));
	bool bLinked = Link(Cursor, PinOf(GetAll, UEdGraphSchema_K2::PN_Execute))
		&& Link(PinOf(Owner, UEdGraphSchema_K2::PN_ReturnValue), PinOf(OwnerLocation, UEdGraphSchema_K2::PN_Self))
		&& Link(PinOf(OwnerLocation, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Nearest, TEXT("Origin")))
		&& Link(PinOf(GetAll, TEXT("OutActors")), PinOf(Nearest, TEXT("ActorsToCheck")))
		&& Link(PinOf(Nearest, TEXT("Distance")), PinOf(InRange, TEXT("A")))
		&& Link(PinOf(GetAll, UEdGraphSchema_K2::PN_Then), PinOf(Branch, UEdGraphSchema_K2::PN_Execute))
		&& Link(PinOf(InRange, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Branch, UEdGraphSchema_K2::PN_Condition))
		&& Link(PinOf(Nearest, UEdGraphSchema_K2::PN_ReturnValue), PinOf(TargetLocation, UEdGraphSchema_K2::PN_Self))
		&& Link(PinOf(GetLocation, Location, EGPD_Output), PinOf(Smooth, TEXT("Current")))
		&& Link(PinOf(TargetLocation, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Smooth, TEXT("Target")))
		&& Link(DeltaTime, PinOf(Smooth, TEXT("DeltaTime")))
		&& Link(PinOf(Branch, UEdGraphSchema_K2::PN_Then), PinOf(SetLocation, UEdGraphSchema_K2::PN_Execute));
	if (!bLinked)
		return FString();

	if (Weight.IsNone())
	{
		if (!Link(PinOf(Smooth, UEdGraphSchema_K2::PN_ReturnValue), PinOf(SetLocation, Location, EGPD_Input)))
			return FString();
	}
	else
	{
		// The weight blends to 1 in range and to 0 out of it; a target picked up from weight ~0 starts at its location (no sweep from the last one).
		UEdGraphNode* GetWeight = VariableNode(AnimBlueprint, Graph, Weight, true, X0 + 1100, Y + 560);
		UK2Node_CallFunction* Fresh = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_DoubleDouble), X0 + 1400, Y + 560);
		UK2Node_CallFunction* Pick = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectVector), X0 + 1680, Y + 300);
		UK2Node_CallFunction* BlendIn = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, FInterpTo), X0 + 1960, Y + 300);
		UK2Node_CallFunction* BlendOut = CallNode(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, FInterpTo), X0 + 1960, Y + 520);
		UEdGraphNode* SetWeightIn = VariableNode(AnimBlueprint, Graph, Weight, false, X0 + 2260, Y - 60);
		UEdGraphNode* SetWeightOut = VariableNode(AnimBlueprint, Graph, Weight, false, X0 + 2260, Y + 160);
		if (!GetWeight || !Fresh || !Pick || !BlendIn || !BlendOut || !SetWeightIn || !SetWeightOut)
			return FString();
		SetDefault(PinOf(Fresh, TEXT("B")), TEXT("0.05"));
		SetDefault(PinOf(BlendIn, TEXT("Target")), TEXT("1.0"));
		SetDefault(PinOf(BlendOut, TEXT("Target")), TEXT("0.0"));
		SetDefault(PinOf(BlendIn, TEXT("InterpSpeed")), FString::SanitizeFloat(Speed, 0));
		SetDefault(PinOf(BlendOut, TEXT("InterpSpeed")), FString::SanitizeFloat(Speed, 0));
		bLinked = Link(PinOf(GetWeight, Weight, EGPD_Output), PinOf(Fresh, TEXT("A")))
			&& Link(PinOf(TargetLocation, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Pick, TEXT("A")))
			&& Link(PinOf(Smooth, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Pick, TEXT("B")))
			&& Link(PinOf(Fresh, UEdGraphSchema_K2::PN_ReturnValue), PinOf(Pick, TEXT("bPickA")))
			&& Link(PinOf(Pick, UEdGraphSchema_K2::PN_ReturnValue), PinOf(SetLocation, Location, EGPD_Input))
			&& Link(PinOf(GetWeight, Weight, EGPD_Output), PinOf(BlendIn, TEXT("Current")))
			&& Link(PinOf(GetWeight, Weight, EGPD_Output), PinOf(BlendOut, TEXT("Current")))
			&& Link(DeltaTime, PinOf(BlendIn, TEXT("DeltaTime")))
			&& Link(DeltaTime, PinOf(BlendOut, TEXT("DeltaTime")))
			&& Link(PinOf(SetLocation, UEdGraphSchema_K2::PN_Then), PinOf(SetWeightIn, UEdGraphSchema_K2::PN_Execute))
			&& Link(PinOf(BlendIn, UEdGraphSchema_K2::PN_ReturnValue), PinOf(SetWeightIn, Weight, EGPD_Input))
			&& Link(PinOf(Branch, UEdGraphSchema_K2::PN_Else), PinOf(SetWeightOut, UEdGraphSchema_K2::PN_Execute))
			&& Link(PinOf(BlendOut, UEdGraphSchema_K2::PN_ReturnValue), PinOf(SetWeightOut, Weight, EGPD_Input));
		if (!bLinked)
			return FString();
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	const FString Smoothing = Speed > 0.0f ? FString::Printf(TEXT("smoothed at speed %s"), *FString::SanitizeFloat(Speed, 0)) : FString(TEXT("not smoothed"));
	return FString::Printf(TEXT("Event Blueprint Update Animation: %s (%s) follows the nearest %s within %s cm of the owner (%s)%s"),
		*Location.ToString(), *LocationNote, *Class->GetName(), *FString::SanitizeFloat(Radius, 0), *Smoothing,
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
