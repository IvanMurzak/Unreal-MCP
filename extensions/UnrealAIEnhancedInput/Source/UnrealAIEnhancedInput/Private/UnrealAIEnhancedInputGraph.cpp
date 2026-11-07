// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIEnhancedInputGraph.h"

#include "IUnrealMcpRqlModule.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Self.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace UnrealAIEnhancedInputGraphLocal
{
	const FName AddFunctionName(TEXT("AddMappingContext"));
	const FName RemoveFunctionName(TEXT("RemoveMappingContext"));
	const FName ContextPinName(TEXT("MappingContext"));
	const FName ActionValuePinName(TEXT("ActionValue"));
	const TCHAR* TriggerPins[] = { TEXT("Started"), TEXT("Triggered"), TEXT("Ongoing"), TEXT("Canceled"), TEXT("Completed") };

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

	FString Title(const UEdGraphNode* Node)
	{
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			return Call->FunctionReference.GetMemberName().ToString();
		if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
			return Macro->GetMacroGraph() ? Macro->GetMacroGraph()->GetName() : FString(TEXT("Macro"));
		return Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}

	UEdGraphPin* RequirePin(UEdGraphNode* Node, const FName Name)
	{
		UEdGraphPin* Pin = Node->FindPin(Name);
		if (!Pin)
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no pin %s"), *Title(Node), *Name.ToString()));
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

	/** The exec output a K2 node continues with ("then"); null for nodes without one (branches, macros). */
	UEdGraphPin* ThenPin(UEdGraphNode* Node)
	{
		UEdGraphPin* Pin = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? Pin : nullptr;
	}

	UEdGraphPin* ExecInput(UEdGraphNode* Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				return Pin;
		}
		return nullptr;
	}

	/** An event as agents name it: "BeginPlay" for ReceiveBeginPlay, the custom event's name. */
	FString EventName(const UK2Node_Event* Event)
	{
		if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Event))
			return Custom->CustomFunctionName.ToString();
		FString Name = Event->EventReference.GetMemberName().ToString();
		if (!Name.RemoveFromStart(TEXT("Receive")))
			Name.RemoveFromStart(TEXT("K2_"));
		if (Name == TEXT("Possess"))
			return TEXT("OnPossess");
		if (Name == TEXT("UnPossess"))
			return TEXT("OnUnpossess");
		return Name;
	}

	/** The event whose exec chain runs @p Node (walking exec inputs back), or null. */
	const UK2Node_Event* OwningEvent(const UEdGraphNode* Node)
	{
		TArray<const UEdGraphNode*> Queue = { Node };
		TSet<const UEdGraphNode*> Seen;
		while (Queue.Num() > 0 && Seen.Num() < 256)
		{
			const UEdGraphNode* Current = Queue.Pop(EAllowShrinking::No);
			if (Seen.Contains(Current))
				continue;
			Seen.Add(Current);
			if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Current))
				return Event;
			for (const UEdGraphPin* Pin : Current->Pins)
			{
				if (Pin->Direction != EGPD_Input || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					continue;
				for (const UEdGraphPin* From : Pin->LinkedTo)
					Queue.Add(From->GetOwningNode());
			}
		}
		return nullptr;
	}

	/** The override event @p Name means for @p Blueprint (BeginPlay, Possessed, OnPossess, ...); null when it is a custom event name. */
	UFunction* OverrideEvent(UBlueprint* Blueprint, const FString& Name, FString& OutError)
	{
		struct FKnown { const TCHAR* Name; UClass* Owner; const TCHAR* Function; };
		const FKnown Known[] = {
			{ TEXT("BeginPlay"), AActor::StaticClass(), TEXT("ReceiveBeginPlay") },
			{ TEXT("Possessed"), APawn::StaticClass(), TEXT("ReceivePossessed") },
			{ TEXT("Unpossessed"), APawn::StaticClass(), TEXT("ReceiveUnpossessed") },
			{ TEXT("ControllerChanged"), APawn::StaticClass(), TEXT("ReceiveControllerChanged") },
			{ TEXT("OnPossess"), AController::StaticClass(), TEXT("ReceivePossess") },
			{ TEXT("Possess"), AController::StaticClass(), TEXT("ReceivePossess") },
			{ TEXT("OnUnpossess"), AController::StaticClass(), TEXT("ReceiveUnPossess") },
			{ TEXT("Unpossess"), AController::StaticClass(), TEXT("ReceiveUnPossess") },
		};
		UClass* Parent = Blueprint->ParentClass;
		for (const FKnown& Entry : Known)
		{
			if (!Name.Equals(Entry.Name, ESearchCase::IgnoreCase))
				continue;
			if (!Parent->IsChildOf(Entry.Owner))
			{
				OutError = FString::Printf(TEXT("%s is a %s event and %s is a %s (%s)"), Entry.Name, *Entry.Owner->GetName(), *Blueprint->GetName(),
					*Parent->GetName(), Parent->IsChildOf(AController::StaticClass()) ? TEXT("controllers: BeginPlay, OnPossess, OnUnpossess")
					: Parent->IsChildOf(APawn::StaticClass()) ? TEXT("pawns: BeginPlay, Possessed, Unpossessed, ControllerChanged")
					: TEXT("use BeginPlay or a custom event"));
				return nullptr;
			}
			return Parent->FindFunctionByName(Entry.Function);
		}
		for (const FString& Candidate : { Name, FString(TEXT("Receive")) + Name, FString(TEXT("K2_")) + Name })
		{
			UFunction* Found = Parent->FindFunctionByName(FName(*Candidate, FNAME_Find));
			if (Found && UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Found))
				return Found;
		}
		return nullptr;
	}

	/**
	 * A parent Blueprint's implementation of @p Event would be silently dropped by an override that starts now (new, or the
	 * editor's placeholder): when the parent implements it, Parent: <Event> runs first. Returns the exec pin what runs next connects to.
	 */
	UEdGraphPin* CallParentEvent(UBlueprint* Blueprint, UK2Node_Event* Event, UEdGraphPin* Then)
	{
		UFunction* Inherited = Blueprint->ParentClass ? Blueprint->ParentClass->FindFunctionByName(Event->EventReference.GetMemberName()) : nullptr;
		if (!Then || !Inherited || !Cast<UBlueprintGeneratedClass>(Inherited->GetOuterUClass()))
			return Then;
		UK2Node_CallFunction* Parent = CastChecked<UK2Node_CallFunction>(CreateGraphNode(Event->GetGraph(), UK2Node_CallParentFunction::StaticClass(),
			Event->NodePosX + 250, Event->NodePosY, [Inherited](UEdGraphNode* Node) { CastChecked<UK2Node_CallFunction>(Node)->SetFromFunction(Inherited); }));
		UEdGraphPin* ParentExec = Parent->FindPin(UEdGraphSchema_K2::PN_Execute);
		return ParentExec && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Then, ParentExec) ? Parent->FindPin(UEdGraphSchema_K2::PN_Then) : Then;
	}

	/** Where an event's steps start: the existing event (or the one to create) and its "then" pin. */
	struct FEventSite
	{
		UK2Node_Event* Event = nullptr;
		UFunction* Override = nullptr; // null: a custom event
		FString CustomName;
	};

	/** Find the event @p Name names in @p Blueprint without changing anything; false after Fail. */
	bool FindEvent(UBlueprint* Blueprint, const FString& Name, FEventSite& Out)
	{
		FString Error;
		Out.Override = OverrideEvent(Blueprint, Name, Error);
		if (!Error.IsEmpty())
		{
			UnrealMcpRql::Fail(Error);
			return false;
		}
		if (Out.Override)
		{
			Out.Event = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, Out.Override->GetOwnerClass(), Out.Override->GetFName());
			if (Out.Event && !Out.Event->IsNodeEnabled() && !Out.Event->IsAutomaticallyPlacedGhostNode())
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("%s's %s is disabled: enable it first"), *Blueprint->GetName(), *Name));
				return false;
			}
			return true;
		}
		if (Name.IsEmpty() || !FName(*Name).IsValidXName(INVALID_OBJECTNAME_CHARACTERS))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not an event name (BeginPlay, Possessed, Unpossessed, ControllerChanged, OnPossess, OnUnpossess, or a custom event)"), *Name));
			return false;
		}
		Out.CustomName = Name;
		Out.Event = FBlueprintEditorUtils::FindCustomEventNode(Blueprint, FName(*Name));
		if (!Out.Event && FindObject<UObject>(Blueprint, *Name))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s already has a function or variable named '%s': name a custom event (or one of its events)"), *Blueprint->GetName(), *Name));
			return false;
		}
		return true;
	}

	/** The event of @p Site, created when it does not exist yet (enabled when it is the editor's placeholder); returns its "then" pin. */
	UEdGraphPin* MaterializeEvent(UBlueprint* Blueprint, UEdGraph* Graph, FEventSite& Site)
	{
		if (Site.Event)
		{
			UEdGraphPin* Then = RequirePin(Site.Event, UEdGraphSchema_K2::PN_Then);
			if (Then && (!Site.Event->IsNodeEnabled() || Site.Event->IsAutomaticallyPlacedGhostNode()))
			{
				Site.Event->Modify();
				Site.Event->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
				Site.Event->NodeComment.Empty(); // the placeholder's "This node is disabled ..." comment
				Site.Event->bCommentBubbleVisible = false;
				// The placeholder starts overriding now: keep the parent's implementation (Then is unlinked: a placeholder runs nothing).
				return CallParentEvent(Blueprint, Site.Event, Then);
			}
			return Then;
		}
		const FVector2D At = Graph->GetGoodPlaceForNewNode();
		if (Site.Override)
		{
			UClass* Owner = Site.Override->GetOwnerClass();
			const FName Function = Site.Override->GetFName();
			Site.Event = CastChecked<UK2Node_Event>(CreateGraphNode(Graph, UK2Node_Event::StaticClass(), At.X, At.Y, [Owner, Function](UEdGraphNode* Node)
			{
				UK2Node_Event* Event = CastChecked<UK2Node_Event>(Node);
				Event->EventReference.SetExternalMember(Function, Owner);
				Event->bOverrideFunction = true;
			}));
			return CallParentEvent(Blueprint, Site.Event, Site.Event->FindPin(UEdGraphSchema_K2::PN_Then));
		}
		const FName Custom(*Site.CustomName);
		Site.Event = CastChecked<UK2Node_Event>(CreateGraphNode(Graph, UK2Node_CustomEvent::StaticClass(), At.X, At.Y,
			[Custom](UEdGraphNode* Node) { CastChecked<UK2Node_CustomEvent>(Node)->CustomFunctionName = Custom; }));
		return Site.Event->FindPin(UEdGraphSchema_K2::PN_Then);
	}

	/** What runs the steps and how far they have got: the exec outputs the next node continues from, and the place for it. */
	struct FChainCursor
	{
		UEdGraph* Graph = nullptr;
		TArray<UEdGraphPin*> Tails;
		double X = 0;
		double Y = 0;
	};

	/** Connect every tail to @p Exec; the cursor moves right. */
	bool Continue(FChainCursor& Cursor, UEdGraphPin* Exec)
	{
		for (UEdGraphPin* Tail : Cursor.Tails)
		{
			if (!Link(Tail, Exec))
				return false;
		}
		Cursor.Tails.Reset();
		Cursor.X += 320;
		return true;
	}

	/**
	 * The player controller pin a step reads (a Cast To PlayerController continues the chain when the controller has to be
	 * cast: its failure skips the step). Null after Fail.
	 */
	UEdGraphPin* PlayerControllerPin(UBlueprint* Blueprint, UK2Node_Event* Event, bool bBeginPlay, FName Parameter, FChainCursor& Cursor, UEdGraphPin*& OutSkip)
	{
		UClass* Parent = Blueprint->ParentClass;
		UEdGraphPin* Controller = nullptr;
		if (!Parameter.IsNone())
		{
			Controller = RequirePin(Event, Parameter);
			if (!Controller)
				return nullptr;
		}
		else if (Parent->IsChildOf(APlayerController::StaticClass()))
		{
			UEdGraphNode* Self = CreateGraphNode(Cursor.Graph, UK2Node_Self::StaticClass(), Cursor.X, Cursor.Y + 180, [](UEdGraphNode*) {});
			return RequirePin(Self, UEdGraphSchema_K2::PN_Self);
		}
		else if (Parent->IsChildOf(APawn::StaticClass()) && !bBeginPlay)
		{
			UFunction* GetController = APawn::StaticClass()->FindFunctionByName(TEXT("GetController"));
			Controller = GetController ? RequirePin(CreateCallNode(Cursor.Graph, GetController, Cursor.X - 40, Cursor.Y + 180), UEdGraphSchema_K2::PN_ReturnValue) : nullptr;
			if (!Controller)
				return UnrealMcpRql::Failed<UEdGraphPin*>(TEXT("this engine version lacks APawn::GetController"));
		}
		else
		{
			UFunction* GetPlayer = UGameplayStatics::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetPlayerController));
			return GetPlayer ? RequirePin(CreateCallNode(Cursor.Graph, GetPlayer, Cursor.X - 40, Cursor.Y + 180), UEdGraphSchema_K2::PN_ReturnValue) : nullptr;
		}
		UK2Node_DynamicCast* Cast = CastChecked<UK2Node_DynamicCast>(CreateGraphNode(Cursor.Graph, UK2Node_DynamicCast::StaticClass(), Cursor.X, Cursor.Y,
			[](UEdGraphNode* Node) { CastChecked<UK2Node_DynamicCast>(Node)->TargetType = APlayerController::StaticClass(); }));
		if (!Link(Controller, Cast->GetCastSourcePin()) || !Continue(Cursor, RequirePin(Cast, UEdGraphSchema_K2::PN_Execute)))
			return nullptr;
		Cursor.Tails.Add(Cast->GetValidCastPin());
		OutSkip = Cast->GetInvalidCastPin();
		return Cast->GetCastResultPin();
	}

	/** One Add / Remove Mapping Context call after the cursor's tails; null after Fail. */
	UEdGraphNode* AddStep(UBlueprint* Blueprint, UK2Node_Event* Event, bool bBeginPlay, const UnrealAIEnhancedInputGraph::FContextStep& Step, FChainCursor& Cursor)
	{
		UFunction* Function = UEnhancedInputLocalPlayerSubsystem::StaticClass()->FindFunctionByName(Step.bAdd ? AddFunctionName : RemoveFunctionName);
		// The "Enhanced Input Local Player Subsystem" getter node: its class is not exported, so it is found by path.
		UClass* SubsystemNode = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintGraph.K2Node_GetSubsystemFromPC"));
		if (!Function || !SubsystemNode)
			return UnrealMcpRql::Failed<UEdGraphNode*>(TEXT("this engine version lacks a node Add / Remove Mapping Context needs"));
		UEdGraphPin* Skip = nullptr;
		UEdGraphPin* Controller = PlayerControllerPin(Blueprint, Event, bBeginPlay, Step.ControllerParameter, Cursor, Skip);
		if (!Controller)
			return nullptr;
		UEdGraphNode* Subsystem = CreateGraphNode(Cursor.Graph, SubsystemNode, Cursor.X, Cursor.Y + 180, [](UEdGraphNode* Node)
		{
			CastChecked<UK2Node_GetSubsystem>(Node)->Initialize(UEnhancedInputLocalPlayerSubsystem::StaticClass());
		});
		UK2Node_CallFunction* Call = CreateCallNode(Cursor.Graph, Function, Cursor.X + 280, Cursor.Y);
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		UEdGraphPin* ContextPin = RequirePin(Call, ContextPinName);
		UEdGraphPin* Exec = RequirePin(Call, UEdGraphSchema_K2::PN_Execute);
		UEdGraphPin* Then = RequirePin(Call, UEdGraphSchema_K2::PN_Then);
		if (!ContextPin || !Exec || !Then)
			return nullptr;
		Schema->TrySetDefaultObject(*ContextPin, Step.Context);
		if (Step.bAdd)
		{
			if (UEdGraphPin* Priority = RequirePin(Call, TEXT("Priority")))
				Schema->TrySetDefaultValue(*Priority, FString::FromInt(Step.Priority));
			// Registers the context's player-mappable keys with the Enhanced Input user settings (key rebinding); the
			// removal keeps them registered, so the rebinding menu still lists a scheme that is not active.
			if (UEdGraphPin* Options = Call->FindPin(TEXT("Options")))
				Schema->TrySetDefaultValue(*Options, TEXT("(bIgnoreAllPressedKeysUntilRelease=True,bForceImmediately=False,bNotifyUserSettings=True)"));
		}
		if (!Link(Controller, RequirePin(Subsystem, TEXT("PlayerController"))) || !Link(RequirePin(Subsystem, UEdGraphSchema_K2::PN_ReturnValue), RequirePin(Call, UEdGraphSchema_K2::PN_Self))
			|| !Continue(Cursor, Exec))
			return nullptr;
		Cursor.Tails.Add(Then);
		if (Skip)
			Cursor.Tails.Add(Skip);
		Cursor.X += 280;
		return Call;
	}

	/** Every callable function of @p Blueprint an error lists: its functions and custom events, then inherited pawn / character ones. */
	FString CallableNames(UBlueprint* Blueprint)
	{
		TArray<FString> Own;
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() != UEdGraphSchema_K2::FN_UserConstructionScript)
				Own.Add(Graph->GetName());
		}
		TArray<UK2Node_CustomEvent*> Events;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
		for (const UK2Node_CustomEvent* Event : Events)
			Own.Add(Event->CustomFunctionName.ToString());
		// The usual input targets first, then the rest of what the native parents up to the actor offer.
		TArray<FString> Inherited;
		for (const TCHAR* Common : { TEXT("Jump"), TEXT("StopJumping"), TEXT("Crouch"), TEXT("UnCrouch"), TEXT("AddMovementInput"), TEXT("AddControllerYawInput"),
			TEXT("AddControllerPitchInput") })
		{
			if (Blueprint->ParentClass->FindFunctionByName(Common))
				Inherited.Add(Common);
		}
		for (const UClass* Class = Blueprint->ParentClass; Class && Class != AActor::StaticClass() && Inherited.Num() < 30; Class = Class->GetSuperClass())
		{
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It && Inherited.Num() < 30; ++It)
			{
				if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable) && !It->HasAnyFunctionFlags(FUNC_BlueprintPure | FUNC_Static) && !It->HasMetaData(TEXT("DeprecatedFunction")))
					Inherited.AddUnique(It->GetName());
			}
		}
		return FString::Printf(TEXT("its functions and events: %s; inherited: %s"), Own.Num() > 0 ? *FString::Join(Own, TEXT(", ")) : TEXT("none"),
			Inherited.Num() > 0 ? *FString::Join(Inherited, TEXT(", ")) : TEXT("none"));
	}

	/** The function @p Name means for a call from @p Blueprint's event graph; null after Fail. */
	UFunction* CallableFunction(UBlueprint* Blueprint, const FString& Name)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		UClass* Self = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
		UFunction* Function = nullptr;
		FString ClassName, FunctionName;
		if (Trimmed.Split(TEXT("."), &ClassName, &FunctionName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			UClass* Class = UnrealMcpRql::ResolveClass(ClassName);
			Function = Class ? Class->FindFunctionByName(FName(*FunctionName)) : nullptr;
		}
		else if (Self)
		{
			Function = Self->FindFunctionByName(FName(*Trimmed));
			if (!Function && FBlueprintEditorUtils::FindCustomEventNode(Blueprint, FName(*Trimmed)))
			{
				// A custom event added since the last skeleton update.
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Self = Blueprint->SkeletonGeneratedClass;
				Function = Self ? Self->FindFunctionByName(FName(*Trimmed)) : nullptr;
			}
			if (!Function)
				Function = Self ? Self->FindFunctionByName(FName(*(TEXT("K2_") + Trimmed))) : nullptr;
		}
		if (!Function || !UEdGraphSchema_K2::CanUserKismetCallFunction(Function))
			return UnrealMcpRql::Failed<UFunction*>(FString::Printf(TEXT("'%s' is not a function %s can call (%s; or \"Class.Function\" for a library function)"),
				*Trimmed, *Blueprint->GetName(), *CallableNames(Blueprint)));
		if (!Function->HasAnyFunctionFlags(FUNC_Static) && Self && !Self->IsChildOf(Function->GetOwnerClass()))
			return UnrealMcpRql::Failed<UFunction*>(FString::Printf(TEXT("%s is a function of %s, not of %s: bind a function of the Blueprint (or a static library "
				"function) that calls it"), *Function->GetName(), *Function->GetOwnerClass()->GetName(), *Blueprint->GetName()));
		return Function;
	}

	/** The action event pin a trigger event name means (Started, Triggered, ...; "Cancelled" too); false after Fail. */
	bool TriggerPinName(const FString& Name, FName& Out)
	{
		const FString Wanted = Name.TrimStartAndEnd().Equals(TEXT("Cancelled"), ESearchCase::IgnoreCase) ? FString(TEXT("Canceled")) : Name.TrimStartAndEnd();
		for (const TCHAR* Pin : TriggerPins)
		{
			if (Wanted.Equals(Pin, ESearchCase::IgnoreCase))
			{
				Out = Pin;
				return true;
			}
		}
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a trigger event (Started: the first frame the triggers are met, e.g. pressed; "
			"Triggered: every frame they are met, once for Pressed / Released / Tap; Ongoing; Canceled; Completed: no longer met, e.g. released)"), *Name));
		return false;
	}

	/** The last free "then" pin of what @p Pin already runs (or @p Pin); null after Fail, also when the chain already calls @p Function. */
	UEdGraphPin* ChainEnd(UEdGraphPin* Pin, const UFunction* Function)
	{
		UEdGraphPin* Free = Pin;
		TSet<UEdGraphNode*> Seen;
		while (Free->LinkedTo.Num() > 0)
		{
			UEdGraphNode* Next = Free->LinkedTo[0]->GetOwningNode();
			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Next);
			if (Function && Call && Call->GetTargetFunction() == Function)
				return UnrealMcpRql::Failed<UEdGraphPin*>(FString::Printf(TEXT("%s already calls %s"), *Pin->PinName.ToString(), *Function->GetName()));
			UEdGraphPin* Then = ThenPin(Next);
			if (!Then || Seen.Contains(Next) || Free->LinkedTo.Num() > 1)
				return UnrealMcpRql::Failed<UEdGraphPin*>(FString::Printf(TEXT("%s already runs %s, which branches: connect the call where it belongs with "
					"ref(node) | call Connect(...)"), *Pin->PinName.ToString(), *Title(Next)));
			Seen.Add(Next);
			Free = Then;
		}
		return Free;
	}

	/** The action event node of @p Action in @p Blueprint, or null. */
	UK2Node_EnhancedInputAction* FindActionEvent(UBlueprint* Blueprint, const UInputAction* Action)
	{
		TArray<UK2Node_EnhancedInputAction*> Existing;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Existing);
		for (UK2Node_EnhancedInputAction* Node : Existing)
		{
			if (Node->InputAction == Action)
				return Node;
		}
		return nullptr;
	}

	/** The node titles @p Pin runs, in exec order (at most 12; a branch's outputs one after the other). */
	TArray<TSharedPtr<FJsonValue>> Runs(const UEdGraphPin* Pin)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		TArray<const UEdGraphPin*> Queue = { Pin };
		TSet<const UEdGraphNode*> Seen;
		while (Queue.Num() > 0 && Out.Num() < 12)
		{
			const UEdGraphPin* Current = Queue[0];
			Queue.RemoveAt(0);
			for (const UEdGraphPin* To : Current->LinkedTo)
			{
				const UEdGraphNode* Node = To->GetOwningNode();
				if (Seen.Contains(Node))
					continue;
				Seen.Add(Node);
				Out.Add(MakeShared<FJsonValueString>(Title(Node)));
				for (const UEdGraphPin* Next : Node->Pins)
				{
					if (Next->Direction == EGPD_Output && Next->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
						Queue.Add(Next);
				}
			}
		}
		return Out;
	}
}

using namespace UnrealAIEnhancedInputGraphLocal;

UEdGraph* UnrealAIEnhancedInputGraph::EventGraphOf(UObject* Target, UBlueprint*& OutBlueprint)
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

UEdGraphNode* UnrealAIEnhancedInputGraph::FindContextCall(UBlueprint* Blueprint, const UInputMappingContext* Context, bool bAdd, const FString& OnlyEvent, FString* OutEvent)
{
	TArray<UK2Node_CallFunction*> Calls;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Calls);
	for (UK2Node_CallFunction* Call : Calls)
	{
		const UEdGraphPin* Pin = Call->FunctionReference.GetMemberName() == (bAdd ? AddFunctionName : RemoveFunctionName) ? Call->FindPin(ContextPinName) : nullptr;
		if (!Pin || Pin->DefaultObject != Context)
			continue;
		const UK2Node_Event* Event = OwningEvent(Call);
		const FString Name = Event ? EventName(Event) : FString(TEXT("no event"));
		if (!OnlyEvent.IsEmpty() && !Name.Equals(OnlyEvent, ESearchCase::IgnoreCase))
			continue;
		if (OutEvent)
			*OutEvent = Name;
		return Call;
	}
	return nullptr;
}

UEdGraphNode* UnrealAIEnhancedInputGraph::AddContextSteps(UBlueprint* Blueprint, UEdGraph* EventGraph, const FString& Event, TArrayView<const FContextStep> Steps)
{
	FEventSite Site;
	if (!FindEvent(Blueprint, Event.TrimStartAndEnd(), Site))
		return nullptr;
	const bool bBeginPlay = Site.Override && Site.Override->GetFName() == TEXT("ReceiveBeginPlay");
	for (const FContextStep& Step : Steps)
	{
		if (!Step.ControllerParameter.IsNone() && !(Site.Override && Site.Override->FindPropertyByName(Step.ControllerParameter)))
			return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s has no parameter %s"), *Event, *Step.ControllerParameter.ToString()));
	}

	// The new nodes are wired among themselves first; the event (found, enabled or created) then runs them before what it ran.
	UEdGraph* Graph = Site.Event ? Site.Event->GetGraph() : EventGraph;
	Graph->Modify();
	UEdGraphPin* Then = MaterializeEvent(Blueprint, Graph, Site);
	if (!Then)
		return nullptr;
	const TArray<UEdGraphPin*> Before = Then->LinkedTo;
	Site.Event->Modify();
	GetDefault<UEdGraphSchema_K2>()->BreakPinLinks(*Then, /*bSendsNodeNotifcation*/ true);
	FChainCursor Cursor;
	Cursor.Graph = Graph;
	Cursor.Tails.Add(Then);
	const UEdGraphNode* ThenOwner = Then->GetOwningNode();
	Cursor.X = ThenOwner->NodePosX + 320;
	Cursor.Y = ThenOwner->NodePosY;
	UEdGraphNode* Last = nullptr;
	for (const FContextStep& Step : Steps)
	{
		Last = AddStep(Blueprint, Site.Event, bBeginPlay, Step, Cursor);
		if (!Last)
			return nullptr;
	}
	for (UEdGraphPin* Next : Before)
	{
		for (UEdGraphPin* Tail : Cursor.Tails)
		{
			if (!Link(Tail, Next))
				return nullptr;
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return Last;
}

UEdGraphNode* UnrealAIEnhancedInputGraph::FindOrAddActionEvent(UBlueprint* Blueprint, UEdGraph* EventGraph, UInputAction* Action, bool& bOutCreated)
{
	bOutCreated = false;
	if (!GetDefault<UK2Node_EnhancedInputAction>()->IsCompatibleWithGraph(EventGraph))
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s cannot have input events"), *Blueprint->GetName()));
	if (UK2Node_EnhancedInputAction* Existing = FindActionEvent(Blueprint, Action))
		return Existing;
	EventGraph->Modify();
	const FVector2D At = EventGraph->GetGoodPlaceForNewNode();
	UEdGraphNode* Node = CreateGraphNode(EventGraph, UK2Node_EnhancedInputAction::StaticClass(), At.X, At.Y,
		[Action](UEdGraphNode* Created) { CastChecked<UK2Node_EnhancedInputAction>(Created)->InputAction = Action; });
	// Not marked structurally modified here: that regenerates the skeleton class, whose functions (custom events) a caller
	// may hold; callers mark the Blueprint when they are done.
	bOutCreated = true;
	return Node;
}

UEdGraphNode* UnrealAIEnhancedInputGraph::BindAction(UBlueprint* Blueprint, UEdGraph* EventGraph, UInputAction* Action, const FString& TriggerEvent,
	const FString& Function, const FString& ValueParameter)
{
	// Everything that can fail is checked before the graph changes.
	UFunction* Target = CallableFunction(Blueprint, Function);
	if (!Target)
		return nullptr;
	const FString Value = ValueParameter.TrimStartAndEnd();
	const bool bNoValue = Value.Equals(TEXT("none"), ESearchCase::IgnoreCase);
	if (!Value.IsEmpty() && !bNoValue && !Target->FindPropertyByName(FName(*Value)))
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s has no parameter '%s'"), *Target->GetName(), *Value));
	FName PinName;
	if (!TriggerPinName(TriggerEvent, PinName))
		return nullptr;
	UK2Node_EnhancedInputAction* Existing = FindActionEvent(Blueprint, Action);
	UEdGraphPin* ExistingPin = Existing ? RequirePin(Existing, PinName) : nullptr;
	if (Existing && (!ExistingPin || !ChainEnd(ExistingPin, Target)))
		return nullptr;

	bool bCreated = false;
	UEdGraphNode* Node = FindOrAddActionEvent(Blueprint, EventGraph, Action, bCreated);
	UEdGraphPin* Pin = Node ? RequirePin(Node, PinName) : nullptr;
	UEdGraphPin* Free = Pin ? ChainEnd(Pin, Target) : nullptr;
	if (!Free)
		return nullptr;
	UEdGraph* Graph = Node->GetGraph();
	Graph->Modify();
	int32 Row = 0;
	for (const TCHAR* Name : TriggerPins)
	{
		if (Pin->PinName == Name)
			break;
		++Row;
	}
	const UEdGraphNode* FreeOwner = Free->GetOwningNode();
	UK2Node_CallFunction* Call = CreateCallNode(Graph, Target, FreeOwner->NodePosX + 360, (FreeOwner == Node ? Node->NodePosY + Row * 130 : FreeOwner->NodePosY));
	if (!Link(Free, RequirePin(Call, UEdGraphSchema_K2::PN_Execute)))
		return nullptr;

	// The ActionValue: into the named parameter, or (axis actions) into the first parameter of its type.
	UEdGraphPin* ActionValue = Node->FindPin(ActionValuePinName);
	if (ActionValue && !bNoValue && (!Value.IsEmpty() || Action->ValueType != EInputActionValueType::Boolean))
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		UEdGraphPin* Parameter = Value.IsEmpty() ? nullptr : RequirePin(Call, FName(*Value));
		if (!Value.IsEmpty() && (!Parameter || !Link(ActionValue, Parameter)))
			return nullptr;
		for (UEdGraphPin* Candidate : Value.IsEmpty() ? Call->Pins : TArray<UEdGraphPin*>())
		{
			if (Candidate->Direction == EGPD_Input && Candidate->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && Candidate->PinName != UEdGraphSchema_K2::PN_Self
				&& Schema->CanCreateConnection(ActionValue, Candidate).Response == CONNECT_RESPONSE_MAKE)
			{
				Schema->TryCreateConnection(ActionValue, Candidate);
				break;
			}
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return Call;
}

UEdGraphNode* UnrealAIEnhancedInputGraph::BindActionToggle(UBlueprint* Blueprint, UEdGraph* EventGraph, UInputAction* Action, const FString& TriggerEvent,
	const FString& OnFunction, const FString& OffFunction)
{
	UFunction* On = CallableFunction(Blueprint, OnFunction);
	UFunction* Off = On ? CallableFunction(Blueprint, OffFunction) : nullptr;
	const UBlueprint* Macros = Off ? LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")) : nullptr;
	UEdGraph* FlipFlop = nullptr;
	for (UEdGraph* Graph : Macros ? Macros->MacroGraphs : TArray<TObjectPtr<UEdGraph>>())
	{
		if (Graph && Graph->GetFName() == TEXT("FlipFlop"))
			FlipFlop = Graph;
	}
	if (!On || !Off)
		return nullptr;
	if (!FlipFlop)
		return UnrealMcpRql::Failed<UEdGraphNode*>(TEXT("the engine's FlipFlop macro (StandardMacros) is missing"));
	FName PinName;
	if (!TriggerPinName(TriggerEvent, PinName))
		return nullptr;
	UK2Node_EnhancedInputAction* Existing = FindActionEvent(Blueprint, Action);
	UEdGraphPin* ExistingPin = Existing ? RequirePin(Existing, PinName) : nullptr;
	if (Existing && !ExistingPin)
		return nullptr;
	if (ExistingPin && ExistingPin->LinkedTo.Num() > 0)
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s of %s already runs %s: a toggle needs the pin to itself (use another trigger event, "
			"or remove what it runs)"), *ExistingPin->PinName.ToString(), *Action->GetName(), *Title(ExistingPin->LinkedTo[0]->GetOwningNode())));

	bool bCreated = false;
	UEdGraphNode* Node = FindOrAddActionEvent(Blueprint, EventGraph, Action, bCreated);
	UEdGraphPin* Pin = Node ? RequirePin(Node, PinName) : nullptr;
	if (!Pin)
		return nullptr;
	UEdGraph* Graph = Node->GetGraph();
	Graph->Modify();
	const double X = Node->NodePosX + 360;
	const double Y = Node->NodePosY;
	UEdGraphNode* Toggle = CreateGraphNode(Graph, UK2Node_MacroInstance::StaticClass(), X, Y,
		[FlipFlop](UEdGraphNode* Created) { CastChecked<UK2Node_MacroInstance>(Created)->SetMacroGraph(FlipFlop); });
	UK2Node_CallFunction* OnCall = CreateCallNode(Graph, On, X + 300, Y - 60);
	UK2Node_CallFunction* OffCall = CreateCallNode(Graph, Off, X + 300, Y + 100);
	if (!Link(Pin, ExecInput(Toggle)) || !Link(RequirePin(Toggle, TEXT("A")), RequirePin(OnCall, UEdGraphSchema_K2::PN_Execute))
		|| !Link(RequirePin(Toggle, TEXT("B")), RequirePin(OffCall, UEdGraphSchema_K2::PN_Execute)))
		return nullptr;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return Toggle;
}

TSharedPtr<FJsonValue> UnrealAIEnhancedInputGraph::InputSetupJson(const UBlueprint* Blueprint)
{
	TSharedPtr<FJsonObject> Setup = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Contexts;
	TArray<TSharedPtr<FJsonValue>> Actions;
	if (Blueprint)
	{
		UBlueprint* Mutable = const_cast<UBlueprint*>(Blueprint);
		TArray<UK2Node_CallFunction*> Calls;
		FBlueprintEditorUtils::GetAllNodesOfClass(Mutable, Calls);
		for (const UK2Node_CallFunction* Call : Calls)
		{
			const FName Member = Call->FunctionReference.GetMemberName();
			if (Member != AddFunctionName && Member != RemoveFunctionName)
				continue;
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			const UK2Node_Event* Event = OwningEvent(Call);
			Row->SetStringField(TEXT("Event"), Event ? EventName(Event) : FString(TEXT("none (not connected to an event)")));
			Row->SetStringField(TEXT("Op"), Member == AddFunctionName ? TEXT("Add") : TEXT("Remove"));
			const UEdGraphPin* Context = Call->FindPin(ContextPinName);
			Row->SetStringField(TEXT("Context"), !Context ? FString() : Context->LinkedTo.Num() > 0 ? FString(TEXT("(connected)"))
				: Context->DefaultObject ? Context->DefaultObject->GetPackage()->GetName() : FString(TEXT("None")));
			if (const UEdGraphPin* Priority = Member == AddFunctionName ? Call->FindPin(TEXT("Priority")) : nullptr)
				Row->SetNumberField(TEXT("Priority"), FCString::Atoi(*Priority->DefaultValue));
			Contexts.Add(MakeShared<FJsonValueObject>(Row));
		}
		TArray<UK2Node_EnhancedInputAction*> Events;
		FBlueprintEditorUtils::GetAllNodesOfClass(Mutable, Events);
		for (const UK2Node_EnhancedInputAction* Node : Events)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Action"), Node->InputAction ? Node->InputAction->GetName() : FString(TEXT("None")));
			for (const TCHAR* Name : TriggerPins)
			{
				const UEdGraphPin* Pin = Node->FindPin(Name);
				if (Pin && Pin->LinkedTo.Num() > 0)
					Row->SetArrayField(Name, Runs(Pin));
			}
			const UEdGraphPin* Value = Node->FindPin(ActionValuePinName);
			if (Value && Value->LinkedTo.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Into;
				for (const UEdGraphPin* To : Value->LinkedTo)
					Into.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *Title(To->GetOwningNode()), *To->PinName.ToString())));
				Row->SetArrayField(TEXT("ActionValue"), Into);
			}
			Actions.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Setup->SetArrayField(TEXT("Contexts"), Contexts);
	Setup->SetArrayField(TEXT("Actions"), Actions);
	return MakeShared<FJsonValueObject>(Setup);
}
