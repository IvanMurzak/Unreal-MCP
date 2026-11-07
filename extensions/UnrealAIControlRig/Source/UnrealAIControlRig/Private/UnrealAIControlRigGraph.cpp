// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIControlRigGraph.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIControlRigUnits.h"

#include "ControlRigBlueprintLegacy.h"
#include "Dom/JsonObject.h"
#include "Rigs/RigHierarchy.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "UObject/UnrealType.h"

namespace UnrealAIControlRigGraphLocal
{
	/** Lower case, no spaces or underscores: "Target Space" and "TargetSpace" compare equal. */
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

	bool IsOutput(const URigVMPin* Pin)
	{
		return Pin->GetDirection() == ERigVMPinDirection::Output || Pin->GetDirection() == ERigVMPinDirection::IO;
	}

	bool IsInput(const URigVMPin* Pin)
	{
		return Pin->GetDirection() == ERigVMPinDirection::Input || Pin->GetDirection() == ERigVMPinDirection::IO
			|| Pin->GetDirection() == ERigVMPinDirection::Visible;
	}

	/** The node's pins for an error: "Item, Primary.{Weight, Axis, Target, Kind, Space}, Weight". */
	FString PinNames(const URigVMNode* Node)
	{
		TArray<FString> Names;
		for (const URigVMPin* Pin : Node->GetPins())
		{
			if (Pin->IsExecuteContext())
				continue;
			TArray<FString> Subs;
			if (!Pin->IsArray())
			{
				for (const URigVMPin* Sub : Pin->GetSubPins())
					Subs.Add(Sub->GetName());
			}
			Names.Add(Subs.Num() ? FString::Printf(TEXT("%s.{%s}"), *Pin->GetName(), *FString::Join(Subs, TEXT(", "))) : Pin->GetName());
		}
		return FString::Join(Names, TEXT(", "));
	}

	FString NodeLabel(const URigVMNode* Node)
	{
		const FString Title = Node->GetNodeTitle();
		return Title.IsEmpty() || Title == Node->GetName() ? Node->GetName() : FString::Printf(TEXT("%s (%s)"), *Node->GetName(), *Title);
	}

	FString PinType(const URigVMPin* Pin)
	{
		const UObject* Object = Pin->GetCPPTypeObject();
		return Object ? Object->GetName() : Pin->GetCPPType();
	}

	/** The rig variable named @p Name (case-insensitive); false when it has none. */
	bool FindVariable(UControlRigBlueprint* Rig, const FString& Name, FRigVMGraphVariableDescription& OutVariable)
	{
		for (const FRigVMGraphVariableDescription& Variable : Rig->GetMemberVariables())
		{
			if (Variable.Name.ToString().Equals(Name.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				OutVariable = Variable;
				return true;
			}
		}
		return false;
	}

	FString VariableNames(UControlRigBlueprint* Rig)
	{
		TArray<FString> Names;
		for (const FRigVMGraphVariableDescription& Variable : Rig->GetMemberVariables())
			Names.Add(Variable.Name.ToString());
		return Names.Num() ? FString::Join(Names, TEXT(", ")) : FString(TEXT("none yet"));
	}

	/** "Bone:head" -> (Bone, "head"); "head" -> the first match among bones, controls, nulls, curves, connectors. */
	FRigElementKey ElementFromText(UControlRigBlueprint* Rig, const FString& Text)
	{
		FString TypeText;
		FString Name = Text.TrimStartAndEnd().TrimQuotes();
		if (Name.Split(TEXT(":"), &TypeText, &Name))
		{
			const UEnum* Types = StaticEnum<ERigElementType>();
			const int64 Type = Types ? Types->GetValueByNameString(TypeText.TrimStartAndEnd()) : INDEX_NONE;
			if (Type == INDEX_NONE)
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not an element type (Bone, Control, Null, Curve)"), *TypeText));
				return FRigElementKey();
			}
			return UnrealAIControlRigGraph::Element(Rig, Name, { static_cast<ERigElementType>(Type) }, *TypeText.ToLower());
		}
		return UnrealAIControlRigGraph::Element(Rig, Name,
			{ ERigElementType::Bone, ERigElementType::Control, ERigElementType::Null, ERigElementType::Curve, ERigElementType::Connector }, TEXT("element"));
	}

	bool Link(URigVMController* Controller, URigVMPin* From, URigVMPin* To)
	{
		if (Controller->AddLink(From->GetPinPath(), To->GetPinPath(), /*bSetupUndoRedo*/ true, /*bPrintPythonCommand*/ false,
			ERigVMPinDirection::Output, /*bCreateCastNode*/ true))
			return true;
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not link %s (%s) to %s (%s)"), *From->GetPinPath(), *PinType(From), *To->GetPinPath(), *PinType(To)));
		return false;
	}

	/** The rig unit's struct name without its prefix, or what a non-unit node is. */
	FString UnitName(const URigVMNode* Node)
	{
		if (const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node))
		{
			const FString Struct = Unit->GetScriptStruct() ? Unit->GetScriptStruct()->GetName() : FString();
			return Struct.Replace(TEXT("RigUnit_"), TEXT("")).Replace(TEXT("RigVMFunction_"), TEXT(""));
		}
		if (const URigVMVariableNode* Variable = Cast<URigVMVariableNode>(Node))
			return FString::Printf(TEXT("%s %s"), Variable->IsGetter() ? TEXT("Get") : TEXT("Set"), *Variable->GetVariableName().ToString());
		return Node->GetClass()->GetName().Replace(TEXT("RigVM"), TEXT("")).Replace(TEXT("Node"), TEXT(""));
	}
}

using namespace UnrealAIControlRigGraphLocal;

FRigElementKey UnrealAIControlRigGraph::Element(UControlRigBlueprint* Rig, const FString& Name, std::initializer_list<ERigElementType> Types, const TCHAR* What)
{
	const URigHierarchy* Hierarchy = Rig->GetHierarchy();
	const FName Wanted(*Name.TrimStartAndEnd());
	for (const ERigElementType Type : Types)
	{
		const FRigElementKey Key(Wanted, Type);
		if (Hierarchy && Hierarchy->Contains(Key))
			return Key;
	}
	const int32 Bones = Hierarchy ? Hierarchy->Num(ERigElementType::Bone) : 0;
	UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no %s named '%s'%s"), *Rig->GetName(), What, *Name,
		Bones == 0 ? TEXT(" (it has no bones: create the rig from the character's mesh, CreateRig(path, mesh), to import its skeleton)")
			: TEXT(" (see its Bones / Controls members)")));
	return FRigElementKey();
}

FString UnrealAIControlRigGraph::KeyText(const FRigElementKey& Key)
{
	FString Text;
	FRigElementKey::StaticStruct()->ExportText(Text, &Key, nullptr, nullptr, PPF_None, nullptr);
	return Text;
}

URigVMController* UnrealAIControlRigGraph::Controller(UControlRigBlueprint* Rig)
{
	URigVMController* Controller = Rig->GetController();
	if (!Controller || !Controller->GetGraph())
		return UnrealMcpRql::Failed<URigVMController*>(FString::Printf(TEXT("%s has no rig graph to edit"), *Rig->GetName()));
	return Controller;
}

URigVMNode* UnrealAIControlRigGraph::FindNode(UControlRigBlueprint* Rig, const FString& Name)
{
	const URigVMGraph* Graph = Rig->GetModel();
	if (!Graph)
		return UnrealMcpRql::Failed<URigVMNode*>(FString::Printf(TEXT("%s has no rig graph"), *Rig->GetName()));
	const FString Wanted = Name.TrimStartAndEnd();
	TArray<URigVMNode*> Titled;
	for (URigVMNode* Node : Graph->GetNodes())
	{
		if (Node->GetName().Equals(Wanted, ESearchCase::IgnoreCase))
			return Node;
		if (Squash(Node->GetNodeTitle()) == Squash(Wanted))
			Titled.Add(Node);
	}
	if (Titled.Num() == 1)
		return Titled[0];
	TArray<FString> Names;
	for (const URigVMNode* Node : Titled.Num() ? Titled : Graph->GetNodes())
	{
		if (Names.Num() < 40)
			Names.Add(NodeLabel(Node));
	}
	return UnrealMcpRql::Failed<URigVMNode*>(FString::Printf(TEXT("%s: %s '%s' (%s: %s)"), *Rig->GetName(),
		Titled.Num() ? TEXT("several nodes are titled") : TEXT("no node named"), *Name, Titled.Num() ? TEXT("pass one name") : TEXT("nodes"),
		Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none")));
}

URigVMPin* UnrealAIControlRigGraph::FindPin(UControlRigBlueprint* Rig, const FString& Path)
{
	FString NodeName;
	FString PinPath;
	if (!Path.TrimStartAndEnd().Split(TEXT("."), &NodeName, &PinPath))
		return UnrealMcpRql::Failed<URigVMPin*>(FString::Printf(TEXT("'%s' is not a pin path such as Aim.Weight (Node.Pin)"), *Path));
	URigVMNode* Node = FindNode(Rig, NodeName);
	if (!Node)
		return nullptr;
	if (URigVMPin* Exact = Node->FindPin(PinPath))
		return Exact;
	// By name or display name, segment by segment ("Primary.Target Space").
	URigVMPin* Found = nullptr;
	TArray<FString> Segments;
	PinPath.ParseIntoArray(Segments, TEXT("."));
	const TArray<URigVMPin*>* Candidates = &Node->GetPins();
	for (const FString& Segment : Segments)
	{
		Found = nullptr;
		for (URigVMPin* Pin : *Candidates)
		{
			if (Squash(Pin->GetName()) == Squash(Segment) || Squash(Pin->GetDisplayName().ToString()) == Squash(Segment))
			{
				Found = Pin;
				break;
			}
		}
		if (!Found)
			break;
		Candidates = &Found->GetSubPins();
	}
	return Found ? Found : UnrealMcpRql::Failed<URigVMPin*>(FString::Printf(TEXT("%s has no pin '%s' (pins: %s)"), *NodeLabel(Node), *PinPath, *PinNames(Node)));
}

URigVMNode* UnrealAIControlRigGraph::ForwardsSolve(URigVMController* Controller)
{
	for (URigVMNode* Node : Controller->GetGraph()->GetNodes())
	{
		const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node);
		if (Unit && Unit->GetScriptStruct() == FRigUnit_BeginExecution::StaticStruct())
			return Node;
	}
	return Controller->AddUnitNode(FRigUnit_BeginExecution::StaticStruct(), TEXT("Execute"), FVector2D(-400.0, 0.0), TEXT("BeginExecution"));
}

URigVMPin* UnrealAIControlRigGraph::ExecuteOutput(URigVMNode* Node)
{
	URigVMPin* Found = nullptr;
	for (URigVMPin* Pin : Node->GetPins())
	{
		if (Pin->IsExecuteContext() && IsOutput(Pin))
		{
			if (Found)
				return nullptr;
			Found = Pin;
		}
	}
	return Found;
}

URigVMPin* UnrealAIControlRigGraph::ExecuteInput(URigVMNode* Node)
{
	for (URigVMPin* Pin : Node->GetPins())
	{
		if (Pin->IsExecuteContext() && (Pin->GetDirection() == ERigVMPinDirection::Input || Pin->GetDirection() == ERigVMPinDirection::IO))
			return Pin;
	}
	return nullptr;
}

URigVMPin* UnrealAIControlRigGraph::EndOfChain(URigVMNode* Start)
{
	URigVMPin* Exec = ExecuteOutput(Start);
	for (int32 Guard = 0; Exec && Guard < 1000; ++Guard)
	{
		const TArray<URigVMPin*> Targets = Exec->GetLinkedTargetPins();
		if (Targets.Num() == 0)
			return Exec;
		Exec = ExecuteOutput(Targets[0]->GetNode());
	}
	return nullptr;
}

FString UnrealAIControlRigGraph::ValueText(const FString& Value, const FString& CPPType, UObject* TypeObject)
{
	const FString Trimmed = Value.TrimStartAndEnd();
	if (CPPType == TEXT("bool"))
	{
		if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase))
			return TEXT("True");
		if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase))
			return TEXT("False");
	}
	// "(0, 1, 0)" / "[0, 1, 0]" / "0.5" on a struct of numbers (vectors, rotators as Pitch, Yaw, Roll, colours as R, G, B, A, quats).
	const UScriptStruct* Struct = Cast<UScriptStruct>(TypeObject);
	if (!Struct || Trimmed.Contains(TEXT("=")))
		return Trimmed;
	FString Inner = Trimmed;
	if ((Inner.StartsWith(TEXT("(")) && Inner.EndsWith(TEXT(")"))) || (Inner.StartsWith(TEXT("[")) && Inner.EndsWith(TEXT("]"))))
		Inner = Inner.Mid(1, Inner.Len() - 2);
	TArray<FString> Numbers;
	Inner.ParseIntoArray(Numbers, TEXT(","));
	// Only a struct made of numbers alone (not a transform, not a struct with names or keys).
	TArray<const FNumericProperty*> Fields;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FNumericProperty* Number = CastField<FNumericProperty>(*It);
		if (!Number)
			return Trimmed;
		Fields.Add(Number);
	}
	if (Fields.Num() == 0 || (Numbers.Num() != 1 && Numbers.Num() != Fields.Num()))
		return Trimmed;
	TArray<FString> Parts;
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		const FString Number = Numbers[Numbers.Num() == 1 ? 0 : Index].TrimStartAndEnd();
		if (!FCString::IsNumeric(*Number))
			return Trimmed;
		Parts.Add(FString::Printf(TEXT("%s=%s"), *Fields[Index]->GetName(), *Number));
	}
	return TEXT("(") + FString::Join(Parts, TEXT(",")) + TEXT(")");
}

bool UnrealAIControlRigGraph::SetPin(UControlRigBlueprint* Rig, URigVMPin* Pin, const FString& Value, TArray<URigVMNode*>& OutAdded, FString& OutValue)
{
	URigVMController* Controller = UnrealAIControlRigGraph::Controller(Rig);
	if (!Controller)
		return false;
	if (!IsInput(Pin) || Pin->IsExecuteContext())
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is %s: set an input pin (or link this one into an input: SetPin(rig, \"<input>\", \"@%s\"))"),
			*Pin->GetPinPath(), Pin->IsExecuteContext() ? TEXT("an execute pin") : TEXT("an output"), *Pin->GetPinPath()));
	const FString Trimmed = Value.TrimStartAndEnd();
	if (Trimmed.StartsWith(TEXT("@")))
	{
		const FString Source = Trimmed.RightChop(1).TrimStartAndEnd();
		if (Source.Contains(TEXT(".")))
		{
			URigVMPin* From = FindPin(Rig, Source);
			if (!From)
				return false;
			if (!IsOutput(From) || From->IsExecuteContext())
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not an output to link from"), *From->GetPinPath()));
			if (!Link(Controller, From, Pin))
				return false;
			OutValue = TEXT("@") + From->GetPinPath();
			return true;
		}
		FRigVMGraphVariableDescription Variable;
		if (!FindVariable(Rig, Source, Variable))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no variable '%s' (variables: %s; add one with AddVariable)"),
				*Rig->GetName(), *Source, *VariableNames(Rig)));
		const FVector2D At = Pin->GetNode()->GetPosition() + FVector2D(-320.0, 160.0 + 60.0 * OutAdded.Num());
		URigVMVariableNode* Getter = Controller->AddVariableNode(Variable.Name, Variable.CPPType, Variable.CPPTypeObject, /*bIsGetter*/ true, FString(), At);
		if (!Getter)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("could not add a getter of the variable %s"), *Variable.Name.ToString()));
		OutAdded.Add(Getter);
		URigVMPin* GetterValue = Getter->FindPin(TEXT("Value")); // URigVMVariableNode::ValueName is private
		if (!GetterValue || !Link(Controller, GetterValue, Pin))
			return false;
		OutValue = TEXT("@") + Variable.Name.ToString();
		return true;
	}

	FString Text;
	if (Pin->GetCPPTypeObject() == FRigElementKey::StaticStruct() && !Trimmed.StartsWith(TEXT("(")))
	{
		const FRigElementKey Key = Trimmed.IsEmpty() ? FRigElementKey() : ElementFromText(Rig, Trimmed);
		if (!Trimmed.IsEmpty() && !Key.IsValid())
			return false;
		Text = KeyText(Key);
	}
	else
	{
		Text = ValueText(Trimmed, Pin->GetCPPType(), Pin->GetCPPTypeObject());
	}
	if (Pin->GetLinkedSourcePins().Num())
		Controller->BreakAllLinks(Pin->GetPinPath(), /*bAsInput*/ true);
	const FString Before = Pin->GetDefaultValue();
	if (!Controller->SetPinDefaultValue(Pin->GetPinPath(), Text, /*bResizeArrays*/ true, /*bSetupUndoRedo*/ true))
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("could not set %s (%s) to '%s'; its text form is like %s"),
			*Pin->GetPinPath(), *PinType(Pin), *Value, Before.IsEmpty() ? TEXT("the editor's Copy of the pin") : *Before));
	OutValue = Pin->GetDefaultValue();
	return true;
}

bool UnrealAIControlRigGraph::ParseSettings(const FString& Settings, TArray<TPair<FString, FString>>& OutPairs)
{
	TArray<FString> Parts;
	FString Current;
	int32 Depth = 0;
	bool bQuoted = false;
	for (const TCHAR Char : Settings)
	{
		if (Char == TEXT('"'))
			bQuoted = !bQuoted;
		else if (!bQuoted && (Char == TEXT('(') || Char == TEXT('[') || Char == TEXT('{')))
			++Depth;
		else if (!bQuoted && (Char == TEXT(')') || Char == TEXT(']') || Char == TEXT('}')))
			--Depth;
		if (Char == TEXT(',') && Depth == 0 && !bQuoted)
		{
			Parts.Add(Current);
			Current.Reset();
			continue;
		}
		Current.AppendChar(Char);
	}
	Parts.Add(Current);
	for (const FString& Part : Parts)
	{
		if (Part.TrimStartAndEnd().IsEmpty())
			continue;
		int32 Equals = INDEX_NONE;
		Depth = 0;
		for (int32 Index = 0; Index < Part.Len() && Equals == INDEX_NONE; ++Index)
		{
			const TCHAR Char = Part[Index];
			Depth += (Char == TEXT('(') || Char == TEXT('[')) ? 1 : ((Char == TEXT(')') || Char == TEXT(']')) ? -1 : 0);
			if (Char == TEXT('=') && Depth == 0)
				Equals = Index;
		}
		const FString Key = Equals == INDEX_NONE ? FString() : Part.Left(Equals).TrimStartAndEnd();
		if (Key.IsEmpty())
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("'%s' is not a Pin=Value setting (e.g. \"Item=head, Weight=@LookAtWeight\")"), *Part.TrimStartAndEnd()));
		FString Value = Part.Mid(Equals + 1).TrimStartAndEnd();
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
			Value = Value.Mid(1, Value.Len() - 2);
		OutPairs.Emplace(Key, Value);
	}
	return true;
}

void UnrealAIControlRigGraph::RemoveNodes(URigVMController* Controller, const TArray<URigVMNode*>& Nodes)
{
	for (int32 Index = Nodes.Num() - 1; Index >= 0; --Index)
	{
		if (Nodes[Index] && Nodes[Index]->GetGraph())
			Controller->RemoveNode(Nodes[Index]);
	}
}

URigVMNode* UnrealAIControlRigGraph::AddUnit(UControlRigBlueprint* Rig, UScriptStruct* Unit, const FString& Settings, const FString& After, const FString& Name)
{
	URigVMController* Controller = UnrealAIControlRigGraph::Controller(Rig);
	TArray<TPair<FString, FString>> Pairs;
	if (!Controller || !ParseSettings(Settings, Pairs))
		return nullptr;
	const FString NodeName = Name.TrimStartAndEnd();
	if (!NodeName.IsEmpty() && Controller->GetGraph()->FindNodeByName(*NodeName))
		return UnrealMcpRql::Failed<URigVMNode*>(FString::Printf(TEXT("%s already has a node named '%s'"), *Rig->GetName(), *NodeName));

	// Where it runs: after the node (or execute pin) After names, else at the end of Forwards Solve. Resolved before any change.
	URigVMPin* RunsAfter = nullptr;
	const FString AfterText = After.TrimStartAndEnd();
	bool bMutable = false;
	for (TFieldIterator<FStructProperty> It(Unit); It; ++It)
		bMutable |= It->Struct && It->Struct->IsChildOf(FRigVMExecutePin::StaticStruct());
	if (bMutable)
	{
		if (AfterText.IsEmpty())
		{
			URigVMNode* Solve = ForwardsSolve(Controller);
			RunsAfter = Solve ? EndOfChain(Solve) : nullptr;
			if (!RunsAfter)
				return UnrealMcpRql::Failed<URigVMNode*>(TEXT("Forwards Solve branches (a Sequence or branch node): pass After, the node (or Sequence.Pin) the unit runs after (see the Nodes member)"));
		}
		else if (AfterText.Contains(TEXT(".")))
		{
			RunsAfter = FindPin(Rig, AfterText);
			if (RunsAfter && (!RunsAfter->IsExecuteContext() || !IsOutput(RunsAfter)))
				return UnrealMcpRql::Failed<URigVMNode*>(FString::Printf(TEXT("%s is not an execute output"), *RunsAfter->GetPinPath()));
		}
		else if (URigVMNode* Previous = FindNode(Rig, AfterText))
		{
			RunsAfter = ExecuteOutput(Previous);
			if (!RunsAfter)
			{
				TArray<FString> Outputs;
				for (const URigVMPin* Pin : Previous->GetPins())
				{
					if (Pin->IsExecuteContext() && IsOutput(Pin))
						Outputs.Add(Pin->GetPinPath());
				}
				return UnrealMcpRql::Failed<URigVMNode*>(Outputs.Num()
					? FString::Printf(TEXT("%s has several execute outputs: pass After as one of %s"), *NodeLabel(Previous), *FString::Join(Outputs, TEXT(", ")))
					: FString::Printf(TEXT("%s does not run in the execution chain (a pure node)"), *NodeLabel(Previous)));
			}
		}
		if (!RunsAfter)
			return nullptr;
	}
	else if (!AfterText.IsEmpty())
	{
		return UnrealMcpRql::Failed<URigVMNode*>(FString::Printf(TEXT("%s is a pure value node (no execution): leave After empty and link its outputs"), *Unit->GetName()));
	}

	const URigVMNode* Anchor = RunsAfter ? RunsAfter->GetNode() : nullptr;
	FVector2D At = Anchor ? Anchor->GetPosition() + FVector2D(380.0, 0.0) : FVector2D(0.0, 400.0);
	if (!bMutable)
	{
		URigVMNode* Solve = ForwardsSolve(Controller);
		URigVMPin* End = Solve ? EndOfChain(Solve) : nullptr;
		At = (End ? End->GetNode()->GetPosition() : FVector2D::ZeroVector) + FVector2D(0.0, 320.0 + 40.0 * (Controller->GetGraph()->GetNodes().Num() % 6));
	}
	URigVMNode* Node = Controller->AddUnitNode(Unit, TEXT("Execute"), At, NodeName);
	if (!Node)
		return UnrealMcpRql::Failed<URigVMNode*>(FString::Printf(TEXT("the rig graph refused a %s node (see the editor log)"), *Unit->GetName()));
	TArray<URigVMNode*> Added = { Node };
	for (const TPair<FString, FString>& Pair : Pairs)
	{
		URigVMPin* Pin = FindPin(Rig, Node->GetName() + TEXT(".") + Pair.Key);
		FString Ignored;
		if (!Pin || !SetPin(Rig, Pin, Pair.Value, Added, Ignored))
		{
			RemoveNodes(Controller, Added);
			return nullptr;
		}
	}
	if (RunsAfter)
	{
		URigVMPin* In = ExecuteInput(Node);
		URigVMPin* Out = ExecuteOutput(Node);
		const TArray<URigVMPin*> Next = RunsAfter->GetLinkedTargetPins();
		bool bLinked = In && Out;
		for (URigVMPin* Target : Next)
			bLinked = bLinked && Controller->BreakLink(RunsAfter->GetPinPath(), Target->GetPinPath());
		bLinked = bLinked && Link(Controller, RunsAfter, In);
		for (URigVMPin* Target : Next)
			bLinked = bLinked && Link(Controller, Out, Target);
		if (!bLinked)
		{
			RemoveNodes(Controller, Added);
			for (URigVMPin* Target : Next)
				Controller->AddLink(RunsAfter->GetPinPath(), Target->GetPinPath());
			return UnrealMcpRql::Failed<URigVMNode*>(FString::Printf(TEXT("could not link %s into the execution chain after %s"), *Unit->GetName(), *RunsAfter->GetPinPath()));
		}
	}
	return Node;
}

FString UnrealAIControlRigGraph::VariableType(const FString& Type)
{
	FString Wanted = Type.TrimStartAndEnd();
	if (Wanted.StartsWith(TEXT("Array<"), ESearchCase::IgnoreCase) && Wanted.EndsWith(TEXT(">")))
	{
		const FString Inner = VariableType(Wanted.Mid(6, Wanted.Len() - 7));
		return Inner.IsEmpty() ? FString() : TEXT("TArray<") + Inner + TEXT(">");
	}
	static const TMap<FString, FString> Scalars = {
		{ TEXT("bool"), TEXT("bool") }, { TEXT("boolean"), TEXT("bool") }, { TEXT("int"), TEXT("int32") }, { TEXT("int32"), TEXT("int32") },
		{ TEXT("integer"), TEXT("int32") }, { TEXT("float"), TEXT("float") }, { TEXT("real"), TEXT("float") }, { TEXT("double"), TEXT("double") },
		{ TEXT("name"), TEXT("FName") }, { TEXT("fname"), TEXT("FName") }, { TEXT("string"), TEXT("FString") }, { TEXT("fstring"), TEXT("FString") },
	};
	if (const FString* Scalar = Scalars.Find(Wanted.ToLower()))
		return *Scalar;
	const FString Bare = Wanted.StartsWith(TEXT("F")) && Wanted.Len() > 1 && FChar::IsUpper(Wanted[1]) ? Wanted.RightChop(1) : Wanted;
	for (const FString& Candidate : { Wanted, Bare })
	{
		if (const UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*Candidate, EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::ExactClass))
			return Struct->GetPathName();
		if (const UEnum* Enum = FindFirstObject<UEnum>(*Candidate, EFindFirstObjectOptions::NativeFirst))
			return Enum->GetPathName();
	}
	return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a rig variable type (bool, int, float, double, Name, string, Vector, Rotator, Quat, Transform, LinearColor, RigElementKey, an enum, Array<Vector>, ...)"), *Type));
}

TSharedPtr<FJsonValue> UnrealAIControlRigGraph::NodesJson(UObject* Object)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	const UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Object);
	const URigVMGraph* Graph = Rig ? Rig->GetModel() : nullptr;
	if (!Graph)
		return MakeShared<FJsonValueArray>(Rows);
	for (URigVMNode* Node : Graph->GetNodes())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), Node->GetName());
		Row->SetStringField(TEXT("Title"), Node->GetNodeTitle());
		Row->SetStringField(TEXT("Unit"), UnitName(Node));
		TArray<TSharedPtr<FJsonValue>> Next;
		TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
		const int32 Prefix = Node->GetName().Len() + 1;
		for (URigVMPin* Pin : Node->GetAllPinsRecursively())
		{
			if (Pin->IsExecuteContext())
			{
				if (IsOutput(Pin))
				{
					for (const URigVMPin* Target : Pin->GetLinkedTargetPins())
						Next.Add(MakeShared<FJsonValueString>(Target->GetNode()->GetName()));
				}
				continue;
			}
			if (!IsInput(Pin))
				continue;
			const TArray<URigVMPin*> Sources = Pin->GetLinkedSourcePins();
			if (Sources.Num())
				Inputs->SetStringField(Pin->GetPinPath().RightChop(Prefix), TEXT("@") + Sources[0]->GetPinPath());
			else if ((Pin->IsArray() || Pin->GetSubPins().Num() == 0) && !(Pin->GetParentPin() && Pin->GetParentPin()->IsArray()))
				Inputs->SetStringField(Pin->GetPinPath().RightChop(Prefix), Pin->GetDefaultValue()); // leaves and whole arrays
		}
		Row->SetArrayField(TEXT("Next"), Next);
		Row->SetObjectField(TEXT("Inputs"), Inputs);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return MakeShared<FJsonValueArray>(Rows);
}

TSharedPtr<FJsonValue> UnrealAIControlRigGraph::VariablesJson(UObject* Object)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	if (const UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Object))
	{
		for (const FRigVMGraphVariableDescription& Variable : Rig->GetMemberVariables())
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Name"), Variable.Name.ToString());
			Row->SetStringField(TEXT("Type"), Variable.CPPTypeObject ? Variable.CPPTypeObject->GetName() : Variable.CPPType);
			Row->SetBoolField(TEXT("Public"), Variable.bPublic);
			Row->SetStringField(TEXT("Default"), Variable.DefaultValue);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	return MakeShared<FJsonValueArray>(Rows);
}
