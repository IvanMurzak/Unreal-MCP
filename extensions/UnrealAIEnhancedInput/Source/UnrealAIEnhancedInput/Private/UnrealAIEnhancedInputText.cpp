// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIEnhancedInputText.h"

#include "IUnrealMcpRqlModule.h"

#include "InputAction.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"

namespace UnrealAIEnhancedInputTextLocal
{
	/** Lower case without spaces and underscores: "Hold Time Threshold", "hold_time_threshold" and "HoldTimeThreshold" match. */
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

	/** @p Text split on the commas outside parentheses, brackets and quotes, each part trimmed (empty parts dropped). */
	TArray<FString> SplitTopLevel(const FString& Text)
	{
		TArray<FString> Parts;
		FString Current;
		int32 Depth = 0;
		TCHAR Quote = 0;
		for (const TCHAR Char : Text)
		{
			if (Quote)
			{
				if (Char == Quote)
					Quote = 0;
			}
			else if (Char == TEXT('"') || Char == TEXT('\''))
				Quote = Char;
			else if (Char == TEXT('(') || Char == TEXT('['))
				++Depth;
			else if ((Char == TEXT(')') || Char == TEXT(']')) && Depth > 0)
				--Depth;
			else if (Char == TEXT(',') && Depth == 0)
			{
				Current.TrimStartAndEndInline();
				if (!Current.IsEmpty())
					Parts.Add(Current);
				Current.Reset();
				continue;
			}
			Current.AppendChar(Char);
		}
		Current.TrimStartAndEndInline();
		if (!Current.IsEmpty())
			Parts.Add(Current);
		return Parts;
	}

	FString Unquote(FString Text)
	{
		Text.TrimStartAndEndInline();
		if (Text.Len() >= 2 && (Text[0] == TEXT('"') || Text[0] == TEXT('\'')) && Text[Text.Len() - 1] == Text[0])
			Text = Text.Mid(1, Text.Len() - 2);
		return Text;
	}

	const TCHAR* Prefix(const UClass* BaseClass)
	{
		return BaseClass->IsChildOf(UInputModifier::StaticClass()) ? TEXT("InputModifier") : TEXT("InputTrigger");
	}

	const TCHAR* KindName(const UClass* BaseClass)
	{
		return BaseClass->IsChildOf(UInputModifier::StaticClass()) ? TEXT("input modifier") : TEXT("input trigger");
	}

	/** Whether agents can name @p Class: concrete, current, not an engine-internal helper (the chord blocker). */
	bool IsNameable(const UClass* Class, const UClass* BaseClass)
	{
		return Class->IsChildOf(BaseClass) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_HideDropDown)
			&& !Class->GetName().StartsWith(TEXT("SKEL_")) && !Class->GetName().StartsWith(TEXT("REINST_"));
	}

	/** Extra names agents use: the editor's display names cover most ("Chorded Action", "Swizzle Input Axis Values"). */
	const TCHAR* Alias(const FString& CompactName)
	{
		if (CompactName == TEXT("swizzle"))
			return TEXT("swizzleaxis");
		if (CompactName == TEXT("chord") || CompactName == TEXT("chorded"))
			return TEXT("chordaction");
		if (CompactName == TEXT("hold&release") || CompactName == TEXT("holdrelease"))
			return TEXT("holdandrelease");
		return nullptr;
	}

	UClass* FindClass(const FString& Name, UClass* BaseClass)
	{
		FString Wanted = Compact(Name);
		if (const TCHAR* Aliased = Alias(Wanted))
			Wanted = Aliased;
		const FString PrefixCompact = Compact(Prefix(BaseClass));
		for (UClass* Class : UnrealAIEnhancedInputText::Classes(BaseClass))
		{
			const FString ClassName = Compact(Class->GetName());
			if (ClassName == Wanted || ClassName == PrefixCompact + Wanted || Compact(UnrealAIEnhancedInputText::ShortName(Class)) == Wanted)
				return Class;
#if WITH_EDITOR
			FString Display = Compact(Class->GetMetaData(TEXT("DisplayName")));
			Display.ReplaceInline(TEXT("(beta)"), TEXT(""));
			if (!Display.IsEmpty() && Display == Wanted)
				return Class;
#endif
		}
		// A Blueprint trigger or modifier by path or class name.
		UClass* Resolved = Name.Contains(TEXT("/")) || Name.EndsWith(TEXT("_C")) ? UnrealMcpRql::ResolveClass(Name) : nullptr;
		return Resolved && IsNameable(Resolved, BaseClass) ? Resolved : nullptr;
	}

	/** The settings agents can write: editable, not transient or deprecated. */
	bool IsSetting(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst);
	}

	FProperty* FindSetting(const UClass* Class, const FString& Name)
	{
		const FString Wanted = Compact(Name);
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			if (!IsSetting(*It))
				continue;
			const FString Property = Compact(It->GetName());
			// bIsOneShot answers to "IsOneShot" and "OneShot"; bX to "X".
			FString Bare = Property;
			if (CastField<FBoolProperty>(*It) && Bare.StartsWith(TEXT("b")))
			{
				Bare.RightChopInline(1);
				if (Wanted == Bare || (Bare.StartsWith(TEXT("is")) && Wanted == Bare.RightChop(2)))
					return *It;
			}
			if (Property == Wanted)
				return *It;
#if WITH_EDITOR
			if (Compact(It->GetMetaData(TEXT("DisplayName"))) == Wanted)
				return *It;
#endif
		}
		return nullptr;
	}

	/** The setting a positional argument sets: "Hold(0.5)" is the hold time. Null for classes without one. */
	FProperty* MainSetting(const UClass* Class)
	{
		static const TPair<const TCHAR*, const TCHAR*> Main[] = {
			{ TEXT("InputTriggerDown"), TEXT("ActuationThreshold") },
			{ TEXT("InputTriggerPressed"), TEXT("ActuationThreshold") },
			{ TEXT("InputTriggerReleased"), TEXT("ActuationThreshold") },
			{ TEXT("InputTriggerHold"), TEXT("HoldTimeThreshold") },
			{ TEXT("InputTriggerHoldAndRelease"), TEXT("HoldTimeThreshold") },
			{ TEXT("InputTriggerTap"), TEXT("TapReleaseTimeThreshold") },
			{ TEXT("InputTriggerRepeatedTap"), TEXT("RepeatDelay") },
			{ TEXT("InputTriggerPulse"), TEXT("Interval") },
			{ TEXT("InputTriggerChordAction"), TEXT("ChordAction") },
			{ TEXT("InputModifierDeadZone"), TEXT("LowerThreshold") },
			{ TEXT("InputModifierScalar"), TEXT("Scalar") },
			{ TEXT("InputModifierSwizzleAxis"), TEXT("Order") },
			{ TEXT("InputModifierResponseCurveExponential"), TEXT("CurveExponent") },
			{ TEXT("InputModifierFOVScaling"), TEXT("FOVScale") },
			{ TEXT("InputModifierSmoothDelta"), TEXT("SmoothingMethod") },
		};
		for (const UClass* It = Class; It; It = It->GetSuperClass())
		{
			for (const TPair<const TCHAR*, const TCHAR*>& Entry : Main)
			{
				if (It->GetName() == Entry.Key)
					return It->FindPropertyByName(Entry.Value);
			}
		}
		return nullptr;
	}

	bool IsCombo(const UClass* Class) { return Class->IsChildOf(UInputTriggerCombo::StaticClass()); }
	bool IsNegate(const UClass* Class) { return Class->IsChildOf(UInputModifierNegate::StaticClass()); }

	/** The entries of an enum property as the error lists them. */
	FString EnumNames(const UEnum* Enum)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Enum && Index < Enum->NumEnums() - 1; ++Index)
		{
#if WITH_EDITOR
			if (Enum->HasMetaData(TEXT("Hidden"), Index))
				continue;
#endif
			Names.Add(Enum->GetNameStringByIndex(Index));
		}
		return FString::Join(Names, TEXT(", "));
	}

	const UEnum* EnumOf(const FProperty* Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			return EnumProperty->GetEnum();
		if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
			return Byte->Enum;
		return nullptr;
	}

	/** Agent text as FProperty::ImportText reads it: vectors from "(1, 2, 3)" or one number, booleans from yes / on / 1. */
	FString ImportableText(const FProperty* Property, const FString& Raw)
	{
		const FString Text = Unquote(Raw);
		if (CastField<FBoolProperty>(Property))
		{
			const FString Lower = Text.ToLower();
			if (Lower == TEXT("true") || Lower == TEXT("yes") || Lower == TEXT("on") || Lower == TEXT("1"))
				return TEXT("True");
			if (Lower == TEXT("false") || Lower == TEXT("no") || Lower == TEXT("off") || Lower == TEXT("0"))
				return TEXT("False");
			return Text;
		}
		const FStructProperty* Struct = CastField<FStructProperty>(Property);
		if (Struct && (Struct->Struct == TBaseStructure<FVector>::Get() || Struct->Struct == TBaseStructure<FVector2D>::Get()) && !Text.Contains(TEXT("=")))
		{
			FString Inner = Text;
			if (Inner.StartsWith(TEXT("(")) && Inner.EndsWith(TEXT(")")))
				Inner = Inner.Mid(1, Inner.Len() - 2);
			TArray<FString> Numbers;
			Inner.ParseIntoArray(Numbers, TEXT(","), /*InCullEmpty*/ true);
			const bool b2D = Struct->Struct == TBaseStructure<FVector2D>::Get();
			const int32 Count = b2D ? 2 : 3;
			for (FString& Number : Numbers)
			{
				Number.TrimStartAndEndInline();
				if (!Number.IsNumeric())
					return Text;
			}
			if (Numbers.Num() == 1)
				Numbers.Init(FString(Numbers[0]), Count); // a copy: Init empties the array first
			if (Numbers.Num() != Count)
				return Text;
			return b2D ? FString::Printf(TEXT("(X=%s,Y=%s)"), *Numbers[0], *Numbers[1])
				: FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *Numbers[0], *Numbers[1], *Numbers[2]);
		}
		return Text;
	}

	/** Whether @p Text imports into @p Property as a whole (into a scratch value: nothing changes). */
	bool Imports(const FProperty* Property, const FString& Text)
	{
		void* Scratch = FMemory::Malloc(FMath::Max(Property->GetSize(), 1), Property->GetMinAlignment());
		Property->InitializeValue(Scratch);
		const TCHAR* End = Property->ImportText_Direct(*Text, Scratch, nullptr, PPF_None);
		const bool bWhole = End && FString(End).TrimStartAndEnd().IsEmpty();
		Property->DestroyValue(Scratch);
		FMemory::Free(Scratch);
		return bWhole;
	}

	/** Validate @p Raw for @p Property of @p Entry's class and add it to the entry; false after Fail. */
	bool AddSetting(UnrealAIEnhancedInputText::FEntry& Entry, FProperty* Property, const FString& Raw)
	{
		const FString Short = UnrealAIEnhancedInputText::ShortName(Entry.Class);
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const FString Path = Unquote(Raw);
			const bool bNone = Path.IsEmpty() || Path.Equals(TEXT("None"), ESearchCase::IgnoreCase);
			UObject* Object = bNone ? nullptr : UnrealMcpRql::LoadObject(ObjectProperty->PropertyClass, Path);
			if (!Object && !bNone)
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("%s.%s: no %s at '%s' (an asset path such as /Game/Input/IA_Sprint)"), *Short, *Property->GetName(),
					*ObjectProperty->PropertyClass->GetName(), *Path));
				return false;
			}
			Entry.Objects.Emplace(Property, Object);
			return true;
		}
		const FString Text = ImportableText(Property, Raw);
		if (!Imports(Property, Text))
		{
			const UEnum* Enum = EnumOf(Property);
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s.%s: '%s' is not a valid value%s"), *Short, *Property->GetName(), *Unquote(Raw),
				Enum ? *FString::Printf(TEXT(" (one of %s)"), *EnumNames(Enum))
				: CastField<FBoolProperty>(Property) ? TEXT(" (true or false)")
				: CastField<FStructProperty>(Property) ? TEXT(" (one number, or \"(x, y, z)\")")
				: TEXT(" (a number)")));
			return false;
		}
		Entry.Values.Emplace(Property, Text);
		return true;
	}

	/** One "Name(args)" entry; false after Fail. */
	bool ParseEntry(const FString& Item, UClass* BaseClass, UnrealAIEnhancedInputText::FEntry& Entry)
	{
		FString Name = Item;
		FString Arguments;
		int32 Open = INDEX_NONE;
		if (Item.FindChar(TEXT('('), Open))
		{
			if (!Item.EndsWith(TEXT(")")))
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("'%s': a closing parenthesis is missing (e.g. Hold(0.5) or DeadZone(LowerThreshold=0.2))"), *Item));
				return false;
			}
			Name = Item.Left(Open);
			Arguments = Item.Mid(Open + 1, Item.Len() - Open - 2);
		}
		Name.TrimStartAndEndInline();
		Entry.Class = FindClass(Name, BaseClass);
		if (!Entry.Class)
		{
			TArray<FString> Names;
			for (const UClass* Class : UnrealAIEnhancedInputText::Classes(BaseClass))
				Names.Add(UnrealAIEnhancedInputText::ShortName(Class));
			UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not an %s (%ss: %s, or a Blueprint class path)"), *Name, KindName(BaseClass),
				KindName(BaseClass), *FString::Join(Names, TEXT(", "))));
			return false;
		}

		TArray<FString> Positional;
		for (const FString& Argument : SplitTopLevel(Arguments))
		{
			int32 Equals = INDEX_NONE;
			const bool bNamed = Argument.FindChar(TEXT('='), Equals) && !Argument.StartsWith(TEXT("(")) && !Argument.StartsWith(TEXT("\""));
			if (!bNamed)
			{
				Positional.Add(Argument);
				continue;
			}
			const FString Key = Argument.Left(Equals).TrimStartAndEnd();
			FProperty* Property = FindSetting(Entry.Class, Key);
			if (!Property)
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no setting '%s' (settings: %s)"), *UnrealAIEnhancedInputText::ShortName(Entry.Class), *Key,
					*UnrealAIEnhancedInputText::SettingsOf(Entry.Class)));
				return false;
			}
			if (!AddSetting(Entry, Property, Argument.Mid(Equals + 1)))
				return false;
		}
		if (Positional.Num() == 0)
			return true;

		const FString Short = UnrealAIEnhancedInputText::ShortName(Entry.Class);
		if (IsCombo(Entry.Class))
		{
			// Combo(/Game/Input/IA_Light, /Game/Input/IA_Light, /Game/Input/IA_Heavy): the steps in order.
			for (const FString& Step : Positional)
			{
				UInputAction* Action = Cast<UInputAction>(UnrealMcpRql::LoadObject(UInputAction::StaticClass(), Unquote(Step)));
				if (!Action)
				{
					UnrealMcpRql::Fail(FString::Printf(TEXT("Combo: no InputAction at '%s' (the steps are action paths, in order)"), *Unquote(Step)));
					return false;
				}
				Entry.ComboSteps.Add(Action);
			}
			return true;
		}
		if (IsNegate(Entry.Class) && Positional.Num() == 1)
		{
			// Negate(Y): only the named axes.
			const FString Axes = Unquote(Positional[0]).ToUpper();
			bool bValid = !Axes.IsEmpty();
			for (const TCHAR Char : Axes)
				bValid &= Char == TEXT('X') || Char == TEXT('Y') || Char == TEXT('Z');
			if (!bValid)
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("Negate: '%s' is not a set of axes (X, Y, Z, XY, ...: the axes it negates)"), *Positional[0]));
				return false;
			}
			for (const TCHAR* Axis : { TEXT("X"), TEXT("Y"), TEXT("Z") })
			{
				if (!AddSetting(Entry, Entry.Class->FindPropertyByName(*(FString(TEXT("b")) + Axis)), Axes.Contains(Axis) ? TEXT("true") : TEXT("false")))
					return false;
			}
			return true;
		}
		FProperty* Main = MainSetting(Entry.Class);
		if (!Main)
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s takes named settings only, e.g. %s(%s) (settings: %s)"), *Short, *Short,
				TEXT("Name=Value"), *UnrealAIEnhancedInputText::SettingsOf(Entry.Class)));
			return false;
		}
		// Scalar(2, 2, 1) is one vector: the positional parts form the main setting together.
		const bool bVector = CastField<FStructProperty>(Main) != nullptr;
		if (Positional.Num() > 1 && !bVector)
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s takes one positional value (%s); name the others: %s"), *Short, *Main->GetName(),
				*UnrealAIEnhancedInputText::SettingsOf(Entry.Class)));
			return false;
		}
		return AddSetting(Entry, Main, bVector && Positional.Num() > 1 ? FString::Printf(TEXT("(%s)"), *FString::Join(Positional, TEXT(", "))) : Positional[0]);
	}

	/** A setting's value as Describe prints it (and Parse reads it back). */
	FString ValueText(const FProperty* Property, const void* Value)
	{
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
			return Bool->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
		if (const FFloatProperty* Float = CastField<FFloatProperty>(Property))
			return FString::SanitizeFloat(Float->GetPropertyValue(Value), 0);
		if (const FDoubleProperty* Double = CastField<FDoubleProperty>(Property))
			return FString::SanitizeFloat(Double->GetPropertyValue(Value), 0);
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Target = Object->GetObjectPropertyValue(Value);
			return Target ? Target->GetPackage()->GetName() : TEXT("None");
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
		{
			if (Struct->Struct == TBaseStructure<FVector>::Get())
			{
				const FVector& Vector = *static_cast<const FVector*>(Value);
				return FString::Printf(TEXT("(%s, %s, %s)"), *FString::SanitizeFloat(Vector.X, 0), *FString::SanitizeFloat(Vector.Y, 0), *FString::SanitizeFloat(Vector.Z, 0));
			}
		}
		FString Text;
		Property->ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
		return Text;
	}
}

using namespace UnrealAIEnhancedInputTextLocal;

FString UnrealAIEnhancedInputText::ShortName(const UClass* Class)
{
	if (!Class)
		return FString();
	FString Name = Class->GetName();
	Name.RemoveFromEnd(TEXT("_C"));
	if (!Name.RemoveFromStart(TEXT("InputTrigger")))
		Name.RemoveFromStart(TEXT("InputModifier"));
	return Name;
}

TArray<UClass*> UnrealAIEnhancedInputText::Classes(UClass* BaseClass)
{
	TArray<UClass*> Derived;
	GetDerivedClasses(BaseClass, Derived, /*bRecursive*/ true);
	TArray<UClass*> Out = Derived.FilterByPredicate([BaseClass](const UClass* Class) { return IsNameable(Class, BaseClass); });
	Out.Sort([](const UClass& A, const UClass& B) { return ShortName(&A) < ShortName(&B); });
	return Out;
}

FString UnrealAIEnhancedInputText::SettingsOf(const UClass* Class)
{
	TArray<FString> Settings;
	const UObject* Defaults = Class->GetDefaultObject();
	const FProperty* Main = MainSetting(Class);
	const bool bCombo = IsCombo(Class);
	if (bCombo)
		Settings.Add(TEXT("the step actions in order (positional)"));
	if (Main)
		Settings.Add(FString::Printf(TEXT("%s=%s (first argument)"), *Main->GetName(), *ValueText(Main, Main->ContainerPtrToValuePtr<void>(Defaults))));
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		if (IsSetting(*It) && *It != Main && !(bCombo && It->GetFName() == GET_MEMBER_NAME_CHECKED(UInputTriggerCombo, ComboActions)))
			Settings.Add(FString::Printf(TEXT("%s=%s"), *It->GetName(), *ValueText(*It, It->ContainerPtrToValuePtr<void>(Defaults))));
	}
	return Settings.Num() > 0 ? FString::Join(Settings, TEXT(", ")) : FString(TEXT("none"));
}

bool UnrealAIEnhancedInputText::Parse(const FString& Text, UClass* BaseClass, TArray<FEntry>& Out)
{
	for (const FString& Item : SplitTopLevel(Text))
	{
		FEntry Entry;
		if (!ParseEntry(Item, BaseClass, Entry))
			return false;
		Out.Add(MoveTemp(Entry));
	}
	return true;
}

UObject* UnrealAIEnhancedInputText::Build(const FEntry& Entry, UObject* Outer)
{
	UObject* Instance = NewObject<UObject>(Outer, Entry.Class, NAME_None, Outer->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);
	for (const TPair<FProperty*, FString>& Value : Entry.Values)
		Value.Key->ImportText_InContainer(*Value.Value, Instance, Instance, PPF_None);
	for (const TPair<FProperty*, UObject*>& Object : Entry.Objects)
		CastFieldChecked<FObjectPropertyBase>(Object.Key)->SetObjectPropertyValue_InContainer(Instance, Object.Value);
	if (UInputTriggerCombo* Combo = Cast<UInputTriggerCombo>(Instance))
	{
		for (UInputAction* Step : Entry.ComboSteps)
		{
			FInputComboStepData& Data = Combo->ComboActions.AddDefaulted_GetRef();
			Data.ComboStepAction = Step;
		}
	}
	return Instance;
}

FString UnrealAIEnhancedInputText::Describe(const UObject* Instance)
{
	if (!Instance)
		return FString();
	const UClass* Class = Instance->GetClass();
	const UObject* Defaults = Class->GetDefaultObject();
	TArray<FString> Settings;
	const UInputTriggerCombo* Combo = Cast<UInputTriggerCombo>(Instance);
	if (Combo)
	{
		for (const FInputComboStepData& Step : Combo->ComboActions)
			Settings.Add(Step.ComboStepAction ? Step.ComboStepAction->GetPackage()->GetName() : FString(TEXT("None")));
	}
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		if (!IsSetting(*It) || It->Identical_InContainer(Instance, Defaults)
			|| (Combo && It->GetFName() == GET_MEMBER_NAME_CHECKED(UInputTriggerCombo, ComboActions)))
			continue;
		Settings.Add(FString::Printf(TEXT("%s=%s"), *It->GetName(), *ValueText(*It, It->ContainerPtrToValuePtr<void>(Instance))));
	}
	const FString Name = ShortName(Class);
	return Settings.Num() > 0 ? FString::Printf(TEXT("%s(%s)"), *Name, *FString::Join(Settings, TEXT(", "))) : Name;
}
