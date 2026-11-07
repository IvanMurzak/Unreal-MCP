// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIEnhancedInputRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "K2Node_GetSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

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

	/** The concrete InputModifier class a name means ("Negate", "InputModifierNegate", "Swizzle"); null when none. */
	UClass* ModifierClass(const FString& Name)
	{
		const FString Short = Name.Equals(TEXT("Swizzle"), ESearchCase::IgnoreCase) ? FString(TEXT("SwizzleAxis")) : Name;
		for (const FString& Candidate : { Short, TEXT("InputModifier") + Short })
		{
			UClass* Class = UnrealMcpRql::ResolveClass(Candidate);
			if (Class && Class->IsChildOf(UInputModifier::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				return Class;
		}
		return nullptr;
	}

	/** Every concrete InputModifier class name without its prefix, for the error that lists them. */
	FString ModifierNames()
	{
		TArray<FString> Names;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UInputModifier::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated) && It->IsNative())
				Names.Add(UUnrealAIEnhancedInputRqlApi::ModifierDisplayName(*It));
		}
		Names.Sort();
		return FString::Join(Names, TEXT(", "));
	}

	/** The modifier classes of a comma-separated list, in order; false after Fail (an unknown name). */
	bool ParseModifiers(const FString& Text, TArray<UClass*>& Out)
	{
		TArray<FString> Names;
		Text.ParseIntoArray(Names, TEXT(","), /*InCullEmpty*/ true);
		for (FString& Name : Names)
		{
			Name.TrimStartAndEndInline();
			if (Name.IsEmpty())
				continue;
			UClass* Class = ModifierClass(Name);
			if (!Class)
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not an input modifier (modifiers: %s, or a Blueprint modifier class path)"), *Name, *ModifierNames()));
				return false;
			}
			Out.Add(Class);
		}
		return true;
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

	/** The Blueprint's existing BeginPlay event (an editor placeholder included), or null. */
	UK2Node_Event* FindBeginPlay(UBlueprint* Blueprint)
	{
		return FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, AActor::StaticClass(), TEXT("ReceiveBeginPlay"));
	}

	/**
	 * A BeginPlay that starts overriding now (new, or the editor's placeholder) would silently drop a parent Blueprint's
	 * BeginPlay: when the parent implements it, Parent: BeginPlay runs first. Returns the exec pin what runs next connects to.
	 */
	UEdGraphPin* CallParentBeginPlay(UBlueprint* Blueprint, UK2Node_Event* Begin, UEdGraphPin* Then)
	{
		UFunction* Inherited = Blueprint->ParentClass ? Blueprint->ParentClass->FindFunctionByName(TEXT("ReceiveBeginPlay")) : nullptr;
		if (!Then || !Inherited || !Cast<UBlueprintGeneratedClass>(Inherited->GetOuterUClass()))
			return Then;
		UK2Node_CallFunction* Parent = CastChecked<UK2Node_CallFunction>(CreateGraphNode(Begin->GetGraph(), UK2Node_CallParentFunction::StaticClass(),
			Begin->NodePosX + 250, Begin->NodePosY, [Inherited](UEdGraphNode* Node) { CastChecked<UK2Node_CallFunction>(Node)->SetFromFunction(Inherited); }));
		UEdGraphPin* ParentExec = Parent->FindPin(UEdGraphSchema_K2::PN_Execute);
		return ParentExec && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Then, ParentExec) ? Parent->FindPin(UEdGraphSchema_K2::PN_Then) : Then;
	}

	/** A new BeginPlay event in @p Graph; when a parent Blueprint implements BeginPlay, followed by its Parent: BeginPlay call. Returns the exec pin what runs next connects to. */
	UEdGraphPin* AddBeginPlay(UBlueprint* Blueprint, UEdGraph* Graph, UK2Node_Event*& OutEvent)
	{
		const FName Name(TEXT("ReceiveBeginPlay")); // protected: GET_FUNCTION_NAME_CHECKED cannot name it
		Graph->Modify();
		const FVector2D At = Graph->GetGoodPlaceForNewNode();
		OutEvent = CastChecked<UK2Node_Event>(CreateGraphNode(Graph, UK2Node_Event::StaticClass(), At.X, At.Y, [Name](UEdGraphNode* Node)
		{
			UK2Node_Event* Event = CastChecked<UK2Node_Event>(Node);
			Event->EventReference.SetExternalMember(Name, AActor::StaticClass());
			Event->bOverrideFunction = true;
		}));
		return CallParentBeginPlay(Blueprint, OutEvent, OutEvent->FindPin(UEdGraphSchema_K2::PN_Then));
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

FString UUnrealAIEnhancedInputRqlApi::ModifierDisplayName(const UClass* Class)
{
	FString Name = Class ? Class->GetName() : FString();
	Name.RemoveFromStart(TEXT("InputModifier"));
	return Name;
}

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

bool UUnrealAIEnhancedInputRqlApi::AddMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Modifiers)
{
	FKey Parsed;
	TArray<UClass*> ModifierClasses;
	if (!ValidKey(Key, Parsed) || !ParseModifiers(Modifiers, ModifierClasses))
		return false;
	Context->Modify();
	FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Parsed);
	// Instanced into the context, as the editor's Modifiers array creates them.
	for (UClass* Class : ModifierClasses)
		Mapping.Modifiers.Add(NewObject<UInputModifier>(Context, Class, NAME_None, Context->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional));
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
	UK2Node_Event* Begin = FindBeginPlay(Owner);
	if (Begin && !Begin->IsNodeEnabled() && !Begin->IsAutomaticallyPlacedGhostNode())
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s's BeginPlay is disabled: enable it first"), *Owner->GetName()));

	// The new nodes, wired among themselves (a failure here leaves only them, unconnected to the Blueprint's logic).
	UEdGraph* Graph = Begin ? Begin->GetGraph() : EventGraph;
	Graph->Modify();
	const FVector2D Origin = Begin ? FVector2D(Begin->NodePosX, Begin->NodePosY) : FVector2D(Graph->GetGoodPlaceForNewNode());
	const double X = Origin.X;
	const double Y = Origin.Y;
	UK2Node_CallFunction* GetController = CreateCallNode(Graph, PlayerController, X + 40, Y + 160);
	UEdGraphNode* GetSubsystem = CreateGraphNode(Graph, SubsystemNode, X + 300, Y + 160, [](UEdGraphNode* Node)
	{
		CastChecked<UK2Node_GetSubsystem>(Node)->Initialize(UEnhancedInputLocalPlayerSubsystem::StaticClass());
	});
	UK2Node_CallFunction* Add = CreateCallNode(Graph, AddFunction, X + 620, Y);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* ContextPin = RequirePin(Add, TEXT("MappingContext"));
	UEdGraphPin* PriorityPin = RequirePin(Add, TEXT("Priority"));
	UEdGraphPin* AddExec = RequirePin(Add, UEdGraphSchema_K2::PN_Execute);
	UEdGraphPin* AddThen = RequirePin(Add, UEdGraphSchema_K2::PN_Then);
	if (!ContextPin || !PriorityPin || !AddExec || !AddThen)
		return nullptr;
	Schema->TrySetDefaultObject(*ContextPin, Context);
	Schema->TrySetDefaultValue(*PriorityPin, FString::FromInt(Priority));
	if (!Link(RequirePin(GetController, UEdGraphSchema_K2::PN_ReturnValue), RequirePin(GetSubsystem, TEXT("PlayerController")))
		|| !Link(RequirePin(GetSubsystem, UEdGraphSchema_K2::PN_ReturnValue), RequirePin(Add, UEdGraphSchema_K2::PN_Self)))
		return nullptr;

	// Then BeginPlay, checked before it changes: what it ran before now follows Add Mapping Context.
	UEdGraphPin* Then = nullptr;
	TArray<UEdGraphPin*> Before;
	if (Begin)
	{
		Then = RequirePin(Begin, UEdGraphSchema_K2::PN_Then);
		if (!Then)
			return nullptr;
		Before = Then->LinkedTo;
		for (UEdGraphPin* Next : Before)
		{
			if (Schema->CanCreateConnection(AddThen, Next).Response == CONNECT_RESPONSE_DISALLOW)
				return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("cannot move %s after Add Mapping Context"), *Next->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString()));
		}
		Begin->Modify();
		Schema->BreakPinLinks(*Then, /*bSendsNodeNotifcation*/ true);
		if (!Begin->IsNodeEnabled() || Begin->IsAutomaticallyPlacedGhostNode())
		{
			Begin->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
			Begin->NodeComment.Empty(); // the placeholder's "This node is disabled ..." comment
			Begin->bCommentBubbleVisible = false;
			Then = CallParentBeginPlay(Owner, Begin, Then); // the placeholder starts overriding now
		}
	}
	else
	{
		Then = AddBeginPlay(Owner, Graph, Begin);
	}
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

namespace UnrealAIEnhancedInputRqlLocal
{
	/** The concrete InputTrigger class a name means ("Pressed", "InputTriggerHold"); null when none. */
	UClass* TriggerClass(const FString& Name)
	{
		for (const FString& Candidate : { Name, TEXT("InputTrigger") + Name })
		{
			UClass* Class = UnrealMcpRql::ResolveClass(Candidate);
			if (Class && Class->IsChildOf(UInputTrigger::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				return Class;
		}
		return nullptr;
	}

	const TCHAR* TriggerEventName(ETriggerEvent Event)
	{
		switch (Event)
		{
			case ETriggerEvent::Triggered: return TEXT("Triggered");
			case ETriggerEvent::Started: return TEXT("Started");
			case ETriggerEvent::Ongoing: return TEXT("Ongoing");
			case ETriggerEvent::Canceled: return TEXT("Canceled");
			case ETriggerEvent::Completed: return TEXT("Completed");
			default: return TEXT("None");
		}
	}

	/** The trigger event of each frame, as runs: "Triggered x30, Completed x1" (frames without an event are left out). */
	class FTriggerEventLog
	{
	public:
		void Add(ETriggerEvent Event)
		{
			if (Event == ETriggerEvent::None)
			{
				Last = ETriggerEvent::None;
				return;
			}
			if (Runs.Num() > 0 && Event == Last)
				++Runs.Last().Value;
			else
				Runs.Add({ Event, 1 });
			Last = Event;
		}

		FString ToString() const
		{
			TArray<FString> Parts;
			for (const TPair<ETriggerEvent, int32>& Run : Runs)
				Parts.Add(FString::Printf(TEXT("%s x%d"), TriggerEventName(Run.Key), Run.Value));
			return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("no trigger event"));
		}

	private:
		TArray<TPair<ETriggerEvent, int32>> Runs;
		ETriggerEvent Last = ETriggerEvent::None;
	};
}

TArray<FString> UUnrealAIEnhancedInputRqlApi::AppliedContexts(const APlayerController* Controller)
{
	TArray<TPair<int32, FString>> Applied;
	// The applied-context map of the player input is not public: ask the player's subsystem about each loaded context (a
	// context applied to a player is loaded).
	const ULocalPlayer* Player = Controller ? Controller->GetLocalPlayer() : nullptr;
	if (const UEnhancedInputLocalPlayerSubsystem* Input = Player ? Player->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr)
	{
		for (TObjectIterator<UInputMappingContext> It; It; ++It)
		{
			int32 Priority = 0;
			if (!It->HasAnyFlags(RF_ClassDefaultObject) && Input->HasMappingContext(*It, Priority))
				Applied.Add({ Priority, It->GetName() });
		}
	}
	Applied.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key != B.Key ? A.Key > B.Key : A.Value < B.Value; });
	TArray<FString> Names;
	for (const TPair<int32, FString>& Entry : Applied)
		Names.Add(FString::Printf(TEXT("%s (priority %d)"), *Entry.Value, Entry.Key));
	return Names;
}

FUnrealMcpPlayState UUnrealAIEnhancedInputRqlApi::InjectInput(UInputAction* Action, FVector Value, double Seconds, int32 Frames, const FString& Triggers)
{
	using namespace UnrealAIEnhancedInputRqlLocal;
	using UnrealMcpRql::Failed;
	FString Error;
	const int32 Count = UnrealMcpRql::PlayFrameCount(Seconds, Frames, Error);
	if (Count < 0)
		return Failed<FUnrealMcpPlayState>(Error);
	APlayerController* Controller = UnrealMcpRql::PlayController();
	if (!Controller)
		return Failed<FUnrealMcpPlayState>(UnrealMcpRql::PlayWorld() ? TEXT("the play world has no player controller to inject input into")
			: TEXT("no Play In Editor session is running: editor | call StartPlay() first"));
	if (!Cast<UEnhancedPlayerInput>(Controller->PlayerInput))
		return Failed<FUnrealMcpPlayState>(TEXT("player 0 does not use Enhanced Input (Project Settings > Input: Default Player Input Class = EnhancedPlayerInput)"));

	// Extra triggers, as a key mapping's: transient objects held by a GC guard for the call.
	TArray<UInputTrigger*> TriggerObjects;
	TArray<FString> Names;
	Triggers.ParseIntoArray(Names, TEXT(","), /*bCullEmpty*/ true);
	for (const FString& Raw : Names)
	{
		const FString Name = Raw.TrimStartAndEnd();
		if (Name.IsEmpty())
			continue;
		UClass* Class = TriggerClass(Name);
		if (!Class)
			return Failed<FUnrealMcpPlayState>(FString::Printf(TEXT("'%s' is not an input trigger (Pressed, Released, Down, Hold, HoldAndRelease, Tap, Pulse)"), *Name));
		TriggerObjects.Add(NewObject<UInputTrigger>(GetTransientPackage(), Class));
	}
	TArray<TStrongObjectPtr<UInputTrigger>> Keep;
	for (UInputTrigger* Trigger : TriggerObjects)
		Keep.Emplace(Trigger);

	const FInputActionValue RawValue(Action->ValueType, Value);
	FTriggerEventLog Events;
	FString LastValue;
	auto Sample = [Action, &Events, &LastValue](const APlayerController* From, bool bHeldFrame)
	{
		const UEnhancedPlayerInput* Input = From ? Cast<UEnhancedPlayerInput>(From->PlayerInput) : nullptr;
		const FInputActionInstance* Instance = Input ? Input->FindActionInstanceData(Action) : nullptr;
		Events.Add(Instance ? Instance->GetTriggerEvent() : ETriggerEvent::None);
		if (Instance && bHeldFrame)
			LastValue = Instance->GetValue().ToString();
	};
	int32 Advanced = 0;
	// The input of frame N is evaluated in frame N's tick: the frame's event is read before the next frame's injection.
	const bool bHeld = UnrealMcpRql::AdvancePlay(Count, [&](APlayerController* Target, int32 Frame)
	{
		if (Frame > 0)
			Sample(Target, true);
		if (UEnhancedPlayerInput* Input = Target ? Cast<UEnhancedPlayerInput>(Target->PlayerInput) : nullptr)
			Input->InjectInputForAction(Action, RawValue, {}, TriggerObjects);
	}, Advanced, Error);
	if (!bHeld)
		return Failed<FUnrealMcpPlayState>(Error);
	// One frame without input: the action completes (Completed / Canceled fire) as when the key is let go.
	int32 Released = 0;
	const bool bReleased = UnrealMcpRql::AdvancePlay(1, [&](APlayerController* Target, int32) { Sample(Target, true); }, Released, Error);
	if (!bReleased)
		return Failed<FUnrealMcpPlayState>(Error);
	Sample(UnrealMcpRql::PlayController(), false);

	FUnrealMcpPlayState State = UnrealMcpRql::ReadPlayState();
	State.FramesAdvanced = Advanced + Released;
	State.Notes.Add(FString::Printf(TEXT("%s: %s (%d frame(s) held + 1 released)"), *Action->GetName(), *Events.ToString(), Advanced));
	if (!LastValue.IsEmpty())
		State.Notes.Add(FString::Printf(TEXT("%s value while held: %s"), *Action->GetName(), *LastValue));
	return State;
}
