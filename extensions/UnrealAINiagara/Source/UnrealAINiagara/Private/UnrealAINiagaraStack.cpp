// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAINiagaraStack.h"

#include "IUnrealMcpRqlModule.h"

#include "Dom/JsonObject.h"
#include "Misc/StringOutputDevice.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraEmitterHandle.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/StructOnScope.h"
#include "NiagaraEmitterEditorData.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace UnrealAINiagaraStackLocal
{
	using namespace UnrealAINiagara;

	/** The dynamic inputs `random(min, max)` and `curve(...)` set, by input type. */
	/** A static switch type (static bool / enum / int) as its plain type: same memory layout, same text forms. */
	FNiagaraTypeDefinition Plain(const FNiagaraTypeDefinition& Type)
	{
		return Type.IsStatic() ? Type.RemoveStaticDef() : Type;
	}

	const TCHAR* RandomScript(const FNiagaraTypeDefinition& InType)
	{
		const FNiagaraTypeDefinition Type = Plain(InType);
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
			return TEXT("/Niagara/DynamicInputs/UniformRange/UniformRangedFloat.UniformRangedFloat");
		if (Type == FNiagaraTypeDefinition::GetIntDef())
			return TEXT("/Niagara/DynamicInputs/UniformRange/UniformRangedInt.UniformRangedInt");
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
			return TEXT("/Niagara/DynamicInputs/UniformRange/UniformRangedVector2D.UniformRangedVector2D");
		if (Type == FNiagaraTypeDefinition::GetVec3Def())
			return TEXT("/Niagara/DynamicInputs/UniformRange/UniformRangedVector.UniformRangedVector");
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
			return TEXT("/Niagara/DynamicInputs/UniformRange/UniformRangedVector4.UniformRangedVector4");
		if (Type == FNiagaraTypeDefinition::GetColorDef())
			return TEXT("/Niagara/DynamicInputs/UniformRange/UniformRangedLinearColor.UniformRangedLinearColor");
		return nullptr;
	}

	const TCHAR* CurveScript(const FNiagaraTypeDefinition& InType)
	{
		const FNiagaraTypeDefinition Type = Plain(InType);
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
			return TEXT("/Niagara/DynamicInputs/ValueFromCurve/FloatFromCurve.FloatFromCurve");
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
			return TEXT("/Niagara/DynamicInputs/ValueFromCurve/Vector2DFromCurve.Vector2DFromCurve");
		if (Type == FNiagaraTypeDefinition::GetVec3Def())
			return TEXT("/Niagara/DynamicInputs/ValueFromCurve/VectorFromCurve.VectorFromCurve");
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
			return TEXT("/Niagara/DynamicInputs/ValueFromCurve/Vector4FromCurve.Vector4FromCurve");
		if (Type == FNiagaraTypeDefinition::GetColorDef())
			return TEXT("/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve");
		return nullptr;
	}

	/** How many floats a float-based Niagara type holds (0: not one). */
	int32 FloatCount(const FNiagaraTypeDefinition& InType)
	{
		const FNiagaraTypeDefinition Type = UnrealAINiagaraStackLocal::Plain(InType);
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
			return 1;
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
			return 2;
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
			return 3;
		if (Type == FNiagaraTypeDefinition::GetVec4Def() || Type == FNiagaraTypeDefinition::GetColorDef() || Type == FNiagaraTypeDefinition::GetQuatDef())
			return 4;
		return 0;
	}

	FString Number(double Value)
	{
		FString Text = FString::Printf(TEXT("%.6g"), Value);
		return Text == TEXT("-0") ? FString(TEXT("0")) : Text;
	}

	/** Split @p Text at top-level commas (not inside brackets). */
	TArray<FString> SplitTopLevel(const FString& Text)
	{
		TArray<FString> Parts;
		int32 Depth = 0;
		FString Current;
		for (const TCHAR Char : Text)
		{
			if (Char == TEXT('(') || Char == TEXT('[') || Char == TEXT('{'))
				++Depth;
			else if (Char == TEXT(')') || Char == TEXT(']') || Char == TEXT('}'))
				--Depth;
			if (Char == TEXT(',') && Depth == 0)
			{
				Parts.Add(Current.TrimStartAndEnd());
				Current.Reset();
				continue;
			}
			Current.AppendChar(Char);
		}
		if (!Current.TrimStartAndEnd().IsEmpty())
			Parts.Add(Current.TrimStartAndEnd());
		return Parts;
	}

	/** The arguments of `name(a, b, ...)` when @p Text is such a call (case-insensitive name). */
	bool CallArguments(const FString& Text, const TCHAR* Name, TArray<FString>& OutArguments)
	{
		const FString Prefix = FString(Name) + TEXT("(");
		if (!Text.StartsWith(Prefix, ESearchCase::IgnoreCase) || !Text.EndsWith(TEXT(")")))
			return false;
		OutArguments = SplitTopLevel(Text.Mid(Prefix.Len(), Text.Len() - Prefix.Len() - 1));
		return true;
	}

	/**
	 * Numbers from "(1, 2, 3)", "[1, 2, 3]", "1 2 3", "(X=1,Y=2,Z=3)" or "#rrggbb[aa]" (sRGB, converted to linear as the
	 * color picker does; bColor only).
	 */
	bool ParseFloats(const FString& Text, bool bColor, TArray<double>& Out)
	{
		FString Body = Text.TrimStartAndEnd();
		if (bColor && Body.StartsWith(TEXT("#")))
		{
			const FString Hex = Body.Mid(1);
			if (!(Hex.Len() == 6 || Hex.Len() == 8))
				return false;
			for (const TCHAR Char : Hex)
			{
				if (!FChar::IsHexDigit(Char))
					return false;
			}
			const FLinearColor Color = FLinearColor(FColor::FromHex(Hex));
			Out = { Color.R, Color.G, Color.B, Hex.Len() == 8 ? Color.A : 1.0 };
			return true;
		}
		Body.ReplaceCharInline(TEXT('('), TEXT(' '));
		Body.ReplaceCharInline(TEXT(')'), TEXT(' '));
		Body.ReplaceCharInline(TEXT('['), TEXT(' '));
		Body.ReplaceCharInline(TEXT(']'), TEXT(' '));
		Body.ReplaceCharInline(TEXT('{'), TEXT(' '));
		Body.ReplaceCharInline(TEXT('}'), TEXT(' '));
		Body.ReplaceCharInline(TEXT(','), TEXT(' '));
		TArray<FString> Tokens;
		Body.ParseIntoArrayWS(Tokens);
		for (FString Token : Tokens)
		{
			int32 Equals = INDEX_NONE;
			if (Token.FindChar(TEXT('='), Equals))
				Token.MidInline(Equals + 1);
			else if (Token.FindChar(TEXT(':'), Equals))
				Token.MidInline(Equals + 1);
			if (Token.IsEmpty())
				continue;
			if (!Token.IsNumeric())
				return false;
			Out.Add(FCString::Atod(*Token));
		}
		return Out.Num() > 0;
	}

	/** Fill @p Count floats from parsed numbers: one number fills every component (a colour keeps alpha 1). */
	bool FitFloats(TArray<double>& Values, int32 Count, bool bColor)
	{
		if (Values.Num() == Count)
			return true;
		if (Values.Num() == 1)
		{
			const double Value = Values[0];
			Values.Init(Value, Count);
			if (bColor && Count == 4)
				Values[3] = 1.0;
			return true;
		}
		if (bColor && Count == 4 && Values.Num() == 3)
		{
			Values.Add(1.0);
			return true;
		}
		return false;
	}

	bool ParseBool(const FString& Text, bool& Out)
	{
		const FString Lower = Text.TrimStartAndEnd().ToLower();
		if (Lower == TEXT("true") || Lower == TEXT("1") || Lower == TEXT("on") || Lower == TEXT("yes"))
			return (Out = true), true;
		if (Lower == TEXT("false") || Lower == TEXT("0") || Lower == TEXT("off") || Lower == TEXT("no"))
			return (Out = false), true;
		return false;
	}

	FString EnumEntries(const UEnum& Enum)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < Enum.NumEnums() - 1; ++Index)
		{
			if (!Enum.HasMetaData(TEXT("Hidden"), Index))
				Names.Add(Enum.GetDisplayNameTextByIndex(Index).ToString());
		}
		return FString::Join(Names, TEXT(", "));
	}

	/** The curve data interface of an input (its own value or, for a value-from-curve dynamic input, its child's). */
	UNiagaraDataInterfaceCurveBase* CurveOf(UNiagaraStackFunctionInput& Input)
	{
		return Input.GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Data
			? Cast<UNiagaraDataInterfaceCurveBase>(Input.GetDataValueObject()) : nullptr;
	}

	FString FormatCurve(UNiagaraDataInterfaceCurveBase& Curve)
	{
		TArray<UNiagaraDataInterfaceCurveBase::FCurveData> Channels;
		Curve.GetCurveData(Channels);
		TArray<float> Times;
		for (const UNiagaraDataInterfaceCurveBase::FCurveData& Channel : Channels)
		{
			for (const FRichCurveKey& Key : Channel.Curve->GetConstRefOfKeys())
				Times.AddUnique(Key.Time);
		}
		Times.Sort();
		TArray<FString> Keys;
		for (const float Time : Times)
		{
			TArray<FString> Values;
			for (const UNiagaraDataInterfaceCurveBase::FCurveData& Channel : Channels)
				Values.Add(Number(Channel.Curve->Eval(Time)));
			const FString Joined = FString::Join(Values, TEXT(", "));
			Keys.Add(Number(Time) + TEXT(": ") + (Values.Num() == 1 ? Joined : TEXT("(") + Joined + TEXT(")")));
		}
		return FString::Printf(TEXT("curve(%s)"), *FString::Join(Keys, TEXT(", ")));
	}

	/** Replace the keys of @p Curve with `t: value` entries (values with one number per curve channel, or "#rrggbb" for colours). */
	bool WriteCurve(UNiagaraDataInterfaceCurveBase& Curve, const TArray<FString>& Entries, FString& OutError)
	{
		TArray<UNiagaraDataInterfaceCurveBase::FCurveData> Channels;
		Curve.GetCurveData(Channels);
		if (Channels.Num() == 0)
			return (OutError = FString::Printf(TEXT("%s has no curves"), *Curve.GetClass()->GetName())), false;
		TArray<TPair<double, TArray<double>>> Keys;
		for (const FString& Entry : Entries)
		{
			int32 Colon = INDEX_NONE;
			if (!Entry.FindChar(TEXT(':'), Colon))
				return (OutError = FString::Printf(TEXT("curve key '%s' is not 'time: value' (e.g. curve(0: 1, 1: 0))"), *Entry)), false;
			const FString Time = Entry.Left(Colon).TrimStartAndEnd();
			TArray<double> Values;
			const bool bColor = Channels.Num() == 4;
			if (!Time.IsNumeric() || !ParseFloats(Entry.Mid(Colon + 1), bColor, Values) || !FitFloats(Values, Channels.Num(), bColor))
				return (OutError = FString::Printf(TEXT("curve key '%s': expected 'time: value' with %d number(s)%s"), *Entry, Channels.Num(),
					bColor ? TEXT(" or #rrggbb") : TEXT(""))), false;
			Keys.Add({ FCString::Atod(*Time), Values });
		}
		if (Keys.Num() == 0)
			return (OutError = TEXT("a curve needs at least one key: curve(0: 1, 1: 0)")), false;
		Curve.Modify();
		for (int32 Channel = 0; Channel < Channels.Num(); ++Channel)
		{
			FRichCurve& Rich = *Channels[Channel].Curve;
			Rich.Reset();
			for (const TPair<double, TArray<double>>& Key : Keys)
			{
				const FKeyHandle Handle = Rich.AddKey(Key.Key, Key.Value[Channel]);
				Rich.SetKeyInterpMode(Handle, RCIM_Cubic);
			}
			Rich.AutoSetTangents();
		}
		Curve.UpdateLUT();
		Curve.PostEditChange();
		return true;
	}

	/**
	 * The packages of the graphs (the system's, its emitters' and every module / dynamic input script they call) holding
	 * pins whose Niagara type is gone: a class or struct of a plugin that is not loaded. The Niagara editor asserts on them.
	 */
	TArray<FString> MissingTypes(UNiagaraSystem& System)
	{
		TArray<FString> Where;
		TSet<const UEdGraph*> Visited;
		TFunction<void(const UNiagaraScriptSourceBase*)> Scan = [&](const UNiagaraScriptSourceBase* SourceBase)
		{
			const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
			const UEdGraph* Graph = Source ? Source->NodeGraph : nullptr;
			if (!Graph || Visited.Contains(Graph) || Where.Num() >= 4)
				return;
			Visited.Add(Graph);
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
					continue;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					const FName Category = Pin ? Pin->PinType.PinCategory : NAME_None;
					if ((Category == UEdGraphSchema_Niagara::PinCategoryClass || Category == UEdGraphSchema_Niagara::PinCategoryStaticClass
						|| Category == UEdGraphSchema_Niagara::PinCategoryType || Category == UEdGraphSchema_Niagara::PinCategoryStaticType)
						&& Pin->PinType.PinSubCategoryObject == nullptr)
					{
						Where.AddUnique(Graph->GetOutermost()->GetName());
						break;
					}
				}
				if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node))
				{
					if (Call->FunctionScript)
						Scan(Call->FunctionScript->GetLatestSource());
				}
			}
		};
		Scan(System.GetSystemSpawnScript()->GetLatestSource());
		for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
				Scan(Data->GraphSource);
		}
		return Where;
	}

	/**
	 * The inputs below @p Entry. The stack nests inputs an option controls under that option's input (Lifetime Min under
	 * Lifetime Mode): those keep @p Prefix, the path of their function call. The inputs of a dynamic input set on an input
	 * belong to another function call and are named below it: "Lifetime.Minimum".
	 */
}

FString UnrealAINiagara::FormatDataInterface(UNiagaraDataInterface& Data)
{
		TArray<FString> Parts;
		for (TFieldIterator<FProperty> It(Data.GetClass()); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Deprecated))
				continue;
			FString Text;
			It->ExportTextItem_InContainer(Text, &Data, nullptr, &Data, PPF_None);
			Parts.Add(It->GetName() + TEXT("=") + Text);
		}
		return FString::Printf(TEXT("%s(%s)"), *Data.GetClass()->GetName(), *FString::Join(Parts, TEXT(", ")));
	}

bool UnrealAINiagara::WriteDataInterface(UNiagaraDataInterface& Data, const FString& InText, FString& OutError)
{
		using namespace UnrealAINiagaraStackLocal;
		FString Text = InText.TrimStartAndEnd().TrimQuotes();
		if (Text.StartsWith(TEXT("/")) && !Text.Contains(TEXT("=")))
		{
			// An asset path alone: the editable object property that takes it (a static mesh interface's DefaultMesh; an
			// editor-only Preview* property only when nothing else does).
			UObject* Asset = UnrealMcpRql::LoadObject(UObject::StaticClass(), Text);
			if (!Asset)
				return (OutError = FString::Printf(TEXT("no asset at '%s'"), *Text)), false;
			FObjectPropertyBase* Best = nullptr;
			for (TFieldIterator<FObjectPropertyBase> It(Data.GetClass()); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Deprecated) || !Asset->IsA(It->PropertyClass))
					continue;
				if (!Best || (Best->GetName().StartsWith(TEXT("Preview")) && !It->GetName().StartsWith(TEXT("Preview"))))
					Best = *It;
			}
			if (Best)
				Text = Best->GetName() + TEXT("=") + Asset->GetPathName();
			if (Text.StartsWith(TEXT("/")))
				return (OutError = FString::Printf(TEXT("no property of %s takes a %s (%s)"), *Data.GetClass()->GetName(), *Asset->GetClass()->GetName(), *FormatDataInterface(Data))), false;
		}
		if (Text.StartsWith(TEXT("(")) && Text.EndsWith(TEXT(")")))
			Text = Text.Mid(1, Text.Len() - 2);
		TArray<TPair<FProperty*, FString>> Writes;
		for (const FString& Part : SplitTopLevel(Text))
		{
			FString Name, Value;
			if (!Part.Split(TEXT("="), &Name, &Value))
				return (OutError = FString::Printf(TEXT("a %s takes \"Property=Value, ...\" (its properties: %s)"), *Data.GetClass()->GetName(), *FormatDataInterface(Data))), false;
			FProperty* Found = nullptr;
			for (TFieldIterator<FProperty> It(Data.GetClass()); It && !Found; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Edit) && Normalize(It->GetName()) == Normalize(Name))
					Found = *It;
			}
			if (!Found)
				return (OutError = FString::Printf(TEXT("%s has no property '%s' (%s)"), *Data.GetClass()->GetName(), *Name.TrimStartAndEnd(), *FormatDataInterface(Data))), false;
			Writes.Add({ Found, Value.TrimStartAndEnd() });
		}
		if (Writes.Num() == 0)
			return (OutError = FString::Printf(TEXT("no \"Property=Value\" given (%s)"), *FormatDataInterface(Data))), false;
		Data.Modify();
		for (const TPair<FProperty*, FString>& Write : Writes)
		{
			FString Value = Write.Value;
			Value.TrimQuotesInline();
			const TCHAR* End = Write.Key->ImportText_InContainer(*Value, &Data, &Data, PPF_None);
			if (!End)
				return (OutError = FString::Printf(TEXT("'%s' is not a value of %s"), *Value, *Write.Key->GetName())), false;
			FPropertyChangedEvent Event(Write.Key, EPropertyChangeType::ValueSet);
			Data.PostEditChangeProperty(Event);
		}
		return true;
}

namespace UnrealAINiagaraStackLocal
{
	void CollectInputs(UNiagaraStackEntry& Entry, const FString& Prefix, const UNiagaraNodeFunctionCall* PrefixNode, const FString& ParentPath,
		bool bParentHidden, TArray<FInputRef>& Out)
	{
		TArray<UNiagaraStackEntry*> Children;
		Entry.GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(Child))
			{
				const UNiagaraNodeFunctionCall* Node = &Input->GetInputFunctionCallNode();
				const FString OwnPrefix = Node == PrefixNode ? Prefix : ParentPath + TEXT(".");
				const FString Path = OwnPrefix + Input->GetDisplayName().ToString();
				const bool bHidden = bParentHidden || Input->GetIsHidden() || !Input->GetShouldPassFilterForVisibleCondition();
				Out.Add({ Input, Path, bHidden });
				CollectInputs(*Input, OwnPrefix, Node, Path, bHidden, Out);
			}
			else if (Child && !Cast<UNiagaraStackModuleItem>(Child))
			{
				CollectInputs(*Child, Prefix, PrefixNode, ParentPath, bParentHidden, Out);
			}
		}
	}

	void CollectModules(UNiagaraStackEntry& Entry, const FString& Emitter, const FString& Stage, TArray<FModuleRef>& Out)
	{
		TArray<UNiagaraStackEntry*> Children;
		Entry.GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (UNiagaraStackModuleItem* Module = Cast<UNiagaraStackModuleItem>(Child))
				Out.Add({ Module, Emitter, Stage, Module->GetDisplayName().ToString() });
			else if (UNiagaraStackScriptItemGroup* Group = Cast<UNiagaraStackScriptItemGroup>(Child))
				CollectModules(*Group, Emitter, Group->GetDisplayName().ToString(), Out);
			else if (Child && !Cast<UNiagaraStackFunctionInput>(Child))
				CollectModules(*Child, Emitter, Stage, Out);
		}
	}

	FString ModuleList(const TArray<FModuleRef>& Modules)
	{
		TArray<FString> Names;
		for (const FModuleRef& Module : Modules)
			Names.Add(Module.Stage + TEXT("/") + Module.Name);
		return Names.Num() ? FString::Join(Names, TEXT(", ")) : FString(TEXT("none"));
	}

	bool InputMatches(const FInputRef& Input, const FString& Path)
	{
		TArray<FString> Want, Have;
		Path.ParseIntoArray(Want, TEXT("."));
		Input.Path.ParseIntoArray(Have, TEXT("."));
		if (Want.Num() != Have.Num())
			return false;
		for (int32 Index = 0; Index < Want.Num(); ++Index)
		{
			if (Normalize(Want[Index]) != Normalize(Have[Index]))
				return false;
		}
		return true;
	}

	UNiagaraStackFunctionInput* FindInput(const FModuleRef& Module, const FString& Path, bool bFail = true)
	{
		const TArray<FInputRef> All = Inputs(Module, /*bHidden*/ true);
		const FInputRef* Found = All.FindByPredicate([&Path](const FInputRef& Input) { return InputMatches(Input, Path); });
		if (!Found && !Path.Contains(TEXT(".")))
		{
			// "Minimum" for "Lifetime.Minimum" when only one input has that name.
			TArray<const FInputRef*> ByName;
			for (const FInputRef& Input : All)
			{
				FString Last = Input.Path;
				Input.Path.Split(TEXT("."), nullptr, &Last, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
				if (Normalize(Last) == Normalize(Path))
					ByName.Add(&Input);
			}
			Found = ByName.Num() == 1 ? ByName[0] : nullptr;
		}
		if (Found && !Found->bHidden)
			return Found->Input;
		if (bFail)
		{
			TArray<FString> Names;
			for (const FInputRef& Input : All)
			{
				if (!Input.bHidden)
					Names.Add(Input.Path);
			}
			const FString Owner = FString::Printf(TEXT("module '%s/%s' of %s"), *Module.Stage, *Module.Name, Module.Emitter.IsEmpty() ? TEXT("the system") : *Module.Emitter);
			UnrealMcpRql::Fail(Found
				? FString::Printf(TEXT("input '%s' of %s is hidden by the module's current options: set the option that selects it first (a ... Mode input; the visible inputs: %s)"),
					*Found->Path, *Owner, *FString::Join(Names, TEXT(", ")))
				: FString::Printf(TEXT("%s has no input '%s' (inputs: %s)"), *Owner, *Path, Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none")));
		}
		return nullptr;
	}

	/** Child inputs (one level below @p Path) of an input after a dynamic input was set on it. */
	TArray<FInputRef> ChildInputs(const FModuleRef& Module, const FString& Path)
	{
		TArray<FInputRef> Out;
		for (const FInputRef& Input : Inputs(Module))
		{
			if (Input.Path.StartsWith(Path + TEXT(".")) && !Input.Path.Mid(Path.Len() + 1).Contains(TEXT(".")))
				Out.Add(Input);
		}
		return Out;
	}

	bool SetLocal(UNiagaraStackFunctionInput& Input, const FString& Text, FString& OutError)
	{
		const FNiagaraTypeDefinition& Type = Input.GetInputType();
		UScriptStruct* Struct = Type.GetScriptStruct();
		if (!Struct)
			return (OutError = FString::Printf(TEXT("its type %s takes no text value"), *TypeName(Type))), false;
		TSharedRef<FStructOnScope> Value = MakeShared<FStructOnScope>(Struct);
		if (!ParseValue(Type, Text, Value->GetStructMemory(), OutError))
			return false;
		Input.SetLocalValue(Value);
		return true;
	}
}

FString UnrealAINiagara::Normalize(const FString& Name)
{
	FString Out;
	Out.Reserve(Name.Len());
	for (const TCHAR Char : Name)
	{
		if (!FChar::IsWhitespace(Char) && Char != TEXT('_') && Char != TEXT('-'))
			Out.AppendChar(FChar::ToLower(Char));
	}
	return Out;
}

FString UnrealAINiagara::EmitterNames(const UNiagaraSystem& System)
{
	TArray<FString> Names;
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		Names.Add(Handle.GetName().ToString());
	return Names.Num() ? FString::Join(Names, TEXT(", ")) : FString(TEXT("none"));
}

FNiagaraEmitterHandle* UnrealAINiagara::FindEmitter(UNiagaraSystem& System, const FString& Name)
{
	for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		if (Normalize(Handle.GetName().ToString()) == Normalize(Name))
			return &Handle;
	}
	UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no emitter '%s' (emitters: %s)"), *System.GetName(), *Name, *EmitterNames(System)));
	return nullptr;
}

FString UnrealAINiagara::TypeName(const FNiagaraTypeDefinition& InType)
{
	const FNiagaraTypeDefinition Type = UnrealAINiagaraStackLocal::Plain(InType);
	if (Type == FNiagaraTypeDefinition::GetFloatDef())
		return TEXT("float");
	if (Type == FNiagaraTypeDefinition::GetIntDef())
		return TEXT("int");
	if (Type == FNiagaraTypeDefinition::GetBoolDef())
		return TEXT("bool");
	if (Type == FNiagaraTypeDefinition::GetVec2Def())
		return TEXT("Vector2");
	if (Type == FNiagaraTypeDefinition::GetVec3Def())
		return TEXT("Vector");
	if (Type == FNiagaraTypeDefinition::GetVec4Def())
		return TEXT("Vector4");
	if (Type == FNiagaraTypeDefinition::GetColorDef())
		return TEXT("LinearColor");
	if (Type == FNiagaraTypeDefinition::GetPositionDef())
		return TEXT("Position");
	if (Type == FNiagaraTypeDefinition::GetQuatDef())
		return TEXT("Quat");
	return Type.GetName();
}

bool UnrealAINiagara::ParseValue(const FNiagaraTypeDefinition& InType, const FString& InText, uint8* Memory, FString& OutError)
{
	using namespace UnrealAINiagaraStackLocal;
	const FNiagaraTypeDefinition Type = Plain(InType);
	const FString Text = InText.TrimStartAndEnd().TrimQuotes();
	if (Type == FNiagaraTypeDefinition::GetBoolDef())
	{
		bool bValue = false;
		if (!ParseBool(Text, bValue))
			return (OutError = FString::Printf(TEXT("'%s' is not a bool (true / false)"), *Text)), false;
		reinterpret_cast<FNiagaraBool*>(Memory)->SetValue(bValue);
		return true;
	}
	if (Type.IsEnum() && Type.GetEnum())
	{
		const UEnum& Enum = *Type.GetEnum();
		for (int32 Index = 0; Index < Enum.NumEnums() - 1; ++Index)
		{
			if (Normalize(Text) == Normalize(Enum.GetNameStringByIndex(Index)) || Normalize(Text) == Normalize(Enum.GetDisplayNameTextByIndex(Index).ToString()))
			{
				reinterpret_cast<FNiagaraInt32*>(Memory)->Value = static_cast<int32>(Enum.GetValueByIndex(Index));
				return true;
			}
		}
		if (Text.IsNumeric() && Enum.IsValidEnumValue(FCString::Atoi64(*Text)))
		{
			reinterpret_cast<FNiagaraInt32*>(Memory)->Value = FCString::Atoi(*Text);
			return true;
		}
		return (OutError = FString::Printf(TEXT("'%s' is not an entry of %s (%s)"), *Text, *Enum.GetName(), *EnumEntries(Enum))), false;
	}
	if (Type == FNiagaraTypeDefinition::GetIntDef())
	{
		if (!Text.IsNumeric())
			return (OutError = FString::Printf(TEXT("'%s' is not an int"), *Text)), false;
		reinterpret_cast<FNiagaraInt32*>(Memory)->Value = FMath::RoundToInt(FCString::Atod(*Text));
		return true;
	}
	if (const int32 Count = FloatCount(Type))
	{
		const bool bColor = Type == FNiagaraTypeDefinition::GetColorDef();
		TArray<double> Values;
		if (!ParseFloats(Text, bColor, Values) || !FitFloats(Values, Count, bColor))
		{
			const TCHAR* Components = Count == 2 ? TEXT("x, y") : Count == 3 ? TEXT("x, y, z") : TEXT("x, y, z, w");
			const FString Expected = Count == 1 ? FString(TEXT("a number"))
				: bColor ? FString(TEXT("\"#rrggbb\" (sRGB) or \"(r, g, b, a)\" (linear)"))
				: FString::Printf(TEXT("\"(%s)\", or one number for every component"), Components);
			OutError = FString::Printf(TEXT("'%s' is not a %s (%s)"), *Text, *TypeName(Type), *Expected);
			return false;
		}
		float* Floats = reinterpret_cast<float*>(Memory);
		for (int32 Index = 0; Index < Count; ++Index)
			Floats[Index] = static_cast<float>(Values[Index]);
		return true;
	}
	if (UScriptStruct* Struct = Type.GetScriptStruct())
	{
		FStringOutputDevice Errors;
		const TCHAR* End = Struct->ImportText(*Text, Memory, nullptr, PPF_None, &Errors, Struct->GetName());
		if (End && FString(End).TrimStartAndEnd().IsEmpty())
			return true;
		OutError = FString::Printf(TEXT("'%s' is not a %s value"), *Text, *TypeName(Type));
		if (!Errors.IsEmpty())
			OutError += TEXT(": ") + Errors;
		return false;
	}
	return (OutError = FString::Printf(TEXT("a %s takes no text value"), *TypeName(Type))), false;
}

FString UnrealAINiagara::FormatValue(const FNiagaraTypeDefinition& InType, const uint8* Memory)
{
	using namespace UnrealAINiagaraStackLocal;
	const FNiagaraTypeDefinition Type = Plain(InType);
	if (Type == FNiagaraTypeDefinition::GetBoolDef())
		return reinterpret_cast<const FNiagaraBool*>(Memory)->GetValue() ? TEXT("true") : TEXT("false");
	if (Type.IsEnum() && Type.GetEnum())
	{
		const int32 Value = reinterpret_cast<const FNiagaraInt32*>(Memory)->Value;
		const int32 Index = Type.GetEnum()->GetIndexByValue(Value);
		return Index == INDEX_NONE ? FString::FromInt(Value) : Type.GetEnum()->GetDisplayNameTextByIndex(Index).ToString();
	}
	if (Type == FNiagaraTypeDefinition::GetIntDef())
		return FString::FromInt(reinterpret_cast<const FNiagaraInt32*>(Memory)->Value);
	if (const int32 Count = FloatCount(Type))
	{
		const float* Floats = reinterpret_cast<const float*>(Memory);
		if (Count == 1)
			return Number(Floats[0]);
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Count; ++Index)
			Parts.Add(Number(Floats[Index]));
		return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(", ")));
	}
	if (UScriptStruct* Struct = Type.GetScriptStruct())
	{
		FString Text;
		Struct->ExportText(Text, Memory, nullptr, nullptr, PPF_None, nullptr);
		return Text;
	}
	return FString();
}

FString UnrealAINiagara::FormatInput(UNiagaraStackFunctionInput& Input)
{
	using namespace UnrealAINiagaraStackLocal;
	using EMode = UNiagaraStackFunctionInput::EValueMode;
	switch (Input.GetValueMode())
	{
	case EMode::Local:
	{
		const TSharedPtr<const FStructOnScope> Value = Input.GetLocalValueStruct();
		return Value.IsValid() ? FormatValue(Input.GetInputType(), Value->GetStructMemory()) : FString();
	}
	case EMode::Linked:
		return TEXT("@") + Input.GetLinkedParameterValue().GetName().ToString();
	case EMode::Dynamic:
	{
		const UNiagaraNodeFunctionCall* Node = Input.GetDynamicInputNode();
		return FString::Printf(TEXT("dynamic input %s (its inputs follow)"), Node ? *Node->GetFunctionName() : TEXT("?"));
	}
	case EMode::Data:
	{
		UNiagaraDataInterface* Data = Input.GetDataValueObject();
		if (UNiagaraDataInterfaceCurveBase* Curve = Cast<UNiagaraDataInterfaceCurveBase>(Data))
			return FormatCurve(*Curve);
		return Data ? FString::Printf(TEXT("data interface %s"), *Data->GetClass()->GetName()) : FString(TEXT("data interface"));
	}
	case EMode::ObjectAsset:
	{
		const UObject* Asset = Input.GetObjectAssetValue();
		return Asset ? Asset->GetPathName() : FString(TEXT("None"));
	}
	case EMode::Expression:
		return TEXT("expression ") + Input.GetCustomExpressionText().ToString();
	case EMode::DefaultFunction:
	{
		const UNiagaraNodeFunctionCall* Node = Input.GetDefaultFunctionNode();
		return FString::Printf(TEXT("default %s"), Node ? *Node->GetFunctionName() : TEXT("function"));
	}
	default:
		return TEXT("(not editable here)");
	}
}

TArray<UnrealAINiagara::FInputRef> UnrealAINiagara::Inputs(const FModuleRef& Module, bool bHidden)
{
	TArray<FInputRef> Out;
	if (Module.Item)
		UnrealAINiagaraStackLocal::CollectInputs(*Module.Item, FString(), &Module.Item->GetModuleNode(), FString(), false, Out);
	if (!bHidden)
		Out.RemoveAll([](const FInputRef& Input) { return Input.bHidden; });
	return Out;
}

UnrealAINiagara::FSystemStack::FSystemStack(UNiagaraSystem& InSystem)
	: System(InSystem)
{
	// What the engine's audit commandlet and script-upgrade path build: no preview component, no sequencer, no
	// auto compile, no undo-client registration (the RQL mutation's transaction records the edits).
	const TArray<FString> Missing = UnrealAINiagaraStackLocal::MissingTypes(System);
	if (Missing.Num() > 0)
	{
		const bool bFluids = Missing.ContainsByPredicate([](const FString& Package) { return Package.StartsWith(TEXT("/NiagaraFluids/")); });
		Problem = FString::Printf(TEXT("%s uses Niagara types of a plugin that is not loaded (in %s); the Niagara editor cannot open it until that plugin is enabled%s"),
			*System.GetName(), *FString::Join(Missing, TEXT(", ")),
			bFluids ? TEXT(": Niagara Fluids needs the ChaosNiagara plugin (niagara | call EnableFluids() enables both; restart the editor)")
				: TEXT(" (the editor log names the missing classes)"));
		return;
	}
	FNiagaraSystemViewModelOptions Options;
	Options.bCanModifyEmittersFromTimeline = false;
	Options.bCanSimulate = false;
	Options.bCanAutoCompile = false;
	Options.bIsForDataProcessingOnly = true;
	Options.MessageLogGuid = System.GetAssetGuid();
	SystemViewModel = MakeShared<FNiagaraSystemViewModel>();
	SystemViewModel->Initialize(System, Options);
	// An emitter in Summary view (the Niagara Fluids templates) shows only its summary inputs in the stack: open the full
	// stack (modules, stages) for this call, as the editor's "Show Summary View" toggle does, and restore it on close.
	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : SystemViewModel->GetEmitterHandleViewModels())
	{
		UNiagaraEmitterEditorData& EditorData = Handle->GetEmitterViewModel()->GetEditorData();
		if (EditorData.ShouldShowSummaryView())
		{
			SummaryViews.Add(&EditorData);
			EditorData.SetShowSummaryView(false);
		}
	}
}

UnrealAINiagara::FSystemStack::~FSystemStack()
{
	for (const TWeakObjectPtr<UNiagaraEmitterEditorData>& EditorData : SummaryViews)
	{
		if (EditorData.IsValid())
			EditorData->SetShowSummaryView(true);
	}
	SystemViewModel.Reset();
}

FNiagaraSystemViewModel* UnrealAINiagara::FSystemStack::ViewModel() const
{
	return Check() ? SystemViewModel.Get() : nullptr;
}

bool UnrealAINiagara::FSystemStack::Check() const
{
	return SystemViewModel.IsValid() || UnrealMcpRql::Failed<bool>(Problem);
}

UNiagaraStackViewModel* UnrealAINiagara::FSystemStack::Stack(const FString& Emitter)
{
	if (!Check())
		return nullptr;
	if (Emitter.IsEmpty() || Normalize(Emitter) == TEXT("system"))
		return SystemViewModel->GetSystemStackViewModel();
	FNiagaraEmitterHandle* Handle = FindEmitter(System, Emitter);
	if (!Handle)
		return nullptr;
	const TSharedPtr<FNiagaraEmitterHandleViewModel> HandleViewModel = SystemViewModel->GetEmitterHandleViewModelById(Handle->GetId());
	UNiagaraStackViewModel* Stack = HandleViewModel.IsValid() ? HandleViewModel->GetEmitterStackViewModel() : nullptr;
	return Stack ? Stack : UnrealMcpRql::Failed<UNiagaraStackViewModel*>(FString::Printf(TEXT("emitter '%s' has no editable stack"), *Emitter));
}

void UnrealAINiagara::FSystemStack::Refresh(const FString& Emitter)
{
	UNiagaraStackViewModel* StackViewModel = Stack(Emitter);
	if (UNiagaraStackEntry* Root = StackViewModel ? StackViewModel->GetRootEntry() : nullptr)
		Root->RefreshChildren();
}

TArray<UnrealAINiagara::FModuleRef> UnrealAINiagara::FSystemStack::Modules(const FString& Emitter)
{
	TArray<FModuleRef> Out;
	UNiagaraStackViewModel* StackViewModel = Stack(Emitter);
	UNiagaraStackEntry* Root = StackViewModel ? StackViewModel->GetRootEntry() : nullptr;
	if (!Root)
		return Out;
	Root->RefreshChildren();
	const bool bSystem = Emitter.IsEmpty() || Normalize(Emitter) == TEXT("system");
	UnrealAINiagaraStackLocal::CollectModules(*Root, bSystem ? FString() : FindEmitter(System, Emitter)->GetName().ToString(), FString(), Out);
	return Out;
}

bool UnrealAINiagara::FSystemStack::FindModule(const FString& Emitter, const FString& Module, FModuleRef& Out)
{
	if (!Stack(Emitter))
		return false;
	const TArray<FModuleRef> All = Modules(Emitter);
	FString Stage, Name = Module;
	Module.Split(TEXT("/"), &Stage, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	TArray<FModuleRef> Found;
	for (const FModuleRef& Candidate : All)
	{
		const bool bName = Normalize(Candidate.Name) == Normalize(Name)
			|| Normalize(Candidate.Item->GetModuleNode().GetFunctionName()) == Normalize(Name);
		if (bName && (Stage.IsEmpty() || Normalize(Candidate.Stage) == Normalize(Stage)))
			Found.Add(Candidate);
	}
	const FString Owner = Emitter.IsEmpty() ? FString(TEXT("the system stack")) : FString::Printf(TEXT("emitter '%s'"), *Emitter);
	if (Found.Num() == 1)
		return (Out = Found[0]), true;
	if (Found.Num() > 1)
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has several '%s' modules (%s): name it with its stage, e.g. \"%s/%s\""),
			*Owner, *Name, *UnrealAINiagaraStackLocal::ModuleList(Found), *Found[0].Stage, *Found[0].Name));
	return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no module '%s' (modules: %s); add one with AddModule"),
		*Owner, *Module, *UnrealAINiagaraStackLocal::ModuleList(All)));
}

UNiagaraStackScriptItemGroup* UnrealAINiagara::FSystemStack::FindStage(const FString& Emitter, const FString& Stage)
{
	UNiagaraStackViewModel* StackViewModel = Stack(Emitter);
	UNiagaraStackEntry* Root = StackViewModel ? StackViewModel->GetRootEntry() : nullptr;
	if (!Root)
		return nullptr;
	Root->RefreshChildren();
	TArray<UNiagaraStackScriptItemGroup*> Groups;
	Root->GetUnfilteredChildrenOfType(Groups, /*bRecursive*/ true);
	TArray<FString> Names;
	for (UNiagaraStackScriptItemGroup* Group : Groups)
	{
		const FString Name = Group->GetDisplayName().ToString();
		if (Normalize(Name) == Normalize(Stage))
			return Group;
		Names.Add(Name);
	}
	return UnrealMcpRql::Failed<UNiagaraStackScriptItemGroup*>(FString::Printf(TEXT("%s has no stage '%s' (stages: %s)"),
		Emitter.IsEmpty() ? TEXT("the system stack") : *FString::Printf(TEXT("emitter '%s'"), *Emitter), *Stage, *FString::Join(Names, TEXT(", "))));
}

FString UnrealAINiagara::FSystemStack::SetInput(const FString& Emitter, const FString& Module, const FString& InputPath, const FString& InValue)
{
	using namespace UnrealAINiagaraStackLocal;
	FModuleRef ModuleRef;
	if (!FindModule(Emitter, Module, ModuleRef))
		return FString();
	UNiagaraStackFunctionInput* Input = FindInput(ModuleRef, InputPath);
	if (!Input)
		return FString();
	const FString Where = FString::Printf(TEXT("%s/%s.%s"), *ModuleRef.Stage, *ModuleRef.Name, *InputPath);
	if (Input->GetHasEditCondition() && !Input->GetEditConditionEnabled())
	{
		if (!Input->GetShowEditConditionInline())
		{
			const TOptional<FNiagaraVariable> Condition = Input->GetEditConditionVariable();
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s is inactive: it depends on another input%s; set that first"),
				*Where, Condition.IsSet() ? *FString::Printf(TEXT(" (%s)"), *Condition->GetName().ToString()) : TEXT("")));
		}
		Input->SetEditConditionEnabled(true);
	}

	const FNiagaraTypeDefinition Type = Input->GetInputType();
	const FString Value = InValue.TrimStartAndEnd();
	FString Error;
	TArray<FString> Arguments;
	const auto Refind = [&]() -> UNiagaraStackFunctionInput*
	{
		Refresh(Emitter);
		return FindModule(Emitter, ModuleRef.Stage + TEXT("/") + ModuleRef.Name, ModuleRef) ? FindInput(ModuleRef, InputPath) : nullptr;
	};

	if (CallArguments(Value, TEXT("random"), Arguments))
	{
		const TCHAR* ScriptPath = RandomScript(Type);
		UNiagaraScript* Script = ScriptPath ? Cast<UNiagaraScript>(UnrealMcpRql::LoadObject(UNiagaraScript::StaticClass(), ScriptPath)) : nullptr;
		if (!Script || Arguments.Num() != 2)
			return UnrealMcpRql::Failed<FString>(Script
				? FString::Printf(TEXT("random takes two values: random(min, max), e.g. random(1, 2) or random((0, 0, 100), (0, 0, 300))"))
				: FString::Printf(TEXT("%s is a %s: random(min, max) works on float, int, vector and colour inputs"), *Where, *TypeName(Type)));
		Input->SetDynamicInput(Script);
		if (!(Input = Refind()))
			return FString();
		const TArray<FInputRef> Children = ChildInputs(ModuleRef, InputPath);
		const FInputRef* Min = Children.FindByPredicate([](const FInputRef& R) { return Normalize(R.Path).EndsWith(TEXT("minimum")) || Normalize(R.Path).EndsWith(TEXT("min")); });
		const FInputRef* Max = Children.FindByPredicate([](const FInputRef& R) { return Normalize(R.Path).EndsWith(TEXT("maximum")) || Normalize(R.Path).EndsWith(TEXT("max")); });
		if (!Min || !Max)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: the random dynamic input has no Minimum / Maximum inputs"), *Where));
		if (!SetLocal(*Min->Input, Arguments[0], Error) || !SetLocal(*Max->Input, Arguments[1], Error))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: %s"), *Where, *Error));
		return FString::Printf(TEXT("random(%s, %s)"), *FormatInput(*Min->Input), *FormatInput(*Max->Input));
	}

	if (CallArguments(Value, TEXT("curve"), Arguments))
	{
		UNiagaraDataInterfaceCurveBase* Curve = CurveOf(*Input);
		if (!Curve)
		{
			const TCHAR* ScriptPath = CurveScript(Type);
			UNiagaraScript* Script = ScriptPath ? Cast<UNiagaraScript>(UnrealMcpRql::LoadObject(UNiagaraScript::StaticClass(), ScriptPath)) : nullptr;
			if (!Script)
				return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s is a %s: curve(...) works on float, vector and colour inputs and on curve data interfaces"), *Where, *TypeName(Type)));
			// A value-from-curve dynamic input: the curve is indexed by the particle's normalized age (over its life).
			Input->SetDynamicInput(Script);
			if (!(Input = Refind()))
				return FString();
			for (const FInputRef& Child : ChildInputs(ModuleRef, InputPath))
			{
				if ((Curve = CurveOf(*Child.Input)) != nullptr)
					break;
			}
			if (!Curve)
				return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: the value-from-curve dynamic input has no curve"), *Where));
		}
		if (!WriteCurve(*Curve, Arguments, Error))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: %s"), *Where, *Error));
		return FormatCurve(*Curve);
	}

	if (Value.StartsWith(TEXT("@")))
	{
		Input->SetLinkedParameterValue(FNiagaraVariableBase(Type, FName(*Value.Mid(1))));
	}
	else if (Normalize(Value) == TEXT("default"))
	{
		if (Input->CanReset())
			Input->Reset();
	}
	else if (Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Data)
	{
		UNiagaraDataInterface* Data = Input->GetDataValueObject();
		if (!Data || Cast<UNiagaraDataInterfaceCurveBase>(Data))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s is a curve: curve(time: value, ...), e.g. curve(0: 1, 1: 0)"), *Where));
		if (!WriteDataInterface(*Data, Value, Error))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: %s"), *Where, *Error));
		return FormatDataInterface(*Data);
	}
	else if (Type.IsUObject() && !Type.IsDataInterface())
	{
		UObject* Asset = Normalize(Value) == TEXT("none") ? nullptr : UnrealMcpRql::LoadObject(Type.GetClass(), Value);
		if (!Asset && Normalize(Value) != TEXT("none"))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s takes a %s asset path; '%s' is none"), *Where, *Type.GetClass()->GetName(), *Value));
		Input->SetObjectAssetValue(Asset);
	}
	else if (!SetLocal(*Input, Value, Error))
	{
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: %s"), *Where, *Error));
	}
	return (Input = Refind()) != nullptr ? FormatInput(*Input) : FString();
}

TSharedPtr<FJsonValue> UnrealAINiagara::FSystemStack::ToJson(bool bWithInputs)
{
	if (!SystemViewModel.IsValid())
		return MakeShared<FJsonValueString>(Problem);
	TArray<FString> Emitters = { FString() };
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		Emitters.Add(Handle.GetName().ToString());
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FString& Emitter : Emitters)
	{
		for (const FModuleRef& Module : Modules(Emitter))
		{
			const FString EmitterName = Emitter.IsEmpty() ? FString(TEXT("System")) : Emitter;
			if (!bWithInputs)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("Emitter"), EmitterName);
				Row->SetStringField(TEXT("Stage"), Module.Stage);
				Row->SetStringField(TEXT("Module"), Module.Name);
				Row->SetBoolField(TEXT("Enabled"), Module.Item->GetIsEnabled());
				Rows.Add(MakeShared<FJsonValueObject>(Row));
				continue;
			}
			for (const FInputRef& Input : Inputs(Module))
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("Emitter"), EmitterName);
				Row->SetStringField(TEXT("Module"), Module.Stage + TEXT("/") + Module.Name);
				Row->SetStringField(TEXT("Input"), Input.Path);
				Row->SetStringField(TEXT("Type"), TypeName(Input.Input->GetInputType()));
				Row->SetStringField(TEXT("Value"), FormatInput(*Input.Input));
				if (Input.Input->GetHasEditCondition() && !Input.Input->GetEditConditionEnabled())
					Row->SetBoolField(TEXT("Active"), false);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
	}
	return MakeShared<FJsonValueArray>(Rows);
}
