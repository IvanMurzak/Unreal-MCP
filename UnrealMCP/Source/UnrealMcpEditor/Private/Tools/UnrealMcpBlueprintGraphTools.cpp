// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Tools/UnrealMcpBlueprintGraphTools.h"

#include "UnrealMcpLog.h"
#include "UnrealMcpPinTypes.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "InputCoreTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_InputKey.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

/**
 * Blueprint GRAPH authoring (docs/ARCHITECTURE.md §10): the read surface an agent needs before it can wire
 * anything (`blueprint-graph`), node creation at the level of the graph editor's context menu
 * (`blueprint-add-node`), and wiring through the K2 schema the editor itself uses
 * (`blueprint-connect-pins` / `blueprint-disconnect-pins`).
 *
 * The blueprint-add-function / blueprint-add-event stubs only mint empty graphs and event nodes; without a
 * connect tool the agent could not turn them into a running graph. Every edit here is the same public API path
 * the graph editor takes (FGraphNodeCreator + UEdGraphSchema_K2::CanCreateConnection/TryCreateConnection), and
 * runs ON the game thread (the §4 dispatcher guarantees it).
 *
 * Naming: a node is addressed by its ID (the UObject name, unique inside its graph, as returned by add-node) or
 * by the title the editor draws ("Print String"). An ambiguous title is REFUSED with its candidate ids — never
 * silently resolved to the first match, which is how a wire ends up on the wrong node.
 */
namespace
{
	// ---- resolution helpers ----------------------------------------------------------------------

	/** Resolve a UBlueprint by object path (in-memory first: created earlier this session and unsaved). */
	UBlueprint* BlueprintGraphResolveBlueprint(const FString& Path)
	{
		if (Path.IsEmpty())
			return nullptr;
		if (UBlueprint* Found = FindObject<UBlueprint>(nullptr, *Path))
			return Found;
		return LoadObject<UBlueprint>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** Matching key that ignores spaces, underscores, hyphens and case: "Print String" == "PrintString". */
	FString BlueprintGraphCompact(const FString& Text)
	{
		FString Out = Text;
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		return Out;
	}

	FString BlueprintGraphNodeTitle(const UEdGraphNode* Node)
	{
		return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString();
	}

	/** The node's ID: its UObject name, unique inside the graph and stable while the node lives. */
	FString BlueprintGraphNodeId(const UEdGraphNode* Node)
	{
		return Node ? Node->GetName() : FString();
	}

	/** "Print String (K2Node_CallFunction_3)" — one candidate in an error or a summary. */
	FString BlueprintGraphNodeLabel(const UEdGraphNode* Node)
	{
		return FString::Printf(TEXT("%s (%s)"), *BlueprintGraphNodeId(Node), *BlueprintGraphNodeTitle(Node));
	}

	bool BlueprintGraphIsExec(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	/** "then (out, exec), InString (in, String)" — the pins an agent may name, for an error or a tool result. */
	FString BlueprintGraphPinSummary(const UEdGraphNode* Node)
	{
		TArray<FString> Parts;
		if (Node)
		{
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->bHidden)
					continue;
				Parts.Add(FString::Printf(TEXT("%s (%s, %s)"), *Pin->PinName.ToString(),
					Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"), *UnrealMcpPinTypes::ToString(Pin->PinType)));
			}
		}
		return FString::Join(Parts, TEXT(", "));
	}

	/** Every graph of @p Blueprint an agent may edit: event graphs (ubergraph pages), functions, macros, delegates. */
	void BlueprintGraphCollectGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs)
	{
		for (const TArray<TObjectPtr<UEdGraph>>* List : { &Blueprint->UbergraphPages, &Blueprint->FunctionGraphs,
			&Blueprint->MacroGraphs, &Blueprint->DelegateSignatureGraphs })
		{
			for (UEdGraph* Graph : *List)
			{
				if (Graph)
					OutGraphs.Add(Graph);
			}
		}
	}

	FString BlueprintGraphGraphLabel(UEdGraph* Graph, const UBlueprint* Blueprint)
	{
		if (!Graph)
			return FString();
		const TCHAR* Kind = TEXT("graph");
		if (Blueprint && Blueprint->UbergraphPages.Contains(Graph))
			Kind = TEXT("event graph");
		else if (Blueprint && Blueprint->FunctionGraphs.Contains(Graph))
			Kind = TEXT("function");
		else if (Blueprint && Blueprint->MacroGraphs.Contains(Graph))
			Kind = TEXT("macro");
		else if (Blueprint && Blueprint->DelegateSignatureGraphs.Contains(Graph))
			Kind = TEXT("delegate signature");
		return FString::Printf(TEXT("'%s' (%s)"), *Graph->GetName(), Kind);
	}

	/** "EventGraph, MyFunc" — the graph names an agent may pass. */
	FString BlueprintGraphGraphNames(const TArray<UEdGraph*>& Graphs, const UBlueprint* Blueprint)
	{
		TArray<FString> Names;
		for (UEdGraph* Graph : Graphs)
			Names.Add(BlueprintGraphGraphLabel(Graph, Blueprint));
		return FString::Join(Names, TEXT(", "));
	}

	/**
	 * The graph @p Name names: empty takes the event graph (what most edits want), otherwise a graph name with or
	 * without the editor's spacing ("EventGraph" / "Event Graph", "MyFunc").
	 */
	UEdGraph* BlueprintGraphResolveGraph(UBlueprint* Blueprint, const FString& Name, FString& OutError)
	{
		TArray<UEdGraph*> Graphs;
		BlueprintGraphCollectGraphs(Blueprint, Graphs);

		const FString Wanted = Name.TrimStartAndEnd();
		if (Wanted.IsEmpty())
		{
			UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
			if (EventGraph)
				return EventGraph;
			OutError = FString::Printf(TEXT("'%s' has no event graph: pass 'graph' (it has: %s)."), *Blueprint->GetName(),
				Graphs.Num() ? *BlueprintGraphGraphNames(Graphs, Blueprint) : TEXT("no graphs"));
			return nullptr;
		}

		const bool bWantsEventGraph = BlueprintGraphCompact(Wanted).Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase)
			|| BlueprintGraphCompact(Wanted).Equals(TEXT("Ubergraph"), ESearchCase::IgnoreCase);
		if (bWantsEventGraph)
		{
			if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
				return EventGraph;
		}

		for (UEdGraph* Graph : Graphs)
		{
			if (Graph->GetName().Equals(Wanted, ESearchCase::IgnoreCase))
				return Graph;
		}
		const FString Compact = BlueprintGraphCompact(Wanted);
		for (UEdGraph* Graph : Graphs)
		{
			if (BlueprintGraphCompact(Graph->GetName()).Equals(Compact, ESearchCase::IgnoreCase))
				return Graph;
		}

		OutError = FString::Printf(TEXT("'%s' has no graph '%s' (graphs: %s)."), *Blueprint->GetName(), *Wanted,
			Graphs.Num() ? *BlueprintGraphGraphNames(Graphs, Blueprint) : TEXT("none"));
		return nullptr;
	}

	/**
	 * The node @p Ref names in @p Graph: its ID (object name), its NodeGuid, or the title the editor draws (case,
	 * spaces and underscores ignored). An ambiguous title is refused with the candidate IDs: guessing is how a wire
	 * lands on the wrong node.
	 */
	UEdGraphNode* BlueprintGraphFindNode(UEdGraph* Graph, const FString& Ref, FString& OutError)
	{
		const FString Wanted = Ref.TrimStartAndEnd();
		if (Wanted.IsEmpty())
		{
			OutError = TEXT("'node' is required: a node id (from blueprint-add-node / blueprint-graph) or its title.");
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
				continue;
			if (Node->GetName().Equals(Wanted, ESearchCase::IgnoreCase)
				|| Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).Equals(Wanted, ESearchCase::IgnoreCase)
				|| Node->NodeGuid.ToString(EGuidFormats::Digits).Equals(Wanted, ESearchCase::IgnoreCase))
				return Node;
		}

		TArray<UEdGraphNode*> Matches;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && BlueprintGraphNodeTitle(Node).Equals(Wanted, ESearchCase::IgnoreCase))
				Matches.Add(Node);
		}
		if (Matches.Num() == 0)
		{
			const FString Compact = BlueprintGraphCompact(Wanted);
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && BlueprintGraphCompact(BlueprintGraphNodeTitle(Node)).Equals(Compact, ESearchCase::IgnoreCase))
					Matches.Add(Node);
			}
		}
		if (Matches.Num() == 1)
			return Matches[0];
		if (Matches.Num() > 1)
		{
			TArray<FString> Candidates;
			for (UEdGraphNode* Node : Matches)
				Candidates.Add(BlueprintGraphNodeLabel(Node));
			OutError = FString::Printf(TEXT("'%s' matches %d nodes in graph '%s': %s — pass the node id instead (blueprint-graph lists them)."),
				*Wanted, Matches.Num(), *Graph->GetName(), *FString::Join(Candidates, TEXT(", ")));
			return nullptr;
		}

		TArray<FString> Available;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Available.Num() < 12)
				Available.Add(BlueprintGraphNodeLabel(Node));
		}
		OutError = FString::Printf(TEXT("no node '%s' in graph '%s' (nodes: %s%s)."), *Wanted, *Graph->GetName(),
			Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("none"),
			Graph->Nodes.Num() > Available.Num() ? TEXT(", ...") : TEXT(""));
		return nullptr;
	}

	/**
	 * The pin of @p Node that @p Name names: its pin name, then its display name (spacing ignored), then — for an
	 * empty name or "exec" / "then" / "execute" / "in" / "out" — the node's execution pin in @p Preferred direction.
	 * A node can carry the same name on an input and an output (a variable Set node does), so the preferred
	 * direction decides; anything not found lists the pins the node has.
	 */
	UEdGraphPin* BlueprintGraphFindPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Preferred, FString& OutError)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		TArray<UEdGraphPin*> Visible;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden)
				Visible.Add(Pin);
		}
		auto Prefer = [Preferred](TArray<UEdGraphPin*>& Candidates) -> UEdGraphPin*
		{
			if (Candidates.Num() <= 1)
				return Candidates.Num() ? Candidates[0] : nullptr;
			for (UEdGraphPin* Candidate : Candidates)
			{
				if (Candidate->Direction == Preferred)
					return Candidate;
			}
			return Candidates[0];
		};

		// 1) The pin name itself ("then", "execute", "InString" — the names blueprint-graph reports).
		TArray<UEdGraphPin*> Exact;
		for (UEdGraphPin* Pin : Visible)
		{
			if (Pin->PinName.ToString().Equals(Wanted, ESearchCase::IgnoreCase))
				Exact.Add(Pin);
		}
		if (UEdGraphPin* Found = Prefer(Exact))
			return Found;

		// 2) "exec" (or no name at all) means the flow pin in the wanted direction.
		const bool bExecAlias = Wanted.IsEmpty() || Wanted.Equals(TEXT("exec"), ESearchCase::IgnoreCase);
		if (bExecAlias)
		{
			TArray<UEdGraphPin*> ExecPins;
			for (UEdGraphPin* Pin : Visible)
			{
				if (BlueprintGraphIsExec(Pin))
					ExecPins.Add(Pin);
			}
			if (UEdGraphPin* Found = Prefer(ExecPins))
				return Found;
			OutError = FString::Printf(TEXT("'%s' has no execution pin (pins: %s)."), *BlueprintGraphNodeTitle(Node), *BlueprintGraphPinSummary(Node));
			return nullptr;
		}

		// 3) The display name the editor draws ("In String" for the pin "InString").
		const FString Compact = BlueprintGraphCompact(Wanted);
		TArray<UEdGraphPin*> Display;
		for (UEdGraphPin* Pin : Visible)
		{
			if (BlueprintGraphCompact(Pin->PinName.ToString()).Equals(Compact, ESearchCase::IgnoreCase)
				|| BlueprintGraphCompact(Pin->GetDisplayName().ToString()).Equals(Compact, ESearchCase::IgnoreCase))
				Display.Add(Pin);
		}
		if (UEdGraphPin* Found = Prefer(Display))
			return Found;

		OutError = FString::Printf(TEXT("'%s' has no pin '%s' (pins: %s)."), *BlueprintGraphNodeTitle(Node), *Wanted, *BlueprintGraphPinSummary(Node));
		return nullptr;
	}

	// ---- node creation ---------------------------------------------------------------------------

	/** @p Call's 'x'/'y' when given, else the graph's own free space (the editor's "place it here"). */
	FVector2D BlueprintGraphPlacement(UEdGraph* Graph, const FUnrealMcpToolCall& Call)
	{
		const FVector2D Good = Graph->GetGoodPlaceForNewNode();
		if (!Call.Has(TEXT("x")) && !Call.Has(TEXT("y")))
			return Good;
		return FVector2D(Call.Has(TEXT("x")) ? Call.GetNumber(TEXT("x"), Good.X) : Good.X,
			Call.Has(TEXT("y")) ? Call.GetNumber(TEXT("y"), Good.Y) : Good.Y);
	}

	/** Create a node of @p TNode in @p Graph; @p Configure must set it up before Finalize allocates its pins. */
	template <typename TNode>
	TNode* BlueprintGraphCreateNode(UEdGraph* Graph, UBlueprint* Blueprint, const FVector2D& At, TFunctionRef<void(TNode*)> Configure)
	{
		Graph->Modify();
		FGraphNodeCreator<TNode> Creator(*Graph);
		TNode* Node = Creator.CreateNode(/*bSelectNewNode*/ false);
		Node->NodePosX = FMath::RoundToInt(At.X);
		Node->NodePosY = FMath::RoundToInt(At.Y);
		Configure(Node);
		Creator.Finalize();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Node;
	}

	/** Event nodes only belong in the Blueprint's event graph (the editor refuses them anywhere else). */
	void BlueprintGraphRequireEventGraph(UBlueprint* Blueprint, UEdGraph* Graph, FString& OutError)
	{
		if (FBlueprintEditorUtils::FindEventGraph(Blueprint) == Graph)
			return;
		OutError = FString::Printf(TEXT("events live in the event graph, not in '%s': pass graph: \"EventGraph\"."), *Graph->GetName());
	}

	/** A Blueprint-callable @p Name on @p Class, tolerating the K2_ prefix and UE 4's float naming. */
	UFunction* BlueprintGraphFindCallable(UClass* Class, const FString& Name)
	{
		if (!Class)
			return nullptr;
		TArray<FString, TInlineAllocator<3>> Candidates{ Name, FString(TEXT("K2_")) + Name };
		const FString Double = Name.Replace(TEXT("Float"), TEXT("Double"), ESearchCase::CaseSensitive);
		if (Double != Name && Class->HasAnyClassFlags(CLASS_Native))
			Candidates.Add(Double);
		for (const FString& Candidate : Candidates)
		{
			UFunction* Function = Class->FindFunctionByName(FName(*Candidate, FNAME_Find));
			if (Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
				return Function;
		}
		return nullptr;
	}

	/** Classes a bare function name is looked up in after the Blueprint itself (the editor's most used libraries). */
	const TCHAR* const BlueprintGraphLibraryClasses[] = {
		TEXT("KismetSystemLibrary"), TEXT("KismetMathLibrary"), TEXT("KismetStringLibrary"), TEXT("KismetTextLibrary"),
		TEXT("KismetArrayLibrary"), TEXT("GameplayStatics"), TEXT("BlueprintMapLibrary"), TEXT("BlueprintSetLibrary"),
	};

	/**
	 * The function @p Spec names: "PrintString", "Class.Function" or a function of the Blueprint itself. A miss names
	 * the libraries that do declare it, so the agent can qualify the next call instead of guessing.
	 */
	UFunction* BlueprintGraphResolveFunction(UBlueprint* Blueprint, const FString& Spec, FString& OutError)
	{
		const FString Text = Spec.TrimStartAndEnd();
		if (Text.IsEmpty())
		{
			OutError = TEXT("'function' is required for kind 'call' (e.g. \"PrintString\" or \"KismetMathLibrary.Add_IntInt\").");
			return nullptr;
		}

		FString ClassPart, FunctionPart;
		if (Text.Split(TEXT(":"), &ClassPart, &FunctionPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
			|| Text.Split(TEXT("."), &ClassPart, &FunctionPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassPart);
			if (!Class)
			{
				OutError = FString::Printf(TEXT("no class '%s' (the class part of '%s')."), *ClassPart, *Text);
				return nullptr;
			}
			if (UFunction* Function = BlueprintGraphFindCallable(Class, FunctionPart))
				return Function;
			OutError = FString::Printf(TEXT("%s has no Blueprint-callable function '%s'."), *Class->GetName(), *FunctionPart);
			return nullptr;
		}

		UClass* Own = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
		if (UFunction* Function = BlueprintGraphFindCallable(Own, Text))
			return Function;
		for (const TCHAR* Library : BlueprintGraphLibraryClasses)
		{
			if (UFunction* Function = BlueprintGraphFindCallable(FUnrealMcpObjectRef::ResolveClass(Library), Text))
				return Function;
		}

		// Any other function library (a plugin's or the project's): collect the qualified homes.
		TArray<UClass*> Libraries;
		GetDerivedClasses(UBlueprintFunctionLibrary::StaticClass(), Libraries, /*bRecursive*/ true);
		TArray<FString> Homes;
		UFunction* Single = nullptr;
		int32 Count = 0;
		for (UClass* Library : Libraries)
		{
			if (!Library || Library->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				|| Library->GetName().StartsWith(TEXT("SKEL_")) || Library->GetName().StartsWith(TEXT("REINST_")))
				continue;
			UFunction* Function = BlueprintGraphFindCallable(Library, Text);
			if (!Function || Function->GetOwnerClass() != Library || Function->HasMetaData(TEXT("BlueprintInternalUseOnly")))
				continue;
			Single = Function;
			++Count;
			Homes.Add(Library->GetName() + TEXT(".") + Function->GetName());
		}
		if (Count == 1)
			return Single;
		if (Count > 1)
		{
			OutError = FString::Printf(TEXT("'%s' is on several function libraries: name one (%s)."), *Text, *FString::Join(Homes, TEXT(", ")));
			return nullptr;
		}

		OutError = FString::Printf(TEXT("no Blueprint-callable function '%s' on '%s' or any function library (qualify it: \"KismetMathLibrary.Add_IntInt\")."),
			*Text, *Blueprint->GetName());
		return nullptr;
	}

	/** A Blueprint-visible variable of @p Class by name, else by its authored name in any case. */
	FProperty* BlueprintGraphFindVariable(UClass* Class, const FString& Name)
	{
		if (!Class)
			return nullptr;
		FProperty* Property = FindFProperty<FProperty>(Class, FName(*Name, FNAME_Find));
		for (TFieldIterator<FProperty> It(Class); It && !Property; ++It)
		{
			if (It->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
				Property = *It;
		}
		return (Property && Property->HasAnyPropertyFlags(CPF_BlueprintVisible)) ? Property : nullptr;
	}

	/** The variable a Get / Set node reads: one of the Blueprint (Property null = self context), or of another class. */
	struct FBlueprintGraphVariable
	{
		FProperty* Property = nullptr;
		FName Name;
	};

	FBlueprintGraphVariable BlueprintGraphResolveVariable(UBlueprint* Blueprint, const FString& Spec, bool bWrite, FString& OutError)
	{
		FBlueprintGraphVariable Result;
		const FString Text = Spec.TrimStartAndEnd();
		if (Text.IsEmpty())
		{
			OutError = TEXT("'variable' is required (e.g. \"Health\", or \"BP_Player.Score\" for another class's variable).");
			return Result;
		}
		auto Accept = [bWrite, &OutError](FProperty* Property, FBlueprintGraphVariable& Out) -> bool
		{
			if (bWrite && Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
			{
				OutError = FString::Printf(TEXT("%s is read-only in Blueprints."), *Property->GetAuthoredName());
				return false;
			}
			Out.Property = Property;
			Out.Name = Property->GetFName();
			return true;
		};

		UClass* Own = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;

		FString ClassPart, VariablePart;
		if (!Text.Split(TEXT("."), &ClassPart, &VariablePart, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
			|| ClassPart.IsEmpty() || VariablePart.IsEmpty())
		{
			FProperty* Property = BlueprintGraphFindVariable(Own, Text);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("'%s' has no Blueprint variable '%s' (add it with blueprint-add-variable)."), *Blueprint->GetName(), *Text);
				return Result;
			}
			Accept(Property, Result);
			return Result;
		}

		UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassPart);
		if (!Class && !ClassPart.EndsWith(TEXT("_C")))
			Class = FUnrealMcpObjectRef::ResolveClass(ClassPart + TEXT("_C"));
		if (!Class)
		{
			OutError = FString::Printf(TEXT("no class '%s' (pass a class path for another Blueprint's variable, e.g. \"/Game/BP/BP_Player.Score\")."), *ClassPart);
			return Result;
		}
		if (const UBlueprint* Other = Cast<UBlueprint>(Class->ClassGeneratedBy))
		{
			if (Other->SkeletonGeneratedClass)
				Class = Other->SkeletonGeneratedClass; // variables added since the last compile too
		}
		if (Class == Own || (Own && Own->IsChildOf(Class)))
		{
			FProperty* Property = BlueprintGraphFindVariable(Own, VariablePart);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("'%s' has no Blueprint variable '%s'."), *Blueprint->GetName(), *VariablePart);
				return Result;
			}
			Accept(Property, Result);
			return Result;
		}
		FProperty* Property = BlueprintGraphFindVariable(Class, VariablePart);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("%s has no Blueprint variable '%s'."), *Class->GetName(), *VariablePart);
			return Result;
		}
		Accept(Property, Result);
		return Result;
	}

	void BlueprintGraphSetVariableReference(FMemberReference& Reference, const FBlueprintGraphVariable& Variable)
	{
		if (Variable.Property)
			Reference.SetFromField<FProperty>(Variable.Property, /*bIsConsideredSelfContext*/ false); // with its GUID: survives renames
		else
			Reference.SetSelfMember(Variable.Name);
	}

	UEdGraph* BlueprintGraphFindMacro(const FString& Name)
	{
		UBlueprint* Macros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
			nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Macros)
			return nullptr;
		const FString Compact = BlueprintGraphCompact(Name);
		for (UEdGraph* Graph : Macros->MacroGraphs)
		{
			if (Graph && (Graph->GetName().Equals(Name, ESearchCase::IgnoreCase)
				|| BlueprintGraphCompact(Graph->GetName()).Equals(Compact, ESearchCase::IgnoreCase)))
				return Graph;
		}
		return nullptr;
	}

	FString BlueprintGraphMacroNames()
	{
		TArray<FString> Names;
		if (UBlueprint* Macros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
			nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			for (UEdGraph* Graph : Macros->MacroGraphs)
			{
				if (Graph)
					Names.Add(Graph->GetName());
			}
		}
		return FString::Join(Names, TEXT(", "));
	}

	/** The findable overridable event of the parent class named @p Name ("BeginPlay" or "ReceiveBeginPlay"). */
	UFunction* BlueprintGraphResolveEvent(UBlueprint* Blueprint, const FString& Name)
	{
		if (!Blueprint->ParentClass)
			return nullptr;
		for (const FString& Candidate : { Name, FString(TEXT("Receive")) + Name, FString(TEXT("K2_")) + Name })
		{
			// FNAME_Find (not the newer FNAME_FIND spelling): the enum value has this case in every 5.x the
			// plugin supports, and a name that does not exist is exactly what a miss should be.
			UFunction* Found = Blueprint->ParentClass->FindFunctionByName(FName(*Candidate, FNAME_Find));
			if (Found && UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Found))
				return Found;
		}
		return nullptr;
	}

	// ---- JSON shapes -----------------------------------------------------------------------------

	/** A pin as blueprint-graph / blueprint-add-node report it; hidden pins (the auto-generated ones) are left out. */
	TSharedPtr<FJsonObject> BlueprintGraphPinJson(UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Out->SetStringField(TEXT("displayName"), Pin->GetDisplayName().ToString());
		Out->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
		Out->SetStringField(TEXT("type"), UnrealMcpPinTypes::ToString(Pin->PinType));
		if (BlueprintGraphIsExec(Pin))
			Out->SetBoolField(TEXT("exec"), true);
		if (Pin->Direction == EGPD_Input && !BlueprintGraphIsExec(Pin))
		{
			const FString Default = Pin->GetDefaultAsString();
			if (!Default.IsEmpty())
				Out->SetStringField(TEXT("default"), Default);
		}
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Links;
			for (UEdGraphPin* Other : Pin->LinkedTo)
			{
				if (!Other || !Other->GetOwningNodeUnchecked())
					continue;
				TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
				Link->SetStringField(TEXT("node"), Other->GetOwningNode()->GetName());
				Link->SetStringField(TEXT("pin"), Other->PinName.ToString());
				Links.Add(MakeShared<FJsonValueObject>(Link));
			}
			Out->SetArrayField(TEXT("links"), Links);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> BlueprintGraphNodeJson(UEdGraphNode* Node, bool bIncludePins)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("id"), BlueprintGraphNodeId(Node));
		Out->SetStringField(TEXT("title"), BlueprintGraphNodeTitle(Node));
		Out->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Out->SetNumberField(TEXT("x"), Node->NodePosX);
		Out->SetNumberField(TEXT("y"), Node->NodePosY);
		if (!Node->IsNodeEnabled())
			Out->SetBoolField(TEXT("enabled"), false);
		if (Node->bHasCompilerMessage && !Node->ErrorMsg.IsEmpty())
			Out->SetStringField(TEXT("error"), Node->ErrorMsg);
		if (!Node->NodeComment.IsEmpty())
			Out->SetStringField(TEXT("comment"), Node->NodeComment);
		if (bIncludePins)
		{
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && !Pin->bHidden)
					Pins.Add(MakeShared<FJsonValueObject>(BlueprintGraphPinJson(Pin)));
			}
			Out->SetArrayField(TEXT("pins"), Pins);
		}
		return Out;
	}

	// ---- pin preparation -------------------------------------------------------------------------

	/**
	 * The two pins a connect names, ordered output -> input. Arguments may come reversed (the caller named the input
	 * first); two pins of the same direction are refused, as the editor refuses the drag.
	 */
	bool BlueprintGraphResolveConnection(UEdGraphPin*& A, UEdGraphPin*& B, FString& OutError)
	{
		if (A->GetOwningNode() == B->GetOwningNode())
		{
			OutError = FString::Printf(TEXT("'%s'.%s and '%s'.%s are pins of the same node."), *BlueprintGraphNodeTitle(A->GetOwningNode()),
				*A->PinName.ToString(), *BlueprintGraphNodeTitle(B->GetOwningNode()), *B->PinName.ToString());
			return false;
		}
		if (A->Direction == B->Direction)
		{
			OutError = FString::Printf(TEXT("both pins are %s pins ('%s'.%s and '%s'.%s): a connection goes from an output to an input."),
				A->Direction == EGPD_Input ? TEXT("input") : TEXT("output"), *BlueprintGraphNodeTitle(A->GetOwningNode()),
				*A->PinName.ToString(), *BlueprintGraphNodeTitle(B->GetOwningNode()), *B->PinName.ToString());
			return false;
		}
		if (A->Direction == EGPD_Input)
			Swap(A, B);
		return true;
	}
}

namespace UnrealMcpBlueprintTools
{
	// ---- blueprint-graph -------------------------------------------------------------------------
	static void RegisterGraphRead(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-graph"))
			.Title(TEXT("Get Blueprint Graph"))
			.Description(TEXT("Read one graph of a Blueprint the way the graph editor shows it: every node with its id, "
			                  "title, class, position, enabled state, compile error and its visible pins (name, display name, "
			                  "direction, type, exec flag, default value, and the links to other nodes). This is the read "
			                  "surface for wiring — blueprint-add-node returns the ids to pass on, and blueprint-connect-pins "
			                  "takes the pin names reported here. 'graph' defaults to the event graph; pass a function or macro "
			                  "graph name for those. 'node' narrows the answer to one node (its id or its title)."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("graph"), TEXT("Graph name: \"EventGraph\" (default), a function graph, or a macro graph."))
			.ParamString(TEXT("node"), TEXT("Optional: only this node — its id (e.g. K2Node_CallFunction_3) or its title (e.g. \"Print String\")."))
			.ReadOnlyHint(true).DestructiveHint(false).IdempotentHint(true).OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = BlueprintGraphResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				FString GraphError;
				UEdGraph* Graph = BlueprintGraphResolveGraph(Blueprint, Call.GetString(TEXT("graph")), GraphError);
				if (!Graph)
					return FUnrealMcpToolResult::Error(GraphError);

				const FString NodeRef = Call.GetString(TEXT("node"));
				UEdGraphNode* Only = nullptr;
				if (!NodeRef.IsEmpty())
				{
					FString NodeError;
					Only = BlueprintGraphFindNode(Graph, NodeRef, NodeError);
					if (!Only)
						return FUnrealMcpToolResult::Error(NodeError);
				}

				TArray<TSharedPtr<FJsonValue>> Nodes;
				int32 LinkCount = 0;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node || (Only && Node != Only))
						continue;
					Nodes.Add(MakeShared<FJsonValueObject>(BlueprintGraphNodeJson(Node, /*bIncludePins*/ true)));
					for (UEdGraphPin* Pin : Node->Pins)
						LinkCount += (Pin && !Pin->bHidden) ? Pin->LinkedTo.Num() : 0;
				}

				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("name"), Graph->GetName());
				Out->SetStringField(TEXT("blueprint"), Blueprint->GetName());
				Out->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
				Out->SetArrayField(TEXT("nodes"), Nodes);

				const FString OnlyNote = Only ? FString::Printf(TEXT(" (showing '%s')"), *BlueprintGraphNodeLabel(Only)) : FString();
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("%s of '%s': %d node(s), %d link(s)%s."), *BlueprintGraphGraphLabel(Graph, Blueprint),
						*Blueprint->GetName(), Nodes.Num(), LinkCount, *OnlyNote),
					Out);
			});
	}

	// ---- blueprint-add-node ----------------------------------------------------------------------
	static void RegisterGraphAdd(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-add-node"))
			.Title(TEXT("Add Blueprint Graph Node"))
			.Description(TEXT("Add ONE node inside a Blueprint graph, at the level of the graph editor's context menu. "
			                  "'kind' picks it: call ('function': \"PrintString\", \"KismetMathLibrary.Add_IntInt\", or a function "
			                  "of this Blueprint), variable-get / variable-set ('variable': \"Health\", or \"BP_Player.Score\" for "
			                  "another class's variable, whose object then feeds the Target pin), event ('name': \"BeginPlay\" — "
			                  "enables the disabled placeholder a new Blueprint is seeded with), custom-event ('name'), branch, "
			                  "sequence, self, cast ('class'), macro ('name': ForEachLoop, DoOnce, Gate, ...), input-key "
			                  "('name': \"SpaceBar\").\n"
			                  "Returns the node's id, title and pins, so blueprint-connect-pins can wire it straight away. "
			                  "'x' / 'y' place it; omit them to let the graph pick free space. Nodes are not compiled or "
			                  "saved here: run blueprint-compile, then save with rql-mutate: editor | call SaveAsset(\"...\")."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("kind"), TEXT("Node kind: call, variable-get, variable-set, event, custom-event, branch, sequence, self, cast, macro, input-key."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("graph"), TEXT("Graph name (default: the event graph). Functions and macros take their own graph name."))
			.ParamString(TEXT("function"), TEXT("kind 'call': the Blueprint-callable function (\"PrintString\" or \"Class.Function\")."))
			.ParamString(TEXT("variable"), TEXT("kind 'variable-get' / 'variable-set': the variable (\"Health\" or \"BP_Player.Score\")."))
			.ParamString(TEXT("name"), TEXT("kind 'event' / 'custom-event' / 'macro' / 'input-key': the name to create (BeginPlay, MyEvent, ForEachLoop, SpaceBar)."))
			.ParamString(TEXT("class"), TEXT("kind 'cast': the class to cast to (path or short name)."))
			.ParamNumber(TEXT("x"), TEXT("Optional X position in the graph; omit for free space."))
			.ParamNumber(TEXT("y"), TEXT("Optional Y position in the graph; omit for free space."))
			.ReadOnlyHint(false).DestructiveHint(false).IdempotentHint(false).OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = BlueprintGraphResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				FString GraphError;
				UEdGraph* Graph = BlueprintGraphResolveGraph(Blueprint, Call.GetString(TEXT("graph")), GraphError);
				if (!Graph)
					return FUnrealMcpToolResult::Error(GraphError);
				if (!Cast<UEdGraphSchema_K2>(Graph->GetSchema()))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a Blueprint (K2) graph."), *Graph->GetName()));

				const FString Kind = BlueprintGraphCompact(Call.GetString(TEXT("kind"))).ToLower();
				if (Kind.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("'kind' is required (call, variable-get, variable-set, event, custom-event, branch, sequence, self, cast, macro, input-key)."));

				const FString Name = Call.GetString(TEXT("name")).TrimStartAndEnd();
				const FVector2D At = BlueprintGraphPlacement(Graph, Call);
				UEdGraphNode* Node = nullptr;

				if (Kind == TEXT("call"))
				{
					const FString Function = Call.GetString(TEXT("function")).IsEmpty() ? Name : Call.GetString(TEXT("function"));
					FString FunctionError;
					UFunction* Resolved = BlueprintGraphResolveFunction(Blueprint, Function, FunctionError);
					if (!Resolved)
						return FUnrealMcpToolResult::Error(FunctionError);
					Node = BlueprintGraphCreateNode<UK2Node_CallFunction>(Graph, Blueprint, At,
						[Resolved](UK2Node_CallFunction* Created) { Created->SetFromFunction(Resolved); });
				}
				else if (Kind == TEXT("variableget") || Kind == TEXT("get") || Kind == TEXT("variableset") || Kind == TEXT("set"))
				{
					const bool bWrite = Kind.Contains(TEXT("set"));
					const FString Variable = Call.GetString(TEXT("variable")).IsEmpty() ? Name : Call.GetString(TEXT("variable"));
					FString VariableError;
					const FBlueprintGraphVariable Resolved = BlueprintGraphResolveVariable(Blueprint, Variable, bWrite, VariableError);
					if (VariableError.Len() > 0)
						return FUnrealMcpToolResult::Error(VariableError);
					if (bWrite)
					{
						Node = BlueprintGraphCreateNode<UK2Node_VariableSet>(Graph, Blueprint, At,
							[Resolved](UK2Node_VariableSet* Created) { BlueprintGraphSetVariableReference(Created->VariableReference, Resolved); });
					}
					else
					{
						Node = BlueprintGraphCreateNode<UK2Node_VariableGet>(Graph, Blueprint, At,
							[Resolved](UK2Node_VariableGet* Created) { BlueprintGraphSetVariableReference(Created->VariableReference, Resolved); });
					}
				}
				else if (Kind == TEXT("event"))
				{
					FString EventGraphError;
					BlueprintGraphRequireEventGraph(Blueprint, Graph, EventGraphError);
					if (!EventGraphError.IsEmpty())
						return FUnrealMcpToolResult::Error(EventGraphError);
					if (Name.IsEmpty())
						return FUnrealMcpToolResult::Error(TEXT("'name' is required for kind 'event' (e.g. \"BeginPlay\")."));
					UFunction* Event = BlueprintGraphResolveEvent(Blueprint, Name);
					if (!Event)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not an overridable event of '%s' (e.g. BeginPlay, Tick, ActorBeginOverlap); "
							"for your own event use kind 'custom-event'."), *Name, Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("the parent class")));

					// A new Actor Blueprint carries DISABLED placeholder nodes for the common events: enabling one is adding it.
					UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, Event->GetOwnerClass(), Event->GetFName());
					if (Existing && Existing->IsNodeEnabled() && !Existing->IsAutomaticallyPlacedGhostNode())
					{
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' already has the event '%s' (%s) — connect to that node instead (blueprint-connect-pins)."),
							*Blueprint->GetName(), *Event->GetName(), *BlueprintGraphNodeLabel(Existing)));
					}
					if (Existing)
					{
						Existing->Modify();
						Existing->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
						Existing->NodeComment.Empty();
						Existing->bCommentBubbleVisible = false;
						if (Call.Has(TEXT("x")) || Call.Has(TEXT("y")))
						{
							Existing->NodePosX = FMath::RoundToInt(At.X);
							Existing->NodePosY = FMath::RoundToInt(At.Y);
						}
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
						Node = Existing;
					}
					else
					{
						const FName EventName = Event->GetFName();
						UClass* Owner = Event->GetOwnerClass();
						Node = BlueprintGraphCreateNode<UK2Node_Event>(Graph, Blueprint, At, [EventName, Owner](UK2Node_Event* Created)
						{
							Created->EventReference.SetExternalMember(EventName, Owner);
							Created->bOverrideFunction = true;
						});
					}
				}
				else if (Kind == TEXT("customevent"))
				{
					FString EventGraphError;
					BlueprintGraphRequireEventGraph(Blueprint, Graph, EventGraphError);
					if (!EventGraphError.IsEmpty())
						return FUnrealMcpToolResult::Error(EventGraphError);
					if (Name.IsEmpty() || !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS))
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a valid custom event name."), *Name));
					if (FBlueprintEditorUtils::FindCustomEventNode(Blueprint, FName(*Name)))
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' already has a custom event named '%s'."), *Blueprint->GetName(), *Name));
					const FName EventName(*Name);
					Node = BlueprintGraphCreateNode<UK2Node_CustomEvent>(Graph, Blueprint, At,
						[EventName](UK2Node_CustomEvent* Created) { Created->CustomFunctionName = EventName; });
				}
				else if (Kind == TEXT("cast"))
				{
					const FString ClassName = Call.GetString(TEXT("class")).IsEmpty() ? Name : Call.GetString(TEXT("class"));
					UClass* CastTo = FUnrealMcpObjectRef::ResolveClass(ClassName);
					if (!CastTo)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("no class '%s' to cast to (a class path or short name)."), *ClassName));
					Node = BlueprintGraphCreateNode<UK2Node_DynamicCast>(Graph, Blueprint, At,
						[CastTo](UK2Node_DynamicCast* Created) { Created->TargetType = CastTo; });
				}
				else if (Kind == TEXT("macro"))
				{
					if (Name.IsEmpty())
						return FUnrealMcpToolResult::Error(TEXT("'name' is required for kind 'macro' (e.g. \"ForEachLoop\")."));
					UEdGraph* Macro = BlueprintGraphFindMacro(Name);
					if (!Macro)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("no standard macro '%s' (macros: %s)."), *Name, *BlueprintGraphMacroNames()));
					Node = BlueprintGraphCreateNode<UK2Node_MacroInstance>(Graph, Blueprint, At,
						[Macro](UK2Node_MacroInstance* Created) { Created->SetMacroGraph(Macro); });
				}
				else if (Kind == TEXT("inputkey"))
				{
					FString EventGraphError;
					BlueprintGraphRequireEventGraph(Blueprint, Graph, EventGraphError);
					if (!EventGraphError.IsEmpty())
						return FUnrealMcpToolResult::Error(EventGraphError);
					const FKey Key{ FName(*Name) };
					if (!Key.IsValid())
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a key name (W, A, S, D, SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom, ...)."), *Name));
					if (Key.IsAnalog() || Key.IsDeprecated() || !Key.IsBindableInBlueprints())
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a key with Pressed / Released (an axis such as MouseX is read with GetInputAxisKeyValue)."), *Name));
					Node = BlueprintGraphCreateNode<UK2Node_InputKey>(Graph, Blueprint, At,
						[Key](UK2Node_InputKey* Created) { Created->InputKey = Key; });
				}
				else if (Kind == TEXT("branch") || Kind == TEXT("ifthenelse"))
				{
					Node = BlueprintGraphCreateNode<UK2Node_IfThenElse>(Graph, Blueprint, At, [](UK2Node_IfThenElse*) {});
				}
				else if (Kind == TEXT("sequence"))
				{
					Node = BlueprintGraphCreateNode<UK2Node_ExecutionSequence>(Graph, Blueprint, At, [](UK2Node_ExecutionSequence*) {});
				}
				else if (Kind == TEXT("self"))
				{
					Node = BlueprintGraphCreateNode<UK2Node_Self>(Graph, Blueprint, At, [](UK2Node_Self*) {});
				}
				else
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("unknown kind '%s' (call, variable-get, variable-set, event, custom-event, branch, sequence, self, cast, macro, input-key)."),
						*Call.GetString(TEXT("kind"))));
				}

				if (!Node)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("the K2 node creator made no %s node."), *Call.GetString(TEXT("kind"))));

				UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] blueprint-add-node: %s in %s of %s."),
					*BlueprintGraphNodeLabel(Node), *Graph->GetName(), *Blueprint->GetName());

				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("graph"), Graph->GetName());
				Out->SetObjectField(TEXT("node"), BlueprintGraphNodeJson(Node, /*bIncludePins*/ true));

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Added %s to graph '%s' of '%s'. Pins: %s. Wire it with blueprint-connect-pins(fromNode: \"%s\", ...)."),
						*BlueprintGraphNodeLabel(Node), *Graph->GetName(), *Blueprint->GetName(), *BlueprintGraphPinSummary(Node),
						*BlueprintGraphNodeId(Node)),
					Out);
			});
	}

	// ---- blueprint-connect-pins / blueprint-disconnect-pins --------------------------------------
	static void RegisterGraphWiring(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-connect-pins"))
			.Title(TEXT("Connect Blueprint Pins"))
			.Description(TEXT("Wire two pins of one Blueprint graph, exactly as dragging a wire in the graph editor does: the "
			                  "connection is checked and made by the K2 schema (UEdGraphSchema_K2::CanCreateConnection + "
			                  "TryCreateConnection), so type compatibility, exec-to-exec, single-link data inputs and the "
			                  "automatic conversion / cast nodes behave as in the editor, and a refusal comes back as the schema's "
			                  "own reason.\n"
			                  "Nodes are named by id (as returned by blueprint-add-node / blueprint-graph) or by the title the "
			                  "editor draws (\"Print String\"); a title that matches several nodes is REFUSED with the candidate "
			                  "ids, never guessed. Pins are named as blueprint-graph reports them (\"then\", \"execute\", "
			                  "\"InString\", ...); passing the input first is accepted and swapped. Run blueprint-compile "
			                  "afterwards."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("fromNode"), TEXT("Source node: its id (K2Node_CallFunction_3) or its title (\"Print String\")."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("fromPin"), TEXT("Source pin name (\"then\", \"ReturnValue\", ...)."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("toNode"), TEXT("Target node: its id or its title."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("toPin"), TEXT("Target pin name (\"execute\", \"InString\", \"Condition\", ...)."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("graph"), TEXT("Graph name (default: the event graph)."))
			.ReadOnlyHint(false).DestructiveHint(false).IdempotentHint(true).OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = BlueprintGraphResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				FString GraphError;
				UEdGraph* Graph = BlueprintGraphResolveGraph(Blueprint, Call.GetString(TEXT("graph")), GraphError);
				if (!Graph)
					return FUnrealMcpToolResult::Error(GraphError);
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
				if (!Schema)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a Blueprint (K2) graph."), *Graph->GetName()));

				FString NodeError;
				UEdGraphNode* FromNode = BlueprintGraphFindNode(Graph, Call.GetString(TEXT("fromNode")), NodeError);
				if (!FromNode)
					return FUnrealMcpToolResult::Error(NodeError);
				UEdGraphNode* ToNode = BlueprintGraphFindNode(Graph, Call.GetString(TEXT("toNode")), NodeError);
				if (!ToNode)
					return FUnrealMcpToolResult::Error(NodeError);

				FString PinError;
				UEdGraphPin* FromPin = BlueprintGraphFindPin(FromNode, Call.GetString(TEXT("fromPin")), EGPD_Output, PinError);
				if (!FromPin)
					return FUnrealMcpToolResult::Error(PinError);
				UEdGraphPin* ToPin = BlueprintGraphFindPin(ToNode, Call.GetString(TEXT("toPin")), EGPD_Input, PinError);
				if (!ToPin)
					return FUnrealMcpToolResult::Error(PinError);

				if (!BlueprintGraphResolveConnection(FromPin, ToPin, PinError))
					return FUnrealMcpToolResult::Error(PinError);

				const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
				if (Response.Response == CONNECT_RESPONSE_DISALLOW)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("cannot connect '%s'.%s (%s) to '%s'.%s (%s): %s"),
						*BlueprintGraphNodeTitle(FromPin->GetOwningNode()), *FromPin->PinName.ToString(), *UnrealMcpPinTypes::ToString(FromPin->PinType),
						*BlueprintGraphNodeTitle(ToPin->GetOwningNode()), *ToPin->PinName.ToString(), *UnrealMcpPinTypes::ToString(ToPin->PinType),
						*Response.Message.ToString()));
				}

				const int32 NodesBefore = Graph->Nodes.Num();
				const int32 FromLinksBefore = FromPin->LinkedTo.Num();
				const int32 ToLinksBefore = ToPin->LinkedTo.Num();
				const bool bAlreadyLinked = FromPin->LinkedTo.Contains(ToPin) || ToPin->LinkedTo.Contains(FromPin);

				FromNode->Modify();
				ToNode->Modify();
				if (!Schema->TryCreateConnection(FromPin, ToPin))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("the K2 schema refused to connect '%s'.%s to '%s'.%s."),
						*BlueprintGraphNodeTitle(FromNode), *FromPin->PinName.ToString(), *BlueprintGraphNodeTitle(ToNode), *ToPin->PinName.ToString()));
				}

				// The schema's bool is not proof: a conversion / macro-expansion path can report success without a link.
				// Accept a direct link, or the conversion node it inserted on both sides (which grows both link lists).
				const bool bLinked = FromPin->LinkedTo.Contains(ToPin) || ToPin->LinkedTo.Contains(FromPin)
					|| (FromPin->LinkedTo.Num() > FromLinksBefore && ToPin->LinkedTo.Num() > ToLinksBefore);
				if (!bLinked && !bAlreadyLinked)
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("connecting '%s'.%s to '%s'.%s changed nothing (the schema reported success); "
						"read the graph with blueprint-graph and check the pin directions and types."),
						*BlueprintGraphNodeTitle(FromNode), *FromPin->PinName.ToString(), *BlueprintGraphNodeTitle(ToNode), *ToPin->PinName.ToString()));
				}

				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

				const int32 ConversionNodes = FMath::Max(0, Graph->Nodes.Num() - NodesBefore);
				FString Note;
				if (bAlreadyLinked)
					Note = TEXT(" (it was already connected)");
				else if (ConversionNodes > 0)
					Note = FString::Printf(TEXT(" (a conversion node was inserted: %d node(s))"), ConversionNodes);

				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("graph"), Graph->GetName());
				TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
				Link->SetStringField(TEXT("fromNode"), BlueprintGraphNodeId(FromPin->GetOwningNode()));
				Link->SetStringField(TEXT("fromPin"), FromPin->PinName.ToString());
				Link->SetStringField(TEXT("toNode"), BlueprintGraphNodeId(ToPin->GetOwningNode()));
				Link->SetStringField(TEXT("toPin"), ToPin->PinName.ToString());
				Link->SetBoolField(TEXT("alreadyConnected"), bAlreadyLinked);
				Out->SetObjectField(TEXT("connection"), Link);
				if (ConversionNodes > 0)
					Out->SetNumberField(TEXT("conversionNodesAdded"), ConversionNodes);

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Connected '%s'.%s -> '%s'.%s in graph '%s'%s."),
						*BlueprintGraphNodeTitle(FromPin->GetOwningNode()), *FromPin->PinName.ToString(),
						*BlueprintGraphNodeTitle(ToPin->GetOwningNode()), *ToPin->PinName.ToString(), *Graph->GetName(), *Note),
					Out);
			});

		Registry.Tool(TEXT("blueprint-disconnect-pins"))
			.Title(TEXT("Disconnect Blueprint Pins"))
			.Description(TEXT("Remove wires in a Blueprint graph: with 'node' + 'pin' every connection of that pin (the "
			                  "editor's right-click \"Break all links\"), or with 'toNode' + 'toPin' only the one connection "
			                  "between the two pins. Node and pin naming is the same as blueprint-connect-pins. Run "
			                  "blueprint-compile afterwards."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("node"), TEXT("Node whose pin is unwired: its id or its title."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("pin"), TEXT("Pin name whose connections are removed (\"then\", \"InString\", ...)."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("toNode"), TEXT("Optional: only the link to this node is removed."))
			.ParamString(TEXT("toPin"), TEXT("Optional: the pin of 'toNode' (default: any pin of that node)."))
			.ParamString(TEXT("graph"), TEXT("Graph name (default: the event graph)."))
			.ReadOnlyHint(false).DestructiveHint(true).IdempotentHint(true).OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = BlueprintGraphResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				FString GraphError;
				UEdGraph* Graph = BlueprintGraphResolveGraph(Blueprint, Call.GetString(TEXT("graph")), GraphError);
				if (!Graph)
					return FUnrealMcpToolResult::Error(GraphError);
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
				if (!Schema)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a Blueprint (K2) graph."), *Graph->GetName()));

				FString NodeError;
				UEdGraphNode* Node = BlueprintGraphFindNode(Graph, Call.GetString(TEXT("node")), NodeError);
				if (!Node)
					return FUnrealMcpToolResult::Error(NodeError);

				FString PinError;
				UEdGraphPin* Pin = BlueprintGraphFindPin(Node, Call.GetString(TEXT("pin")), EGPD_Output, PinError);
				if (!Pin)
				{
					PinError.Empty();
					Pin = BlueprintGraphFindPin(Node, Call.GetString(TEXT("pin")), EGPD_Input, PinError);
					if (!Pin)
						return FUnrealMcpToolResult::Error(PinError);
				}

				const FString ToNodeRef = Call.GetString(TEXT("toNode"));
				if (ToNodeRef.IsEmpty())
				{
					const int32 Count = Pin->LinkedTo.Num();
					if (Count == 0)
						return FUnrealMcpToolResult::Success(FString::Printf(TEXT("'%s'.%s has no connections."),
							*BlueprintGraphNodeTitle(Node), *Pin->PinName.ToString()));

					Node->Modify();
					for (UEdGraphPin* Other : Pin->LinkedTo)
					{
						if (Other && Other->GetOwningNode())
							Other->GetOwningNode()->Modify();
					}
					Schema->BreakPinLinks(*Pin, /*bSendsNodeNotifcation*/ true);
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

					return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Removed %d connection(s) of '%s'.%s."),
						Count, *BlueprintGraphNodeTitle(Node), *Pin->PinName.ToString()));
				}

				UEdGraphNode* Other = BlueprintGraphFindNode(Graph, ToNodeRef, NodeError);
				if (!Other)
					return FUnrealMcpToolResult::Error(NodeError);

				UEdGraphPin* OtherPin = nullptr;
				const FString ToPinRef = Call.GetString(TEXT("toPin"));
				if (!ToPinRef.IsEmpty())
				{
					PinError.Empty();
					OtherPin = BlueprintGraphFindPin(Other, ToPinRef, EGPD_Input, PinError);
					if (!OtherPin)
					{
						PinError.Empty();
						OtherPin = BlueprintGraphFindPin(Other, ToPinRef, EGPD_Output, PinError);
						if (!OtherPin)
							return FUnrealMcpToolResult::Error(PinError);
					}
				}
				else
				{
					for (UEdGraphPin* Candidate : Pin->LinkedTo)
					{
						if (Candidate && Candidate->GetOwningNode() == Other)
						{
							OtherPin = Candidate;
							break;
						}
					}
					if (!OtherPin)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s'.%s is not connected to '%s'."),
							*BlueprintGraphNodeTitle(Node), *Pin->PinName.ToString(), *BlueprintGraphNodeTitle(Other)));
				}

				if (!Pin->LinkedTo.Contains(OtherPin) && !OtherPin->LinkedTo.Contains(Pin))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s'.%s is not connected to '%s'.%s."),
						*BlueprintGraphNodeTitle(Node), *Pin->PinName.ToString(), *BlueprintGraphNodeTitle(Other), *OtherPin->PinName.ToString()));

				Node->Modify();
				Other->Modify();
				Schema->BreakSinglePinLink(Pin, OtherPin);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Disconnected '%s'.%s from '%s'.%s."),
					*BlueprintGraphNodeTitle(Node), *Pin->PinName.ToString(), *BlueprintGraphNodeTitle(Other), *OtherPin->PinName.ToString()));
			});
	}

	void RegisterGraphTools(FUnrealMcpToolRegistry& Registry)
	{
		RegisterGraphRead(Registry);
		RegisterGraphAdd(Registry);
		RegisterGraphWiring(Registry);
	}
}
