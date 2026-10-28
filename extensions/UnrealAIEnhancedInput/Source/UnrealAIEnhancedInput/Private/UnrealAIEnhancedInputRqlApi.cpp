// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIEnhancedInputRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "K2Node_GetSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace UnrealAIEnhancedInputRqlLocal
{
	bool ValidKey(const FString& Key, FKey& Out)
	{
		Out = FKey(*Key.TrimStartAndEnd());
		if (Out.IsValid())
			return true;
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a key name (e.g. SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom)"), *Key));
		return false;
	}
}

namespace UnrealAIEnhancedInputRqlLocal
{
	/** The actor Blueprint and event graph @p Target names (the Blueprint, its event graph, or another of its event graphs); null after Fail. */
	UEdGraph* EventGraphOf(UObject* Target, UBlueprint*& OutBlueprint)
	{
		UEdGraph* Graph = Cast<UEdGraph>(Target);
		OutBlueprint = Graph ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : Cast<UBlueprint>(Target);
		if (!OutBlueprint)
			return UnrealMcpRql::Failed<UEdGraph*>(FString::Printf(TEXT("%s is not a Blueprint or a Blueprint graph"),
				Target ? *Target->GetName() : TEXT("null")));
		if (!OutBlueprint->ParentClass || !OutBlueprint->ParentClass->IsChildOf(AActor::StaticClass()))
			return UnrealMcpRql::Failed<UEdGraph*>(FString::Printf(TEXT("%s is not an actor Blueprint: input events need an actor (a Pawn, a Character, a PlayerController)"),
				*OutBlueprint->GetName()));
		if (!Graph)
			Graph = FBlueprintEditorUtils::FindEventGraph(OutBlueprint);
		if (!Graph || !OutBlueprint->UbergraphPages.Contains(Graph))
			return UnrealMcpRql::Failed<UEdGraph*>(FString::Printf(TEXT("%s is not an event graph of %s (use its EventGraph)"),
				Graph ? *Graph->GetName() : TEXT("the graph"), *OutBlueprint->GetName()));
		return Graph;
	}

	/** A node of @p Class created in @p Graph at @p X, @p Y (@p Configure runs before its pins exist), as the editor's FGraphNodeCreator does. */
	UEdGraphNode* CreateGraphNode(UEdGraph* Graph, UClass* Class, double X, double Y, TFunctionRef<void(UEdGraphNode*)> Configure)
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

	UK2Node_CallFunction* CreateCallNode(UEdGraph* Graph, UFunction* Function, double X, double Y)
	{
		return CastChecked<UK2Node_CallFunction>(CreateGraphNode(Graph, UK2Node_CallFunction::StaticClass(), X, Y,
			[Function](UEdGraphNode* Node) { CastChecked<UK2Node_CallFunction>(Node)->SetFromFunction(Function); }));
	}

	UEdGraphPin* RequirePin(UEdGraphNode* Node, const FName Name)
	{
		UEdGraphPin* Pin = Node->FindPin(Name);
		if (!Pin)
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no pin %s"), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Name.ToString()));
		return Pin;
	}

	bool Link(UEdGraphPin* A, UEdGraphPin* B)
	{
		if (!A || !B)
			return false;
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (Schema->TryCreateConnection(A, B))
			return true;
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not connect %s to %s: %s"), *A->PinName.ToString(), *B->PinName.ToString(),
			*Schema->CanCreateConnection(A, B).Message.ToString()));
		return false;
	}

	/** The Blueprint's BeginPlay event: the existing one (an editor placeholder is enabled), or a new one in @p Graph. */
	UK2Node_Event* BeginPlayEvent(UBlueprint* Blueprint, UEdGraph* Graph)
	{
		const FName Name(TEXT("ReceiveBeginPlay")); // protected: GET_FUNCTION_NAME_CHECKED cannot name it
		if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, AActor::StaticClass(), Name))
		{
			if (!Existing->IsNodeEnabled() || Existing->IsAutomaticallyPlacedGhostNode())
			{
				Existing->Modify();
				Existing->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
				Existing->NodeComment.Empty(); // the placeholder's "This node is disabled ..." comment
				Existing->bCommentBubbleVisible = false;
			}
			return Existing;
		}
		const FVector2D At = Graph->GetGoodPlaceForNewNode();
		return CastChecked<UK2Node_Event>(CreateGraphNode(Graph, UK2Node_Event::StaticClass(), At.X, At.Y, [Name](UEdGraphNode* Node)
		{
			UK2Node_Event* Event = CastChecked<UK2Node_Event>(Node);
			Event->EventReference.SetExternalMember(Name, AActor::StaticClass());
			Event->bOverrideFunction = true;
		}));
	}

	/** The Add Mapping Context call node of @p Blueprint that adds @p Context, if any. */
	UK2Node_CallFunction* FindAddMappingContext(UBlueprint* Blueprint, const UInputMappingContext* Context)
	{
		TArray<UK2Node_CallFunction*> Calls;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Calls);
		for (UK2Node_CallFunction* Call : Calls)
		{
			const UEdGraphPin* Pin = Call->FunctionReference.GetMemberName() == TEXT("AddMappingContext") ? Call->FindPin(TEXT("MappingContext")) : nullptr;
			if (Pin && Pin->DefaultObject == Context)
				return Call;
		}
		return nullptr;
	}
}

using namespace UnrealAIEnhancedInputRqlLocal;

UInputAction* UUnrealAIEnhancedInputRqlApi::CreateAction(const FString& Path, EInputActionValueType ValueType)
{
	UInputAction* Action = Cast<UInputAction>(UnrealMcpRql::CreateAsset(UInputAction::StaticClass(), Path));
	if (Action)
		Action->ValueType = ValueType;
	return Action;
}

UInputMappingContext* UUnrealAIEnhancedInputRqlApi::CreateContext(const FString& Path)
{
	return Cast<UInputMappingContext>(UnrealMcpRql::CreateAsset(UInputMappingContext::StaticClass(), Path));
}

bool UUnrealAIEnhancedInputRqlApi::AddMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key)
{
	FKey Parsed;
	if (!ValidKey(Key, Parsed))
		return false;
	Context->Modify();
	Context->MapKey(Action, Parsed);
	Context->MarkPackageDirty();
	return true;
}

int32 UUnrealAIEnhancedInputRqlApi::RemoveMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key)
{
	FKey Parsed;
	if (!ValidKey(Key, Parsed))
		return 0;
	const int32 Removed = Context->GetMappings().FilterByPredicate([Action, &Parsed](const FEnhancedActionKeyMapping& Mapping)
	{
		return Mapping.Action == Action && Mapping.Key == Parsed;
	}).Num();
	if (Removed > 0)
	{
		Context->Modify();
		// UnmapKey removes one mapping per call; its control-mapping rebuild requests are deferred and coalesce.
		for (int32 Index = 0; Index < Removed; ++Index)
			Context->UnmapKey(Action, Parsed);
		Context->MarkPackageDirty();
	}
	return Removed;
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::AddActionEvent(UObject* Graph, UInputAction* Action)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* EventGraph = EventGraphOf(Graph, Blueprint);
	if (!EventGraph)
		return nullptr;
	if (!GetDefault<UK2Node_EnhancedInputAction>()->IsCompatibleWithGraph(EventGraph))
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s cannot have input events"), *Blueprint->GetName()));
	TArray<UK2Node_EnhancedInputAction*> Existing;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Existing);
	for (const UK2Node_EnhancedInputAction* Node : Existing)
	{
		if (Node->InputAction == Action)
			return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s already has an action event for %s (one per action: connect more "
				"nodes to its pins; find it with unreal.asset(path).EventGraph | get Nodes | where Title contains \"%s\")"),
				*Blueprint->GetName(), *Action->GetName(), *Action->GetName()));
	}
	EventGraph->Modify();
	const FVector2D At = EventGraph->GetGoodPlaceForNewNode();
	UEdGraphNode* Node = CreateGraphNode(EventGraph, UK2Node_EnhancedInputAction::StaticClass(), At.X, At.Y,
		[Action](UEdGraphNode* Created) { CastChecked<UK2Node_EnhancedInputAction>(Created)->InputAction = Action; });
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return Node;
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::AddMappingContextOnBeginPlay(UObject* Blueprint, UInputMappingContext* Context, int32 Priority)
{
	UBlueprint* Owner = nullptr;
	UEdGraph* EventGraph = EventGraphOf(Blueprint, Owner);
	if (!EventGraph)
		return nullptr;
	if (FindAddMappingContext(Owner, Context))
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s already adds %s"), *Owner->GetName(), *Context->GetName()));
	UFunction* AddFunction = UEnhancedInputLocalPlayerSubsystem::StaticClass()->FindFunctionByName(TEXT("AddMappingContext"));
	UFunction* PlayerController = UGameplayStatics::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetPlayerController));
	// The "Enhanced Input Local Player Subsystem" getter node: its class is not exported, so it is found by path.
	UClass* SubsystemNode = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintGraph.K2Node_GetSubsystemFromPC"));
	if (!AddFunction || !PlayerController || !SubsystemNode)
		return UnrealMcpRql::Failed<UEdGraphNode*>(TEXT("this engine version lacks a node Add Mapping Context needs"));

	UK2Node_Event* Begin = BeginPlayEvent(Owner, EventGraph);
	UEdGraph* Graph = Begin->GetGraph();
	Graph->Modify();
	const double X = Begin->NodePosX;
	const double Y = Begin->NodePosY;
	UK2Node_CallFunction* GetController = CreateCallNode(Graph, PlayerController, X + 40, Y + 160);
	UEdGraphNode* GetSubsystem = CreateGraphNode(Graph, SubsystemNode, X + 300, Y + 160, [](UEdGraphNode* Node)
	{
		CastChecked<UK2Node_GetSubsystem>(Node)->Initialize(UEnhancedInputLocalPlayerSubsystem::StaticClass());
	});
	UK2Node_CallFunction* Add = CreateCallNode(Graph, AddFunction, X + 620, Y);

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* ContextPin = RequirePin(Add, TEXT("MappingContext"));
	UEdGraphPin* PriorityPin = RequirePin(Add, TEXT("Priority"));
	if (!ContextPin || !PriorityPin)
		return nullptr;
	Schema->TrySetDefaultObject(*ContextPin, Context);
	Schema->TrySetDefaultValue(*PriorityPin, FString::FromInt(Priority));
	if (!Link(RequirePin(GetController, UEdGraphSchema_K2::PN_ReturnValue), RequirePin(GetSubsystem, TEXT("PlayerController")))
		|| !Link(RequirePin(GetSubsystem, UEdGraphSchema_K2::PN_ReturnValue), RequirePin(Add, UEdGraphSchema_K2::PN_Self)))
		return nullptr;

	// Run first: what BeginPlay ran before now follows Add Mapping Context.
	UEdGraphPin* Then = RequirePin(Begin, UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* AddExec = RequirePin(Add, UEdGraphSchema_K2::PN_Execute);
	UEdGraphPin* AddThen = RequirePin(Add, UEdGraphSchema_K2::PN_Then);
	if (!Then || !AddExec || !AddThen)
		return nullptr;
	const TArray<UEdGraphPin*> Before = Then->LinkedTo;
	Begin->Modify();
	Schema->BreakPinLinks(*Then, /*bSendsNodeNotifcation*/ true);
	if (!Link(Then, AddExec))
		return nullptr;
	for (UEdGraphPin* Next : Before)
	{
		if (!Link(AddThen, Next))
			return nullptr;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Owner);
	return Add;
}
