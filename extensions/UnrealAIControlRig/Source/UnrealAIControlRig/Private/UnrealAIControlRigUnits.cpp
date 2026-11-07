// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIControlRigUnits.h"

#include "IUnrealMcpRqlModule.h"

#include "Interfaces/IPluginManager.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMFunction.h"
#include "RigVMCore/RigVMRegistry.h"
#include "Units/RigUnitContext.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UnrealAIControlRigUnitsLocal
{
	/** Lower case, no spaces or underscores: "Two Bone IK" and "TwoBoneIK" compare equal. */
	FString Compact(const FString& Text)
	{
		FString Out;
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsWhitespace(Char) && Char != TEXT('_'))
				Out.AppendChar(FChar::ToLower(Char));
		}
		return Out;
	}

	/** RigUnit_AimItem -> AimItem; RigVMFunction_MathVectorAdd -> MathVectorAdd. */
	FString ShortName(const FString& StructName)
	{
		for (const TCHAR* Prefix : { TEXT("RigUnit_"), TEXT("RigVMFunction_") })
		{
			if (StructName.StartsWith(Prefix))
				return StructName.RightChop(FCString::Strlen(Prefix));
		}
		return StructName;
	}

	/** A pin type as agents read it: Vector, float, RigElementKey, Array<Transform>, EControlRigVectorKind. */
	FString TypeName(const FProperty* Property)
	{
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
			return FString::Printf(TEXT("Array<%s>"), *TypeName(Array->Inner));
		if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			return Struct->Struct->GetName();
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
			return Enum->GetEnum() ? Enum->GetEnum()->GetName() : Property->GetCPPType();
		if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
			return Byte->Enum ? Byte->Enum->GetName() : TEXT("uint8");
		return Property->GetCPPType();
	}

	/** Structs whose members a pin row lists (the unit's own settings structs, not math values). */
	bool ListsMembers(const UScriptStruct* Struct)
	{
		static const TSet<FString> Values = { TEXT("Vector"), TEXT("Vector2D"), TEXT("Vector4"), TEXT("Rotator"), TEXT("Quat"),
			TEXT("Transform"), TEXT("EulerTransform"), TEXT("LinearColor"), TEXT("Color"), TEXT("RigElementKey"), TEXT("CachedRigElement"),
			TEXT("RigElementKeyCollection") };
		return Struct && !Values.Contains(Struct->GetName());
	}

	bool IsExecutePin(const FProperty* Property)
	{
		const FStructProperty* Struct = CastField<FStructProperty>(Property);
		return Struct && Struct->Struct && Struct->Struct->IsChildOf(FRigVMExecutePin::StaticStruct());
	}

	/** The unit's pins as rows, and whether it has an execute pin (runs in the chain). */
	TArray<FString> Pins(const UScriptStruct* Struct, bool& bOutExecute)
	{
		bOutExecute = false;
		TArray<FString> Out;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (IsExecutePin(Property))
			{
				bOutExecute = true;
				continue;
			}
			const bool bIn = Property->HasMetaData(TEXT("Input")) || Property->HasMetaData(TEXT("Visible"));
			const bool bOut = Property->HasMetaData(TEXT("Output"));
			if (!bIn && !bOut)
				continue;
			FString Row = FString::Printf(TEXT("%s: %s (%s)"), *Property->GetName(), *TypeName(Property),
				bIn && bOut ? TEXT("in/out") : (bIn ? TEXT("in") : TEXT("out")));
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (StructProperty && ListsMembers(StructProperty->Struct))
			{
				TArray<FString> Members;
				for (TFieldIterator<FProperty> Member(StructProperty->Struct); Member; ++Member)
				{
					if (Member->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) || Member->HasMetaData(TEXT("Input")))
						Members.Add(FString::Printf(TEXT("%s %s"), *Member->GetName(), *TypeName(*Member)));
				}
				if (Members.Num())
					Row += FString::Printf(TEXT(" {%s}"), *FString::Join(Members, TEXT(", ")));
			}
			Out.Add(MoveTemp(Row));
		}
		return Out;
	}

	struct FUnit
	{
		UScriptStruct* Struct = nullptr;
		FString DisplayName;
		bool bDeprecated = false;
	};

	/** Every unit a Control Rig can run (its execute context is the Control Rig one or a base of it), one per struct. */
	TArray<FUnit> Units()
	{
		TArray<FUnit> Out;
		TSet<const UScriptStruct*> Seen;
		const FRigVMRegistry& Registry = FRigVMRegistry::Get();
		const TChunkedArray<FRigVMFunction>& Functions = Registry.GetFunctions();
		for (int32 Index = 0; Index < Functions.Num(); ++Index)
		{
			const FRigVMFunction& Function = Functions[Index];
			UScriptStruct* Struct = Function.Struct;
			if (!Struct || Seen.Contains(Struct) || Struct->HasMetaData(TEXT("Abstract")))
				continue;
			const UScriptStruct* Context = Function.GetExecuteContextStruct();
			if (Context && !FControlRigExecuteContext::StaticStruct()->IsChildOf(Context))
				continue;
			Seen.Add(Struct);
			FUnit Unit;
			Unit.Struct = Struct;
			Unit.DisplayName = Struct->HasMetaData(TEXT("DisplayName")) ? Struct->GetMetaData(TEXT("DisplayName")) : ShortName(Struct->GetName());
			Unit.bDeprecated = Struct->HasMetaData(TEXT("Deprecated"));
			Out.Add(MoveTemp(Unit));
		}
		return Out;
	}

	/** An engine plugin that ships units under these names, for the error of a unit that is not registered. */
	FString PluginHint(const FString& Wanted)
	{
		struct FKnown
		{
			const TCHAR* Plugin;
			const TCHAR* What;
			TArray<FString> Keys;
		};
		static const TArray<FKnown> Known = {
			{ TEXT("FullBodyIK"), TEXT("Full Body IK (PBIK)"), { TEXT("pbik"), TEXT("fullbodyik"), TEXT("fbik") } },
			{ TEXT("ControlRigSpline"), TEXT("the spline units"), { TEXT("spline") } },
		};
		for (const FKnown& Entry : Known)
		{
			if (!Entry.Keys.ContainsByPredicate([&Wanted](const FString& Key) { return Wanted.Contains(Key); }))
				continue;
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Entry.Plugin);
			if (Plugin.IsValid() && Plugin->IsEnabled())
				return FString();
			return FString::Printf(TEXT("; %s is in the engine plugin %s, which this project has not enabled: enable it (Edit > Plugins, or add {\"Name\": \"%s\", \"Enabled\": true} to the .uproject \"Plugins\") and restart the editor"),
				Entry.What, Entry.Plugin, Entry.Plugin);
		}
		return FString();
	}
}

using namespace UnrealAIControlRigUnitsLocal;

TArray<UObject*> UnrealAIControlRigUnits::List()
{
	TArray<FUnit> Units = UnrealAIControlRigUnitsLocal::Units();
	Units.RemoveAll([](const FUnit& Unit) { return Unit.bDeprecated; });
	TArray<UUnrealAIControlRigUnit*> Rows;
	for (const FUnit& Unit : Units)
	{
		UUnrealAIControlRigUnit* Row = NewObject<UUnrealAIControlRigUnit>(GetTransientPackage());
		Row->Struct = Unit.Struct->GetName();
		Row->Name = ShortName(Row->Struct);
		Row->DisplayName = Unit.DisplayName;
		Row->Category = Unit.Struct->GetMetaData(TEXT("Category"));
		Row->Keywords = Unit.Struct->GetMetaData(TEXT("Keywords"));
		Row->Description = Unit.Struct->GetToolTipText(/*bShortTooltip*/ true).ToString().TrimStartAndEnd();
		Row->Pins = Pins(Unit.Struct, Row->Execute);
		Rows.Add(Row);
	}
	Rows.Sort([](const UUnrealAIControlRigUnit& A, const UUnrealAIControlRigUnit& B)
	{
		return A.Category != B.Category ? A.Category < B.Category : A.DisplayName < B.DisplayName;
	});
	return TArray<UObject*>(Rows);
}

UScriptStruct* UnrealAIControlRigUnits::Resolve(const FString& Name)
{
	FString Wanted = Compact(Name);
	if (Wanted.IsEmpty())
		return UnrealMcpRql::Failed<UScriptStruct*>(TEXT("name a rig unit (see controlRigUnits), e.g. AimItem"));
	const TArray<FUnit> Units = UnrealAIControlRigUnitsLocal::Units();

	// The struct ("RigUnit_AimItem", "FRigUnit_AimItem") or the struct without its prefix ("AimItem"): unique.
	for (const FUnit& Unit : Units)
	{
		const FString Struct = Compact(Unit.Struct->GetName());
		if (Wanted == Struct || Wanted == TEXT("f") + Struct || Wanted == Compact(ShortName(Unit.Struct->GetName())))
			return Unit.Struct;
	}
	// The display name ("Aim", "Basic IK"): several units can share one (From World on locations, rotations, transforms).
	TArray<const FUnit*> Titled;
	for (const FUnit& Unit : Units)
	{
		if (!Unit.bDeprecated && Compact(Unit.DisplayName) == Wanted)
			Titled.Add(&Unit);
	}
	if (Titled.Num() == 1)
		return Titled[0]->Struct;
	auto Describe = [](const TArray<const FUnit*>& List)
	{
		TArray<FString> Names;
		for (const FUnit* Unit : List)
			Names.Add(FString::Printf(TEXT("%s (%s)"), *ShortName(Unit->Struct->GetName()), *Unit->DisplayName));
		return FString::Join(Names, TEXT(", "));
	};
	if (Titled.Num() > 1)
		return UnrealMcpRql::Failed<UScriptStruct*>(FString::Printf(TEXT("several rig units are titled '%s': %s; pass the name"), *Name, *Describe(Titled)));

	TArray<const FUnit*> Close;
	for (const FUnit& Unit : Units)
	{
		if (!Unit.bDeprecated && Close.Num() < 8
			&& (Compact(Unit.Struct->GetName()).Contains(Wanted) || Compact(Unit.DisplayName).Contains(Wanted)
				|| Compact(Unit.Struct->GetMetaData(TEXT("Keywords"))).Contains(Wanted)))
			Close.Add(&Unit);
	}
	return UnrealMcpRql::Failed<UScriptStruct*>(FString::Printf(TEXT("no rig unit named '%s'%s%s (list them: controlRigUnits | where DisplayName contains \"...\")"),
		*Name, Close.Num() ? *FString::Printf(TEXT("; similar: %s"), *Describe(Close)) : TEXT(""), *PluginHint(Wanted)));
}
