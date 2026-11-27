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
#include "Animation/AnimNodeBase.h"
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
#include "Misc/DefaultValueHelper.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/StringOutputDevice.h"
#include "Misc/ITransaction.h"
#include "Rigs/RigHierarchy.h"
#include "UObject/StructOnScope.h"

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

	/** The rig's inputs of @p Node (CustomPinProperties: protected, but a UPROPERTY, so reflection reaches it); null after Fail. */
	TArray<FOptionalPinFromProperty>* RigInputs(UAnimGraphNode_ControlRig* Node)
	{
		const FArrayProperty* Property = FindFProperty<FArrayProperty>(UAnimGraphNode_CustomProperty::StaticClass(), TEXT("CustomPinProperties"));
		const FStructProperty* Inner = Property ? CastField<FStructProperty>(Property->Inner) : nullptr;
		if (!Inner || Inner->Struct != FOptionalPinFromProperty::StaticStruct())
			return UnrealMcpRql::Failed<TArray<FOptionalPinFromProperty>*>(TEXT("this engine's Control Rig node has no exposable inputs list"));
		return Property->ContainerPtrToValuePtr<TArray<FOptionalPinFromProperty>>(Node);
	}

	/** The entry of @p List named @p Name (case-insensitive); null when none. */
	FOptionalPinFromProperty* FindOptionalPin(TArray<FOptionalPinFromProperty>& List, const FString& Name)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		return List.FindByPredicate([&Wanted](const FOptionalPinFromProperty& Pin) { return Pin.PropertyName.ToString().Equals(Wanted, ESearchCase::IgnoreCase); });
	}

	FString OptionalPinNames(const TArray<FOptionalPinFromProperty>& List)
	{
		TArray<FString> Names;
		for (const FOptionalPinFromProperty& Pin : List)
			Names.Add(Pin.PropertyName.ToString());
		return Names.Num() ? FString::Join(Names, TEXT(", ")) : FString(TEXT("none"));
	}

	/** Show the rig input @p Input of @p Node as a pin (the Details panel's "Use Pin" box); the pin, or null after Fail. */
	UEdGraphPin* ShowInput(UAnimGraphNode_ControlRig* Node, FOptionalPinFromProperty& Input)
	{
		const FName Name = Input.PropertyName; // ReconstructNode rebuilds the list
		if (!Input.bShowPin)
		{
			Node->Modify();
			Input.bShowPin = true;
			// As the Details panel's checkbox does: a pin replaces a curve mapping of the input (FAnimNode_ControlRig::InputMapping).
			const FMapProperty* Mapping = FindFProperty<FMapProperty>(FAnimNode_ControlRig::StaticStruct(), TEXT("InputMapping"));
			if (Mapping && Mapping->KeyProp->IsA<FNameProperty>())
			{
				FScriptMapHelper Map(Mapping, Mapping->ContainerPtrToValuePtr<void>(&Node->Node));
				const int32 Found = Map.FindMapIndexWithKey(&Name);
				if (Found != INDEX_NONE)
					Map.RemoveAt(Found);
			}
			Node->ReconstructNode();
		}
		return PinOf(Node, Name, EGPD_Input);
	}

	/** Show the rig input named @p Input of @p Node as a pin; the pin, or null after Fail. */
	UEdGraphPin* ExposeInput(UAnimGraphNode_ControlRig* Node, const FString& Input)
	{
		TArray<FOptionalPinFromProperty>* Inputs = RigInputs(Node);
		FOptionalPinFromProperty* Found = Inputs ? FindOptionalPin(*Inputs, Input) : nullptr;
		if (Inputs && !Found)
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s's rig %s has no input '%s' (inputs: %s; a rig variable is an input when it is public: AddVariable(..., true))"),
				*Node->GetName(), *RigName(Node), *Input, *OptionalPinNames(*Inputs)));
		return Found ? ShowInput(Node, *Found) : nullptr;
	}

	/**
	 * The anim node's own settings: the editable properties of FAnimNode_ControlRig (Alpha, AlphaInputType, LODThreshold, ...),
	 * without the rig it runs (AddToAnimBlueprint's), the input pose (Source) or object lists (AssetUserData).
	 */
	TArray<FProperty*> NodeSettings()
	{
		TArray<FProperty*> Settings;
		for (TFieldIterator<FProperty> It(FAnimNode_ControlRig::StaticStruct()); It; ++It)
		{
			const FStructProperty* Link = CastField<FStructProperty>(*It);
			const FArrayProperty* List = CastField<FArrayProperty>(*It);
			if (It->HasAnyPropertyFlags(CPF_Edit) && !It->IsA<FObjectPropertyBase>() && It->GetMetaData(TEXT("Category")) != TEXT("ControlRig")
				&& !(Link && Link->Struct->IsChildOf(FPoseLinkBase::StaticStruct())) && !(List && List->Inner->IsA<FObjectPropertyBase>()))
				Settings.Add(*It);
		}
		return Settings;
	}

	/** The setting @p Name of @p Settings, by property or display name; null when none. */
	FProperty* FindSetting(const TArray<FProperty*>& Settings, const FString& Name)
	{
		FProperty* const* Found = Settings.FindByPredicate([&Name](const FProperty* Property)
		{
			return Property->GetName().Equals(Name.TrimStartAndEnd(), ESearchCase::IgnoreCase) || Compact(Property->GetDisplayNameText().ToString()) == Compact(Name);
		});
		return Found ? *Found : nullptr;
	}

	FString SettingText(const UAnimGraphNode_ControlRig* Node, const FProperty* Setting)
	{
		FString Text;
		Setting->ExportText_InContainer(0, Text, &Node->Node, &Node->Node, const_cast<UAnimGraphNode_ControlRig*>(Node), PPF_None);
		return Text;
	}

	/**
	 * Import @p Text into @p Setting of the node struct at @p Container; empty when it is a whole value of the setting ("0.5"
	 * or "1.0f" for Alpha, "-1" for LODThreshold, "Bool" for AlphaInputType), else why not.
	 */
	FString ImportSetting(const FProperty* Setting, const FString& Text, void* Container, UObject* Owner)
	{
		FString Value = Text;
		const FNumericProperty* Number = CastField<FNumericProperty>(Setting);
		if (Number && !Number->IsEnum())
		{
			// A number's import reads "abc", "-" or "." as 0: the text is checked first.
			if (Number->IsFloatingPoint() && (Value.EndsWith(TEXT("f")) || Value.EndsWith(TEXT("F"))))
				Value.LeftChopInline(1);
			if (Number->IsFloatingPoint() ? !FDefaultValueHelper::IsStringValidFloat(Value) : !FDefaultValueHelper::IsStringValidInteger(Value))
				return FString::Printf(TEXT("not a%s number"), Number->IsFloatingPoint() ? TEXT("") : TEXT(" whole"));
		}
		if (Value.IsEmpty())
			return TEXT("empty");
		FStringOutputDevice Errors;
		const TCHAR* Rest = Setting->ImportText_InContainer(*Value, Container, Owner, PPF_None, &Errors);
		if (!Rest || !FString(Rest).TrimStartAndEnd().IsEmpty())
			return Errors.IsEmpty() ? FString(TEXT("not a value of it")) : FString(Errors).TrimStartAndEnd();
		return FString();
	}

	/** Why @p Text is no value of @p Setting (tried on a copy of the node's struct); empty when it is one. */
	FString SettingRefuses(const UAnimGraphNode_ControlRig* Node, const FProperty* Setting, const FString& Text)
	{
		FStructOnScope Copy(FAnimNode_ControlRig::StaticStruct());
		FAnimNode_ControlRig::StaticStruct()->CopyScriptStruct(Copy.GetStructMemory(), &Node->Node);
		return ImportSetting(Setting, Text, Copy.GetStructMemory(), const_cast<UAnimGraphNode_ControlRig*>(Node));
	}

	/** The shown pin of the node setting @p Setting; null when it has none (LODThreshold, Alpha while AlphaInputType is not Float). */
	UEdGraphPin* ShownSettingPin(const UAnimGraphNode_ControlRig* Node, const FProperty* Setting)
	{
		UEdGraphPin* Pin = Node->FindPin(Setting->GetFName(), EGPD_Input);
		return Pin && !Pin->bHidden ? Pin : nullptr;
	}

	/**
	 * The pin of the node setting @p Setting to bind, a hidden optional pin (LODThreshold) shown first; null when the setting
	 * cannot have one now (Alpha while AlphaInputType is not Float).
	 */
	UEdGraphPin* ShowSettingPin(UAnimGraphNode_ControlRig* Node, const FProperty* Setting)
	{
		if (UEdGraphPin* Pin = ShownSettingPin(Node, Setting))
			return Pin;
		FOptionalPinFromProperty* Optional = FindOptionalPin(Node->ShowPinForProperties, Setting->GetName());
		if (!Optional || !Optional->bCanToggleVisibility || Optional->bShowPin)
			return nullptr;
		Node->Modify();
		Optional->bShowPin = true;
		Node->ReconstructNode();
		return ShownSettingPin(Node, Setting);
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
	// Output Pose without an input (the node feeding it was removed, e.g. a Control Rig node this one replaces) takes the one
	// pose that now feeds nothing, as the removed node did; else the rig starts from the reference pose, and says so.
	UEdGraphPin* Into = nullptr;
	UEdGraphPin* From = nullptr;
	FString Note;
	TArray<UAnimGraphNode_Root*> Roots;
	Graph->GetNodesOfClass(Roots);
	UEdGraphPin* Output = Roots.Num() ? PosePin(Roots[0], EGPD_Input) : nullptr;
	auto LoosePose = [Graph, Output, &Note]() -> UEdGraphPin*
	{
		// Only a pose Output Pose (local space, as the rig node's input) can take: not a component-space one.
		TArray<UEdGraphPin*> Loose;
		TArray<FString> Labels;
		for (UEdGraphNode* Candidate : Graph->Nodes)
		{
			UEdGraphPin* Pin = Candidate && !Candidate->IsA<UAnimGraphNode_Root>() ? PosePin(Candidate, EGPD_Output) : nullptr;
			if (Pin && Pin->LinkedTo.Num() == 0 && Graph->GetSchema()->CanCreateConnection(Pin, Output).Response != CONNECT_RESPONSE_DISALLOW)
			{
				Loose.Add(Pin);
				Labels.Add(NodeLabel(Candidate));
			}
		}
		if (Loose.Num() == 1)
		{
			Note = FString::Printf(TEXT(" (Output Pose had no input: reconnected %s, the only pose that fed nothing)"), *Labels[0]);
			return Loose[0];
		}
		Note = Loose.Num()
			? FString::Printf(TEXT(" (WARNING: Output Pose had no input and several poses feed nothing: %s; the rig starts from the reference pose: remove this node and pass After to pick one)"), *FString::Join(Labels, TEXT(", ")))
			: FString(TEXT(" (Output Pose had no input: the rig starts from the reference pose)"));
		return nullptr;
	};
	if (After.TrimStartAndEnd().IsEmpty())
	{
		Into = Output;
		if (!Into)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s's AnimGraph has no Output Pose"), *AnimBlueprint->GetName()));
		From = Into->LinkedTo.Num() ? Into->LinkedTo[0] : LoosePose();
	}
	else
	{
		UEdGraphNode* Previous = FindAnimNode(Graph, After.TrimStartAndEnd());
		if (!Previous)
			return FString();
		From = PosePin(Previous, EGPD_Output);
		Into = From && From->LinkedTo.Num() ? From->LinkedTo[0] : nullptr;
		if (!Into && From && Output && Output->LinkedTo.Num() == 0)
		{
			// The node feeds nothing and Output Pose has no input (what fed it was removed): the rig goes between them.
			Into = Output;
			Note = FString::Printf(TEXT(" (%s fed nothing and Output Pose had no input: both are connected through the rig)"), *Title(Previous));
		}
		if (!Into)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s %s"), *NodeLabel(Previous), From ? TEXT("feeds no pose: connect it first") : TEXT("has no output pose")));
	}
	// The inputs' Anim Blueprint variables are checked before the graph changes (a name taken by another type, a function).
	TArray<FName> Inputs;
	for (const FRigVMGraphVariableDescription& Variable : Rig->GetMemberVariables())
	{
		if (!Variable.bPublic)
			continue;
		Inputs.Add(Variable.Name);
		if (bBindVariables && HasGraphNamed(AnimBlueprint, Variable.Name))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has a function named %s, the name of a rig input: rename one, or pass bBindVariables false and bind with SetRigInput"),
				*AnimBlueprint->GetName(), *Variable.Name.ToString()));
	}

	Graph->Modify();
	TSet<UEdGraphNode*> Before(Graph->Nodes);
	UEdGraphNode* const Source0 = From ? From->GetOwningNode() : nullptr;
	const FName FromName = From ? From->PinName : NAME_None;
	UEdGraphNode* const Into0 = Into->GetOwningNode();
	const FName IntoName = Into->PinName;
	const bool bWasLinked = From && Into->LinkedTo.Contains(From);
	auto Undo = [&]()
	{
		// What a failure leaves: the pose chain as it was, none of the new nodes.
		RemoveAddedNodes(AnimBlueprint, Graph, Before);
		UEdGraphPin* OldFrom = Source0 ? Source0->FindPin(FromName, EGPD_Output) : nullptr;
		UEdGraphPin* OldInto = Into0->FindPin(IntoName, EGPD_Input);
		if (bWasLinked && OldFrom && OldInto && !OldInto->LinkedTo.Contains(OldFrom))
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

	// Every public rig variable is an input pin: bound to the Anim Blueprint variable of its name, or left unbound for
	// SetRigInput / a link (a hidden input has no pin to link).
	TArray<FString> Shown;
	for (const FName& Input : Inputs)
	{
		UEdGraphPin* Pin = ExposeInput(Node, Input.ToString());
		const FString Binding = Pin && bBindVariables ? BindInput(AnimBlueprint, Pin, Input) : Pin ? Pin->PinName.ToString() : FString();
		if (Binding.IsEmpty())
			return Undo();
		Shown.Add(Binding);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	const FString InputsText = Shown.IsEmpty() ? FString()
		: bBindVariables ? TEXT("; inputs: ") + FString::Join(Shown, TEXT(", "))
		: FString::Printf(TEXT("; unbound input pins: %s (set or bind each with SetRigInput(abp, \"%s\", input, \"@Variable\" | value))"), *FString::Join(Shown, TEXT(", ")), *Node->GetName());
	return FString::Printf(TEXT("%s runs %s between %s and %s%s%s"), *Node->GetName(), *Rig->GetName(), Source ? *Title(Source) : TEXT("the reference pose"),
		*Title(Target), *Note, *InputsText);
}

FString UnrealAIControlRigAnim::SetInput(UAnimBlueprint* AnimBlueprint, const FString& NodeName, const FString& Input, const FString& Value)
{
	UAnimGraphNode_ControlRig* Node = FindRigNode(AnimBlueprint, NodeName);
	TArray<FOptionalPinFromProperty>* Inputs = Node ? RigInputs(Node) : nullptr;
	if (!Inputs)
		return FString();
	const FString Trimmed = Value.TrimStartAndEnd();
	const bool bBind = Trimmed.StartsWith(TEXT("@"));
	const FName Variable(*Trimmed.RightChop(1).TrimStartAndEnd());
	// A rig input first, else the anim node's own setting (Alpha, LODThreshold, ...): a pin when it has one shown, else the property.
	FOptionalPinFromProperty* RigInput = FindOptionalPin(*Inputs, Input);
	FProperty* Setting = nullptr;
	if (!RigInput)
	{
		const TArray<FProperty*> Settings = NodeSettings();
		Setting = FindSetting(Settings, Input);
		if (!Setting)
		{
			TArray<FString> Names;
			for (const FProperty* Property : Settings)
				Names.Add(Property->GetName());
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no rig input or node setting '%s' (rig %s's inputs: %s, a rig variable is an input when it is public: AddVariable(..., true); node settings: %s)"),
				*Node->GetName(), *Input, *RigName(Node), *OptionalPinNames(*Inputs), *FString::Join(Names, TEXT(", "))));
		}
	}
	if (bBind)
	{
		// The variable is checked before a hidden pin is shown, so a refused binding changes nothing.
		const FProperty* Typed = Setting;
		if (RigInput && !RigInput->bShowPin)
			Typed = Node->GetTargetClass() ? Node->GetTargetClass()->FindPropertyByName(RigInput->PropertyName) : nullptr;
		const UEdGraphPin* Existing = Node->FindPin(Setting ? Setting->GetFName() : RigInput->PropertyName, EGPD_Input);
		FEdGraphPinType Type;
		bool bTyped = Existing && !Existing->bHidden;
		if (bTyped)
			Type = Existing->PinType;
		else
			bTyped = Typed && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Typed, Type);
		FString Note;
		if (bTyped && !EnsureVariable(AnimBlueprint, Variable, Type, Note, /*bApply*/ false))
			return FString();
		UEdGraphPin* Pin = Setting ? ShowSettingPin(Node, Setting) : ShowInput(Node, *RigInput);
		if (!Pin && Setting)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s's setting %s has no pin to bind now (another setting hides it, as AlphaInputType hides Alpha unless it is Float): set a constant, or change that setting first"),
				*Node->GetName(), *Setting->GetName()));
		const FString Bound = Pin ? BindInput(AnimBlueprint, Pin, Variable) : FString();
		if (!Bound.IsEmpty())
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
		return Bound;
	}
	UEdGraphPin* Pin = Setting ? ShownSettingPin(Node, Setting) : ShowInput(Node, *RigInput);
	if (!Pin && !Setting)
		return FString();
	// Everything is checked before anything changes: the pin's text form, then the property's (on a copy of the node's struct).
	const FString Text = Pin ? PinDefaultText(Pin, Trimmed) : FString();
	if (Pin)
	{
		const FString Invalid = Pin->GetSchema()->IsPinDefaultValid(Pin, Text, nullptr, FText::GetEmpty());
		if (!Invalid.IsEmpty())
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a %s value: %s"), *Value, *UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString(), *Invalid));
	}
	if (Setting)
	{
		const FString Refused = SettingRefuses(Node, Setting, Trimmed);
		if (!Refused.IsEmpty())
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a %s value for %s (%s); it stays %s"), *Value, *Setting->GetCPPType(), *Setting->GetName(), *Refused, *SettingText(Node, Setting)));
	}
	Node->Modify();
	if (Setting)
	{
		// The property is the value while no pin is shown, and the pin's default once one is (kept the same).
		ImportSetting(Setting, Trimmed, &Node->Node, Node);
		if (UEdGraphPin* Hidden = Pin ? nullptr : Node->FindPin(Setting->GetFName(), EGPD_Input))
			SetDefault(Hidden, PinDefaultText(Hidden, Trimmed));
	}
	if (Pin)
	{
		Pin->GetSchema()->BreakPinLinks(*Pin, /*bSendsNodeNotifcation*/ true); // a constant replaces a binding
		SetDefault(Pin, Text);
	}
	if (Setting)
	{
		// As the Details panel does: a new AlphaInputType shows its pin and hides the others.
		FPropertyChangedEvent Changed(Setting, EPropertyChangeType::ValueSet);
		Node->PostEditChangeProperty(Changed);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBlueprint);
	if (Setting)
	{
		const UEdGraphPin* Shown = ShownSettingPin(Node, Setting); // the node may have been rebuilt
		return FString::Printf(TEXT("%s = %s (node setting)"), *Setting->GetName(), Shown ? *PinValue(Shown) : *SettingText(Node, Setting));
	}
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
		TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
		for (const FProperty* Setting : NodeSettings())
		{
			// A shown pin is the value (a link shows as @Variable), the property otherwise.
			const UEdGraphPin* Shown = ShownSettingPin(Node, Setting);
			Settings->SetStringField(Setting->GetName(), Shown ? PinValue(Shown) : SettingText(Node, Setting));
		}
		Row->SetObjectField(TEXT("Settings"), Settings);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return MakeShared<FJsonValueArray>(Rows);
}
