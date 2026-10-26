// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphToken.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "Misc/ITransaction.h"
#include "UObject/UnrealType.h"

#include <memory>
#include <string>
#include <vector>

/**
 * Blueprint graphs (docs/rql.md "Blueprint graphs"): a Blueprint's graphs, their nodes and the nodes' pins, the node
 * calls the graph editor's context menu offers for the common cases (function calls, variables, events, flow control,
 * casts), pin connections through the K2 schema (the same checks the editor applies to a drag between pins) and
 * compile. Graphs and nodes are UObjects (members and refs as usual); pins are not, so a pin is a host object holding
 * its node and pin id, resolved on every read. Every change is undoable (one transaction per mutation) and marks the
 * Blueprint modified; nothing is compiled until `call Compile()`.
 */
namespace UnrealMcpRqlBlueprintsLocal
{
	using namespace UnrealMcpRqlUtil;

	/** A pin of a graph node: valid while the node lives and still has a pin with that id. */
	struct FPinHandle
	{
		TWeakObjectPtr<UEdGraphNode> Node;
		FGuid PinId;

		UEdGraphPin* Resolve() const
		{
			UEdGraphNode* Owner = Node.Get();
			return Owner ? Owner->FindPinById(PinId) : nullptr;
		}
		UEdGraphPin* Require() const
		{
			UEdGraphPin* Pin = Resolve();
			if (!Pin)
				Throw(TEXT("the pin is gone (its node was removed or rebuilt): read the node's Pins again"));
			return Pin;
		}
	};

	char PinTypeKey = 0; // the native type key of EdGraphPin

	UBlueprint* BlueprintOf(const UObject* Object)
	{
		if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
			return FBlueprintEditorUtils::FindBlueprintForNode(Node);
		if (const UEdGraph* Graph = Cast<UEdGraph>(Object))
			return FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
		return nullptr;
	}

	UBlueprint* RequireBlueprint(const UObject* Object)
	{
		UBlueprint* Blueprint = BlueprintOf(Object);
		if (!Blueprint)
			Throw(FString::Printf(TEXT("%s is not part of a Blueprint"), Object ? *Object->GetName() : TEXT("the object")));
		return Blueprint;
	}

	const UEdGraphSchema_K2* K2Schema(const UEdGraph* Graph)
	{
		const UEdGraphSchema_K2* Schema = Graph ? Cast<UEdGraphSchema_K2>(Graph->GetSchema()) : nullptr;
		if (!Schema)
			Throw(FString::Printf(TEXT("%s is not a Blueprint (K2) graph"), Graph ? *Graph->GetName() : TEXT("the graph")));
		return Schema;
	}

	FString GraphKind(const UEdGraph* Graph)
	{
		const UBlueprint* Blueprint = BlueprintOf(Graph);
		if (!Blueprint)
			return TEXT("Other");
		if (Blueprint->UbergraphPages.Contains(Graph))
			return TEXT("EventGraph");
		if (Blueprint->FunctionGraphs.Contains(Graph))
			return TEXT("Function");
		if (Blueprint->MacroGraphs.Contains(Graph))
			return TEXT("Macro");
		if (Blueprint->DelegateSignatureGraphs.Contains(Graph))
			return TEXT("Delegate");
		return TEXT("Other");
	}

	FString CompactName(const FString& Text)
	{
		return Text.Replace(TEXT(" "), TEXT(""));
	}

	FString PinNames(const UEdGraphNode* Node)
	{
		TArray<FString> Names;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden)
				Names.Add(Pin->PinName.ToString());
		}
		return FString::Join(Names, TEXT(", "));
	}

	FString NodeTitle(const UEdGraphNode* Node)
	{
		return Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}

	/**
	 * The pin of @p Node that @p Name names: its pin name, its display name (spaces ignored), or "exec" / "then" for the
	 * execution input / first execution output. Throws with the node's pins when there is none.
	 */
	UEdGraphPin* FindNodePin(UEdGraphNode* Node, const FString& Name)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(Wanted, ESearchCase::IgnoreCase))
				return Pin;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && CompactName(Pin->GetDisplayName().ToString()).Equals(CompactName(Wanted), ESearchCase::IgnoreCase))
				return Pin;
		}
		const bool bExecIn = Wanted.Equals(TEXT("exec"), ESearchCase::IgnoreCase) || Wanted.Equals(TEXT("in"), ESearchCase::IgnoreCase);
		const bool bExecOut = Wanted.Equals(TEXT("out"), ESearchCase::IgnoreCase);
		if (bExecIn || bExecOut)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == (bExecIn ? EGPD_Input : EGPD_Output))
					return Pin;
			}
		}
		Throw(FString::Printf(TEXT("'%s' has no pin '%s' (pins: %s)"), *NodeTitle(Node), *Wanted, *PinNames(Node)));
	}

	void Touched(UEdGraphNode* Node, bool bStructural)
	{
		if (UBlueprint* Blueprint = BlueprintOf(Node))
		{
			if (bStructural)
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			else
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
	}

	void Connect(UEdGraphPin* A, UEdGraphPin* B)
	{
		UEdGraphNode* NodeA = A->GetOwningNode();
		UEdGraphNode* NodeB = B->GetOwningNode();
		if (NodeA->GetGraph() != NodeB->GetGraph())
			Throw(TEXT("pins of different graphs cannot be connected"));
		const UEdGraphSchema_K2* Schema = K2Schema(NodeA->GetGraph());
		const FPinConnectionResponse Response = Schema->CanCreateConnection(A, B);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			Throw(FString::Printf(TEXT("cannot connect %s.%s to %s.%s: %s"), *NodeTitle(NodeA), *A->PinName.ToString(),
				*NodeTitle(NodeB), *B->PinName.ToString(), *Response.Message.ToString()));
		NodeA->Modify();
		NodeB->Modify();
		if (!Schema->TryCreateConnection(A, B))
			Throw(FString::Printf(TEXT("could not connect %s to %s"), *A->PinName.ToString(), *B->PinName.ToString()));
		Touched(NodeA, false);
	}

	void Disconnect(UEdGraphPin* A, UEdGraphPin* B)
	{
		if (!A->LinkedTo.Contains(B))
			Throw(FString::Printf(TEXT("%s is not connected to %s"), *A->PinName.ToString(), *B->PinName.ToString()));
		A->GetOwningNode()->Modify();
		B->GetOwningNode()->Modify();
		K2Schema(A->GetOwningNode()->GetGraph())->BreakSinglePinLink(A, B);
		Touched(A->GetOwningNode(), false);
	}

	void BreakLinks(UEdGraphPin* Pin)
	{
		UEdGraphNode* Node = Pin->GetOwningNode();
		Node->Modify();
		for (UEdGraphPin* Other : Pin->LinkedTo)
		{
			if (Other && Other->GetOwningNode())
				Other->GetOwningNode()->Modify();
		}
		K2Schema(Node->GetGraph())->BreakPinLinks(*Pin, /*bSendsNodeNotifcation*/ true);
		Touched(Node, false);
	}

	/** Set a pin's default as the Details / pin editor does: validated by the schema; object pins take an asset path or class. */
	void SetPinDefault(UEdGraphPin* Pin, const rql::Value& Value, FUnrealMcpRqlTypes& Types)
	{
		if (Pin->Direction != EGPD_Input)
			Throw(FString::Printf(TEXT("%s is an output pin: only input pins have defaults"), *Pin->PinName.ToString()));
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			Throw(FString::Printf(TEXT("%s is an execution pin: it has no default"), *Pin->PinName.ToString()));
		if (Pin->PinType.IsContainer())
			Throw(FString::Printf(TEXT("%s is a container pin: connect a Make Array / Make Set node instead"), *Pin->PinName.ToString()));
		UEdGraphNode* Node = Pin->GetOwningNode();
		const UEdGraphSchema_K2* Schema = K2Schema(Node->GetGraph());
		const FName Category = Pin->PinType.PinCategory;
		Node->Modify();
		if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_Interface)
		{
			UObject* Object = nullptr;
			if (!Value.is_null() && !(Value.is_string() && Value.as_string().empty()))
			{
				Object = Types.AsObject(Value);
				if (!Object)
				{
					UClass* Wanted = Category == UEdGraphSchema_K2::PC_Class ? UClass::StaticClass()
						: Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
					const FString Text = TextArg(Value);
					Object = FUnrealMcpObjectRef::ResolveReference(Wanted ? Wanted : UObject::StaticClass(), Text);
					if (!Object)
						Throw(FString::Printf(TEXT("no %s '%s' for pin %s"), Wanted ? *Wanted->GetName() : TEXT("object"), *Text, *Pin->PinName.ToString()));
				}
			}
			const FString Problem = Schema->IsPinDefaultValid(Pin, FString(), Object, FText());
			if (!Problem.IsEmpty())
				Throw(FString::Printf(TEXT("%s: %s"), *Pin->PinName.ToString(), *Problem));
			Schema->TrySetDefaultObject(*Pin, Object);
		}
		else if (Category == UEdGraphSchema_K2::PC_Text)
		{
			Schema->TrySetDefaultText(*Pin, FText::FromString(TextArg(Value)));
		}
		else
		{
			FString Text = TextArg(Value);
			if (Value.is_bool())
				Text = Value.as_bool() ? TEXT("true") : TEXT("false");
			else if (Value.is_struct() && Value.type())
			{
				// A struct value (a vector read from elsewhere): its export text.
				if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					FString Exported;
					Struct->ExportText(Exported, Value.ptr(), nullptr, nullptr, PPF_None, nullptr);
					Text = Exported;
				}
			}
			else if (Category == UEdGraphSchema_K2::PC_Struct && (Value.is_string() || Value.is_list()))
			{
				// Math structs in their RQL forms: "(1, 2, 3)", [1, 2, 3] -> the pin's own text format.
				if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					rql::Value Built = Types.WrapStruct(Struct, nullptr);
					bool bConverted = true;
					try
					{
						Types.CoerceStruct(Struct, Value, Built.ptr());
					}
					catch (const std::exception&)
					{
						bConverted = false; // not an RQL form: let the schema judge the text as typed
					}
					if (bConverted)
					{
						FString Exported;
						Struct->ExportText(Exported, Built.ptr(), nullptr, nullptr, PPF_None, nullptr);
						Text = Exported;
					}
				}
			}
			const FString Problem = Schema->IsPinDefaultValid(Pin, Text, nullptr, FText());
			if (!Problem.IsEmpty())
				Throw(FString::Printf(TEXT("%s: %s"), *Pin->PinName.ToString(), *Problem));
			Schema->TrySetDefaultValue(*Pin, Text);
		}
		Touched(Node, false);
	}

	rql::Value WrapPin(const rql::TypeDescriptor* PinType, UEdGraphPin* Pin)
	{
		if (!Pin || !Pin->GetOwningNodeUnchecked())
			return rql::Value::null();
		auto Handle = std::make_shared<FPinHandle>();
		Handle->Node = Pin->GetOwningNode();
		Handle->PinId = Pin->PinId;
		FPinHandle* Raw = Handle.get();
		return rql::Value::object(PinType, Raw, std::static_pointer_cast<void>(Handle));
	}

	rql::Value WrapPins(const rql::TypeDescriptor* PinType, const TArray<UEdGraphPin*>& Pins)
	{
		std::vector<rql::Value> Items;
		for (UEdGraphPin* Pin : Pins)
		{
			if (Pin && !Pin->bHidden)
				Items.push_back(WrapPin(PinType, Pin));
		}
		return rql::Value::list_of(std::move(Items));
	}

	FPinHandle& Handle(void* Self)
	{
		return *static_cast<FPinHandle*>(Self);
	}

	/** The pin a `call` argument names: a pin value (Pins[...]) of the same session. */
	UEdGraphPin* PinArg(const rql::Value& Value, const rql::TypeDescriptor* PinType)
	{
		if (!Value.is_object() || Value.type() != PinType || !Value.ptr())
			ThrowMismatch(TEXT("a pin (a node's Pins[...])"), Value);
		return static_cast<FPinHandle*>(Value.ptr())->Require();
	}

	// ---------------------------------------------------------------- node creation

	FVector2D Place(UEdGraph* Graph, const rql::Value& X, const rql::Value& Y)
	{
		if (X.is_number() && Y.is_number())
			return FVector2D(X.as_number(), Y.as_number());
		const FVector2D Good = Graph->GetGoodPlaceForNewNode();
		return FVector2D(X.is_number() ? X.as_number() : Good.X, Y.is_number() ? Y.as_number() : Good.Y);
	}

	/**
	 * Create a node of @p NodeType in @p Graph at @p Position; @p Configure sets it up before its pins are allocated and
	 * must not throw (FGraphNodeCreator asserts that every created node is finalized: validate before calling this).
	 */
	template <typename TNode>
	TNode* CreateNode(UEdGraph* Graph, const FVector2D& Position, TFunctionRef<void(TNode*)> Configure)
	{
		Graph->Modify();
		FGraphNodeCreator<TNode> Creator(*Graph);
		TNode* Node = Creator.CreateNode(/*bSelectNewNode*/ false);
		Node->NodePosX = FMath::RoundToInt(Position.X);
		Node->NodePosY = FMath::RoundToInt(Position.Y);
		Configure(Node);
		Creator.Finalize();
		Touched(Node, true);
		return Node;
	}

	/** A name no variable, function, graph, component or other object of @p Blueprint uses (the editor's own name check). */
	void RequireFreeName(UBlueprint* Blueprint, const FString& Name, const TCHAR* What)
	{
		if (Name.IsEmpty() || !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS))
			Throw(FString::Printf(TEXT("'%s' is not a valid %s name"), *Name, What));
		if (FKismetNameValidator(Blueprint).IsValid(Name) != EValidatorResult::Ok)
			Throw(FString::Printf(TEXT("%s already has a variable, function, graph, event or component named '%s'"), *Blueprint->GetName(), *Name));
	}

	void RequireEventGraph(UEdGraph* Graph)
	{
		if (GraphKind(Graph) != TEXT("EventGraph"))
			Throw(FString::Printf(TEXT("events go into an event graph (Blueprint EventGraph), not the %s graph '%s'"),
				*GraphKind(Graph).ToLower(), *Graph->GetName()));
	}

	/** Classes a bare function name is looked up in after the Blueprint itself (the editor's most used libraries). */
	const TCHAR* const LibraryClasses[] = {
		TEXT("KismetSystemLibrary"), TEXT("KismetMathLibrary"), TEXT("KismetStringLibrary"), TEXT("KismetTextLibrary"),
		TEXT("KismetArrayLibrary"), TEXT("GameplayStatics"), TEXT("BlueprintMapLibrary"), TEXT("BlueprintSetLibrary"),
	};

	UFunction* FindCallable(UClass* Class, const FString& Name)
	{
		if (!Class)
			return nullptr;
		for (const FString& Candidate : { Name, FString(TEXT("K2_")) + Name })
		{
			UFunction* Function = Class->FindFunctionByName(FName(*Candidate, FNAME_Find));
			if (Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
				return Function;
		}
		return nullptr;
	}

	/** "PrintString", "KismetSystemLibrary.PrintString", "/Script/Engine.KismetSystemLibrary:PrintString" or a function of the Blueprint. */
	UFunction* ResolveFunction(UBlueprint* Blueprint, const FString& Spec)
	{
		FString Text = Spec.TrimStartAndEnd();
		FString ClassPart, FunctionPart;
		if (Text.Split(TEXT(":"), &ClassPart, &FunctionPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
			|| Text.Split(TEXT("."), &ClassPart, &FunctionPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassPart);
			if (!Class)
				Throw(FString::Printf(TEXT("no class '%s'"), *ClassPart));
			if (UFunction* Function = FindCallable(Class, FunctionPart))
				return Function;
			Throw(FString::Printf(TEXT("%s has no Blueprint-callable function '%s'"), *Class->GetName(), *FunctionPart));
		}
		UClass* Own = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
		if (UFunction* Function = FindCallable(Own, Text))
			return Function;
		for (const TCHAR* Library : LibraryClasses)
		{
			if (UFunction* Function = FindCallable(FUnrealMcpObjectRef::ResolveClass(Library), Text))
				return Function;
		}
		Throw(FString::Printf(TEXT("no Blueprint-callable function '%s' on %s or the common libraries (name the class: \"KismetMathLibrary.Add_IntInt\")"),
			*Text, *Blueprint->GetName()));
	}

	/** A variable get / set node needs a variable the Blueprint (or its parent class) exposes to Blueprints. */
	FName RequireVariable(UBlueprint* Blueprint, const FString& Name, bool bWrite)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		UClass* Own = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
		FProperty* Property = Own ? FindFProperty<FProperty>(Own, FName(*Trimmed, FNAME_Find)) : nullptr;
		if (!Property)
		{
			for (TFieldIterator<FProperty> It(Own); It && !Property; ++It)
			{
				if (It->GetAuthoredName().Equals(Trimmed, ESearchCase::IgnoreCase))
					Property = *It;
			}
		}
		if (!Property || !Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
			Throw(FString::Printf(TEXT("%s has no Blueprint variable '%s' (add one with blueprint-add-variable)"), *Blueprint->GetName(), *Trimmed));
		if (bWrite && Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
			Throw(FString::Printf(TEXT("%s is read-only in Blueprints"), *Trimmed));
		return Property->GetFName();
	}

	UBlueprint* StandardMacros()
	{
		return LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	UEdGraph* FindMacro(const FString& Name)
	{
		UBlueprint* Macros = StandardMacros();
		if (!Macros)
			return nullptr;
		for (UEdGraph* Graph : Macros->MacroGraphs)
		{
			if (Graph && (Graph->GetName().Equals(Name, ESearchCase::IgnoreCase) || CompactName(Graph->GetName()).Equals(CompactName(Name), ESearchCase::IgnoreCase)))
				return Graph;
		}
		return nullptr;
	}

	FString MacroNames()
	{
		TArray<FString> Names;
		if (UBlueprint* Macros = StandardMacros())
		{
			for (UEdGraph* Graph : Macros->MacroGraphs)
			{
				if (Graph)
					Names.Add(Graph->GetName());
			}
		}
		return FString::Join(Names, TEXT(", "));
	}

	/** Compile @p Blueprint outside the mutation's transaction (a compile rebuilds classes: it is not an edit to undo). */
	rql::Value Compile(UBlueprint* Blueprint)
	{
		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		{
			TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);
		}
		std::vector<rql::Value> Messages;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning)
				continue;
			FString Node;
			for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
			{
				// The compiler names nodes with graph tokens (pins and nodes of the source graph); object tokens are older.
				const UObject* Object = nullptr;
				if (Token->GetType() == EMessageToken::EdGraph)
				{
					const FEdGraphToken& Graph = static_cast<const FEdGraphToken&>(Token.Get());
					Object = Graph.GetPin() ? Graph.GetPin()->GetOwningNodeUnchecked() : Graph.GetGraphObject();
				}
				else if (Token->GetType() == EMessageToken::Object)
					Object = static_cast<const FUObjectToken&>(Token.Get()).GetObject().Get();
				if (const UEdGraphNode* GraphNode = Cast<UEdGraphNode>(Object))
				{
					Node = FString::Printf(TEXT("%s#%lld"), *GraphNode->GetClass()->GetName(), static_cast<long long>(FUnrealMcpRqlTypes::IdOf(GraphNode)));
					break;
				}
			}
			std::vector<std::pair<rql::Value, rql::Value>> Entry;
			Entry.emplace_back(Str(TEXT("Severity")), Str(Severity == EMessageSeverity::Error ? TEXT("Error") : TEXT("Warning")));
			Entry.emplace_back(Str(TEXT("Message")), Str(Message->ToText().ToString()));
			Entry.emplace_back(Str(TEXT("Node")), Node.IsEmpty() ? rql::Value::null() : Str(Node));
			Messages.push_back(rql::Value::map(std::make_shared<rql::OwnedMap>(nullptr, std::move(Entry))));
		}
		std::vector<std::pair<rql::Value, rql::Value>> Out;
		Out.emplace_back(Str(TEXT("Succeeded")), rql::Value::boolean(Results.NumErrors == 0));
		Out.emplace_back(Str(TEXT("Errors")), rql::Value::number(Results.NumErrors));
		Out.emplace_back(Str(TEXT("Warnings")), rql::Value::number(Results.NumWarnings));
		Out.emplace_back(Str(TEXT("Messages")), rql::Value::list_of(std::move(Messages)));
		return rql::Value::map(std::make_shared<rql::OwnedMap>(nullptr, std::move(Out)));
	}

	FString StatusText(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Dirty: return TEXT("Dirty");
		case BS_Error: return TEXT("Error");
		case BS_UpToDate: return TEXT("UpToDate");
		case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
		case BS_BeingCreated: return TEXT("BeingCreated");
		default: return TEXT("Unknown");
		}
	}

	// ---------------------------------------------------------------- hooks

	void BlueprintMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* GraphType = T.ForClass(UEdGraph::StaticClass());
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		FUnrealMcpRqlTypes::SetMember(D, Member("Graphs", T.Registry().list_type(GraphType, true), [Types](void* Self)
		{
			UBlueprint* Blueprint = static_cast<UBlueprint*>(Self);
			TArray<UObject*> Graphs;
			for (const TArray<TObjectPtr<UEdGraph>>* List : { &Blueprint->UbergraphPages, &Blueprint->FunctionGraphs, &Blueprint->MacroGraphs })
			{
				for (UEdGraph* Graph : *List)
				{
					if (Graph)
						Graphs.Add(Graph);
				}
			}
			return Types->WrapObjects(Graphs);
		}, {}, "event graphs, then functions, then macros (each: Name Kind Nodes)", /*bOwning*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("EventGraph", GraphType, [Types](void* Self)
		{
			return Types->WrapObject(FBlueprintEditorUtils::FindEventGraph(static_cast<UBlueprint*>(Self)));
		}, {}, "the main event graph", /*bOwning*/ false, /*bNullable*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Status", String, [](void* Self)
		{
			return Str(StatusText(static_cast<UBlueprint*>(Self)->Status));
		}, {}, "compile status: UpToDate, UpToDateWithWarnings, Dirty (changed since the last compile), Error"));
		FUnrealMcpRqlTypes::SetMember(D, Member("ParentClass", String, [](void* Self)
		{
			const UClass* Parent = static_cast<UBlueprint*>(Self)->ParentClass;
			return Parent ? Str(Parent->GetName()) : rql::Value::null();
		}, {}, "the class it derives from (Actor, Character, BP_Base_C, ...)", /*bOwning*/ false, /*bNullable*/ true));

		FUnrealMcpRqlTypes::AddMethod(D, Method("Compile", {}, rql::builtin::any_type(),
			[](void* Self, const std::vector<rql::Value>&) -> rql::Value
			{
				return Compile(static_cast<UBlueprint*>(Self));
			}, "compile the Blueprint; returns {Succeeded, Errors, Warnings, Messages: [{Severity, Message, Node}]} (a failed compile is a result, not an error)"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddFunction", { FUnrealMcpRqlTypes::Param("Name", String) }, GraphType,
			[Types](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UBlueprint* Blueprint = static_cast<UBlueprint*>(Self);
				const FString Name = TextArg(Args[0]).TrimStartAndEnd();
				// CreateNewGraph renames the graph onto its name: a non-graph object of that name would be a fatal error.
				RequireFreeName(Blueprint, Name, TEXT("function"));
				Blueprint->Modify();
				UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				return Types->WrapObject(Graph);
			}, "add a function graph (with its entry node); returns the graph"));
	}

	void GraphMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* NodeType = T.ForClass(UEdGraphNode::StaticClass());
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		const rql::TypeDescriptor* Number = rql::builtin::double_type();
		FUnrealMcpRqlTypes::SetMember(D, Member("Nodes", T.Registry().list_type(NodeType, true), [Types](void* Self)
		{
			TArray<UObject*> Nodes;
			for (UEdGraphNode* Node : static_cast<UEdGraph*>(Self)->Nodes)
			{
				if (Node)
					Nodes.Add(Node);
			}
			return Types->WrapObjects(Nodes);
		}, {}, "the nodes of the graph", /*bOwning*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Kind", String, [](void* Self) { return Str(GraphKind(static_cast<UEdGraph*>(Self))); },
			{}, "EventGraph, Function, Macro, Delegate or Other"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Blueprint", T.ForClass(UBlueprint::StaticClass()),
			[Types](void* Self) { return Types->WrapObject(BlueprintOf(static_cast<UEdGraph*>(Self))); }, {}, "", false, true));

		auto WithPlace = [Number](std::vector<rql::ParamDescriptor> Params)
		{
			Params.push_back(FUnrealMcpRqlTypes::OptionalParam("X", Number, rql::Value::null(), /*bNullable*/ true));
			Params.push_back(FUnrealMcpRqlTypes::OptionalParam("Y", Number, rql::Value::null(), /*bNullable*/ true));
			return Params;
		};
		auto Graph = [](void* Self) { UEdGraph* Out = static_cast<UEdGraph*>(Self); K2Schema(Out); return Out; };

		FUnrealMcpRqlTypes::AddMethod(D, Method("AddCallFunction", WithPlace({ FUnrealMcpRqlTypes::Param("Function", String) }), NodeType,
			[Types, Graph](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraph* Target = Graph(Self);
				UFunction* Function = ResolveFunction(RequireBlueprint(Target), TextArg(Args[0]));
				return Types->WrapObject(CreateNode<UK2Node_CallFunction>(Target, Place(Target, Args[1], Args[2]),
					[Function](UK2Node_CallFunction* Node) { Node->SetFromFunction(Function); }));
			}, "add a function call node: \"PrintString\", \"KismetMathLibrary.Add_IntInt\" or a function of the Blueprint; returns the node"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddVariableGet", WithPlace({ FUnrealMcpRqlTypes::Param("Variable", String) }), NodeType,
			[Types, Graph](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraph* Target = Graph(Self);
				const FName Variable = RequireVariable(RequireBlueprint(Target), TextArg(Args[0]), /*bWrite*/ false);
				return Types->WrapObject(CreateNode<UK2Node_VariableGet>(Target, Place(Target, Args[1], Args[2]),
					[Variable](UK2Node_VariableGet* Node) { Node->VariableReference.SetSelfMember(Variable); }));
			}, "add a Get node for a variable of the Blueprint; returns the node"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddVariableSet", WithPlace({ FUnrealMcpRqlTypes::Param("Variable", String) }), NodeType,
			[Types, Graph](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraph* Target = Graph(Self);
				const FName Variable = RequireVariable(RequireBlueprint(Target), TextArg(Args[0]), /*bWrite*/ true);
				return Types->WrapObject(CreateNode<UK2Node_VariableSet>(Target, Place(Target, Args[1], Args[2]),
					[Variable](UK2Node_VariableSet* Node) { Node->VariableReference.SetSelfMember(Variable); }));
			}, "add a Set node for a variable of the Blueprint; returns the node"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddEvent", WithPlace({ FUnrealMcpRqlTypes::Param("Event", String) }), NodeType,
			[Types, Graph](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraph* Target = Graph(Self);
				RequireEventGraph(Target);
				UBlueprint* Blueprint = RequireBlueprint(Target);
				const FString Name = TextArg(Args[0]).TrimStartAndEnd();
				UFunction* Event = nullptr;
				for (const FString& Candidate : { Name, FString(TEXT("Receive")) + Name, FString(TEXT("K2_")) + Name })
				{
					UFunction* Found = Blueprint->ParentClass ? Blueprint->ParentClass->FindFunctionByName(FName(*Candidate, FNAME_Find)) : nullptr;
					if (Found && UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Found))
					{
						Event = Found;
						break;
					}
				}
				if (!Event)
					Throw(FString::Printf(TEXT("'%s' is not an overridable event of %s (e.g. BeginPlay, Tick, ActorBeginOverlap); for your own event use AddCustomEvent"),
						*Name, Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("the parent class")));
				// An actor Blueprint starts with disabled placeholder nodes for the common events: enabling one is adding it.
				UK2Node_Event* Node = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, Event->GetOwnerClass(), Event->GetFName());
				if (Node && Node->IsNodeEnabled() && !Node->IsAutomaticallyPlacedGhostNode())
					Throw(FString::Printf(TEXT("%s already has the event %s (%s#%lld)"), *Blueprint->GetName(), *Event->GetName(),
						*Node->GetClass()->GetName(), static_cast<long long>(FUnrealMcpRqlTypes::IdOf(Node))));
				if (Node)
				{
					const bool bGhost = Node->IsAutomaticallyPlacedGhostNode();
					Node->Modify();
					Node->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
					if (bGhost)
					{
						// The placeholder's "This node is disabled ..." comment goes with it, as connecting a pin does.
						Node->NodeComment.Empty();
						Node->bCommentBubbleVisible = false;
					}
					const FVector2D At = Place(Target, Args[1], Args[2]);
					if (Args[1].is_number() || Args[2].is_number())
					{
						Node->NodePosX = FMath::RoundToInt(At.X);
						Node->NodePosY = FMath::RoundToInt(At.Y);
					}
					Touched(Node, true);
					return Types->WrapObject(Node);
				}
				UClass* Owner = Event->GetOwnerClass();
				const FName EventName = Event->GetFName();
				return Types->WrapObject(CreateNode<UK2Node_Event>(Target, Place(Target, Args[1], Args[2]),
					[Owner, EventName](UK2Node_Event* Created)
					{
						Created->EventReference.SetExternalMember(EventName, Owner);
						Created->bOverrideFunction = true;
					}));
			}, "add (or enable) an overridable event of the parent class: \"BeginPlay\", \"Tick\", \"ActorBeginOverlap\"; returns the node"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddCustomEvent", WithPlace({ FUnrealMcpRqlTypes::Param("Name", String) }), NodeType,
			[Types, Graph](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraph* Target = Graph(Self);
				RequireEventGraph(Target);
				UBlueprint* Blueprint = RequireBlueprint(Target);
				const FString Name = TextArg(Args[0]).TrimStartAndEnd();
				RequireFreeName(Blueprint, Name, TEXT("event"));
				if (FBlueprintEditorUtils::FindCustomEventNode(Blueprint, FName(*Name)))
					Throw(FString::Printf(TEXT("%s already has a function or event '%s'"), *Blueprint->GetName(), *Name));
				const FName EventName(*Name);
				return Types->WrapObject(CreateNode<UK2Node_CustomEvent>(Target, Place(Target, Args[1], Args[2]),
					[EventName](UK2Node_CustomEvent* Node) { Node->CustomFunctionName = EventName; }));
			}, "add a custom event the Blueprint can call; returns the node"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddCast", WithPlace({ FUnrealMcpRqlTypes::Param("Class", String) }), NodeType,
			[Types, Graph](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraph* Target = Graph(Self);
				const FString ClassName = TextArg(Args[0]);
				UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassName);
				if (!Class)
					Throw(FString::Printf(TEXT("no class '%s'"), *ClassName));
				return Types->WrapObject(CreateNode<UK2Node_DynamicCast>(Target, Place(Target, Args[1], Args[2]),
					[Class](UK2Node_DynamicCast* Node) { Node->TargetType = Class; }));
			}, "add a Cast To <Class> node; returns the node"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddNode", WithPlace({ FUnrealMcpRqlTypes::Param("Kind", String) }), NodeType,
			[Types, Graph](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraph* Target = Graph(Self);
				const FString Kind = CompactName(TextArg(Args[0]));
				const FVector2D At = Place(Target, Args[1], Args[2]);
				if (Kind.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("IfThenElse"), ESearchCase::IgnoreCase))
					return Types->WrapObject(CreateNode<UK2Node_IfThenElse>(Target, At, [](UK2Node_IfThenElse*) {}));
				if (Kind.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
					return Types->WrapObject(CreateNode<UK2Node_ExecutionSequence>(Target, At, [](UK2Node_ExecutionSequence*) {}));
				if (Kind.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
					return Types->WrapObject(CreateNode<UK2Node_Self>(Target, At, [](UK2Node_Self*) {}));
				if (UEdGraph* Macro = FindMacro(Kind))
					return Types->WrapObject(CreateNode<UK2Node_MacroInstance>(Target, At, [Macro](UK2Node_MacroInstance* Node) { Node->SetMacroGraph(Macro); }));
				Throw(FString::Printf(TEXT("unknown node kind '%s' (Branch, Sequence, Self, or a standard macro: %s); calls: AddCallFunction"),
					*TextArg(Args[0]), *MacroNames()));
			}, "add a flow node: Branch, Sequence, Self, or a standard macro (ForLoop, ForEachLoop, WhileLoop, DoOnce, Gate, FlipFlop, IsValid, ...); returns the node"));
	}

	void NodeMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D, const rql::TypeDescriptor* PinType)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		const rql::TypeDescriptor* Int = rql::builtin::number_type(true, true, 4);
		const rql::TypeDescriptor* NodeType = T.ForClass(UEdGraphNode::StaticClass());
		auto Node = [](void* Self) { return static_cast<UEdGraphNode*>(Self); };

		FUnrealMcpRqlTypes::SetMember(D, Member("Title", String, [Node](void* Self) { return Str(NodeTitle(Node(Self))); },
			{}, "the title shown on the node"));
		FUnrealMcpRqlTypes::SetMember(D, Member("X", Int, [Node, Int](void* Self) { return rql::Value::number(Node(Self)->NodePosX, Int); },
			[Node](void* Self, const rql::Value& Value) { Node(Self)->NodePosX = FMath::RoundToInt(Value.as_number()); Touched(Node(Self), false); },
			"position in the graph"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Y", Int, [Node, Int](void* Self) { return rql::Value::number(Node(Self)->NodePosY, Int); },
			[Node](void* Self, const rql::Value& Value) { Node(Self)->NodePosY = FMath::RoundToInt(Value.as_number()); Touched(Node(Self), false); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("Comment", String, [Node](void* Self) { return Str(Node(Self)->NodeComment); },
			[Node](void* Self, const rql::Value& Value)
			{
				UEdGraphNode* Target = Node(Self);
				Target->NodeComment = TextArg(Value);
				Target->bCommentBubbleVisible = !Target->NodeComment.IsEmpty();
				Touched(Target, false);
			}, "the comment bubble"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Enabled", rql::builtin::bool_type(),
			[Node](void* Self) { return rql::Value::boolean(Node(Self)->IsNodeEnabled()); },
			[Node](void* Self, const rql::Value& Value)
			{
				Node(Self)->SetEnabledState(BoolArg(Value) ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled, /*bUserAction*/ true);
				Touched(Node(Self), true);
			}, "disabled nodes are left out of the compile"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Error", String, [Node](void* Self)
		{
			const UEdGraphNode* Target = Node(Self);
			return Target->bHasCompilerMessage ? Str(Target->ErrorMsg) : Str(FString());
		}, {}, "the last compile's message on this node (empty when none)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Graph", T.ForClass(UEdGraph::StaticClass()),
			[Types, Node](void* Self) { return Types->WrapObject(Node(Self)->GetGraph()); }, {}, "", false, true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Pins", T.Registry().list_type(PinType, false),
			[PinType, Node](void* Self) { return WrapPins(PinType, Node(Self)->Pins); }, {},
			"the visible pins (Name Direction Type DefaultValue LinkedTo)", /*bOwning*/ true));

		FUnrealMcpRqlTypes::AddMethod(D, Method("Connect",
			{ FUnrealMcpRqlTypes::Param("Pin", String), FUnrealMcpRqlTypes::Param("Target", NodeType), FUnrealMcpRqlTypes::Param("TargetPin", String) },
			nullptr, [Types, Node](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraphNode* Other = Cast<UEdGraphNode>(Types->AsObject(Args[1]));
				if (!Other)
					ThrowMismatch(TEXT("a graph node"), Args[1]);
				Connect(FindNodePin(Node(Self), TextArg(Args[0])), FindNodePin(Other, TextArg(Args[2])));
				return rql::Value::null();
			}, "connect one of this node's pins to a pin of another node (by pin name: \"then\", \"execute\", \"InString\")"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("Disconnect",
			{ FUnrealMcpRqlTypes::Param("Pin", String), FUnrealMcpRqlTypes::Param("Target", NodeType), FUnrealMcpRqlTypes::Param("TargetPin", String) },
			nullptr, [Types, Node](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraphNode* Other = Cast<UEdGraphNode>(Types->AsObject(Args[1]));
				if (!Other)
					ThrowMismatch(TEXT("a graph node"), Args[1]);
				Disconnect(FindNodePin(Node(Self), TextArg(Args[0])), FindNodePin(Other, TextArg(Args[2])));
				return rql::Value::null();
			}, "remove one connection"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("BreakLinks", { FUnrealMcpRqlTypes::OptionalParam("Pin", String, rql::Value::string("")) }, nullptr,
			[Node](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UEdGraphNode* Target = Node(Self);
				const FString Name = TextArg(Args[0]);
				if (!Name.IsEmpty())
				{
					BreakLinks(FindNodePin(Target, Name));
					return rql::Value::null();
				}
				Target->Modify();
				for (UEdGraphPin* Pin : Target->Pins)
				{
					for (UEdGraphPin* Other : Pin ? Pin->LinkedTo : TArray<UEdGraphPin*>())
					{
						if (Other && Other->GetOwningNode())
							Other->GetOwningNode()->Modify();
					}
				}
				Target->BreakAllNodeLinks();
				Touched(Target, false);
				return rql::Value::null();
			}, "remove the connections of one pin, or of every pin when none is named"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("SetPinDefault",
			{ FUnrealMcpRqlTypes::Param("Pin", String), FUnrealMcpRqlTypes::Param("Value", rql::builtin::any_type(), /*bNullable*/ true) },
			nullptr, [Types, Node](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				SetPinDefault(FindNodePin(Node(Self), TextArg(Args[0])), Args[1], *Types);
				return rql::Value::null();
			}, "set the default value of an unconnected input pin (text, number, bool, \"(1, 2, 3)\", an asset path for object pins)"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("Destroy", {}, nullptr, [Node](void* Self, const std::vector<rql::Value>&) -> rql::Value
		{
			UEdGraphNode* Target = Node(Self);
			if (!Target->CanUserDeleteNode())
				Throw(FString::Printf(TEXT("'%s' cannot be deleted (function entry / result nodes belong to their graph)"), *NodeTitle(Target)));
			UBlueprint* Blueprint = RequireBlueprint(Target);
			Target->GetGraph()->Modify();
			FBlueprintEditorUtils::RemoveNode(Blueprint, Target, /*bDontRecompile*/ true);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			return rql::Value::null();
		}, "remove the node and its connections"));
	}

	void PinMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		const rql::TypeDescriptor* Bool = rql::builtin::bool_type();
		const rql::TypeDescriptor* PinType = &D;
		auto Pin = [](void* Self) { return Handle(Self).Require(); };

		FUnrealMcpRqlTypes::SetMember(D, Member("Name", String, [Pin](void* Self) { return Str(Pin(Self)->PinName.ToString()); },
			{}, "the pin name (Connect / SetPinDefault take it)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("DisplayName", String, [Pin](void* Self) { return Str(Pin(Self)->GetDisplayName().ToString()); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("Direction", String, [Pin](void* Self)
		{
			return Str(Pin(Self)->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
		}, {}, "Input or Output"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Type", String, [Pin](void* Self)
		{
			return Str(UEdGraphSchema_K2::TypeToText(Pin(Self)->PinType).ToString());
		}, {}, "the pin type as the editor names it (\"Boolean\", \"Vector\", \"Actor Object Reference\", \"Exec\")"));
		FUnrealMcpRqlTypes::SetMember(D, Member("IsExec", Bool, [Pin](void* Self)
		{
			return rql::Value::boolean(Pin(Self)->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
		}, {}, "an execution (white) pin"));
		// Declared "any": a default can be given as text, a number, a bool or a struct form ("(1, 2, 3)"); it reads as text.
		FUnrealMcpRqlTypes::SetMember(D, Member("DefaultValue", rql::builtin::any_type(),
			[Pin](void* Self) { return Str(Pin(Self)->GetDefaultAsString()); },
			[Types, Pin](void* Self, const rql::Value& Value) { SetPinDefault(Pin(Self), Value, *Types); },
			"the default of an unconnected input pin (writable; object pins take an asset path)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("LinkedTo", T.Registry().list_type(PinType, false),
			[PinType, Pin](void* Self) { return WrapPins(PinType, Pin(Self)->LinkedTo); }, {}, "the pins this one is connected to"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Node", T.ForClass(UEdGraphNode::StaticClass()),
			[Types, Pin](void* Self) { return Types->WrapObject(Pin(Self)->GetOwningNode()); }, {}, "the node of the pin", false, true));
		D.display_name = "Name";
		D.display_explicit = true;

		FUnrealMcpRqlTypes::AddMethod(D, Method("Connect", { FUnrealMcpRqlTypes::Param("Other", PinType) }, nullptr,
			[PinType, Pin](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				Connect(Pin(Self), PinArg(Args[0], PinType));
				return rql::Value::null();
			}, "connect to another pin"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("Disconnect", { FUnrealMcpRqlTypes::Param("Other", PinType) }, nullptr,
			[PinType, Pin](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				Disconnect(Pin(Self), PinArg(Args[0], PinType));
				return rql::Value::null();
			}, "remove the connection to another pin"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("BreakLinks", {}, nullptr, [Pin](void* Self, const std::vector<rql::Value>&) -> rql::Value
		{
			BreakLinks(Pin(Self));
			return rql::Value::null();
		}, "remove every connection of the pin"));
	}
}

void UnrealMcpRqlAddBlueprints(FUnrealMcpRqlSession& Session)
{
	using namespace UnrealMcpRqlBlueprintsLocal;
	FUnrealMcpRqlTypes& T = Session.Types();
	FUnrealMcpRqlTypes* Types = &T;
	rql::TypeDescriptor& PinType = T.NativeObjectType(&PinTypeKey, TEXT("EdGraphPin"),
		TEXT("a pin of a Blueprint graph node: Name Direction Type DefaultValue LinkedTo Node; calls Connect / Disconnect / BreakLinks"));
	PinMembers(T, PinType);
	const rql::TypeDescriptor* Pin = &PinType;
	T.AddClassHook(UBlueprint::StaticClass(), &BlueprintMembers);
	T.AddClassHook(UEdGraph::StaticClass(), &GraphMembers);
	T.AddClassHook(UEdGraphNode::StaticClass(), [Pin](FUnrealMcpRqlTypes& Types, rql::TypeDescriptor& D) { NodeMembers(Types, D, Pin); });
	Session.AddSource(TEXT("blueprints"), T.ForClass(UBlueprint::StaticClass()), /*bList*/ true,
		TEXT("the project's Blueprints (Graphs, EventGraph, Status; call Compile / AddFunction); graphs list Nodes, nodes list Pins"),
		[Types]() { return UnrealMcpRqlAssetList(*Types, UBlueprint::StaticClass(), FString()); });
}
