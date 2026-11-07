// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FProperty;
class UClass;
class UInputAction;
class UObject;

/**
 * Triggers and modifiers as text, the form agents write and read back: "Hold(0.5), Pressed" or
 * "DeadZone(LowerThreshold=0.25, Type=Axial), Scalar(2)". An entry is a class (its short name: "Hold" for
 * InputTriggerHold, "Negate" for InputModifierNegate, the editor's display name, or a Blueprint class path) with optional
 * settings: `Name=Value` for any editable property, or one positional value for the class's main setting (see
 * MainSetting). Values are text as the Details panel takes them, numbers, `true` / `false`, enum entries, asset paths
 * and vectors as one number or "(x, y, z)".
 */
namespace UnrealAIEnhancedInputText
{
	/** One parsed and validated entry: Build cannot fail. */
	struct FEntry
	{
		UClass* Class = nullptr;
		/** Settings as the text FProperty::ImportText takes (validated against a scratch value). */
		TArray<TPair<FProperty*, FString>> Values;
		/** Object settings (ChordAction), loaded. */
		TArray<TPair<FProperty*, UObject*>> Objects;
		/** A Combo trigger's steps, in order. */
		TArray<UInputAction*> ComboSteps;
	};

	/**
	 * Parse a comma-separated list of @p BaseClass (UInputTrigger or UInputModifier) entries; an empty text is an empty
	 * list. False after UnrealMcpRql::Fail, naming the known classes or the class's settings.
	 */
	bool Parse(const FString& Text, UClass* BaseClass, TArray<FEntry>& Out);

	/** The entry as an instanced subobject of @p Outer (as the editor's instanced arrays create them). */
	UObject* Build(const FEntry& Entry, UObject* Outer);

	/** Every entry of @p Entries built into @p Outer, as @p T. */
	template <typename T>
	TArray<TObjectPtr<T>> BuildAll(const TArray<FEntry>& Entries, UObject* Outer)
	{
		TArray<TObjectPtr<T>> Out;
		for (const FEntry& Entry : Entries)
			Out.Add(CastChecked<T>(Build(Entry, Outer)));
		return Out;
	}

	/** "Hold(HoldTimeThreshold=0.5)": the short name and the settings that differ from the class defaults (Parse reads it back). */
	FString Describe(const UObject* Instance);

	/** The instances as one text list, as Parse reads it: "SwizzleAxis, Negate" ("" for none; nulls skipped). */
	template <typename T>
	FString DescribeList(const TArray<TObjectPtr<T>>& Instances)
	{
		TArray<FString> Out;
		for (const T* Instance : Instances)
		{
			if (Instance)
				Out.Add(Describe(Instance));
		}
		return FString::Join(Out, TEXT(", "));
	}

	/** The short name of a trigger or modifier class: "Hold" for InputTriggerHold, "Negate" for InputModifierNegate. */
	FString ShortName(const UClass* Class);

	/** The concrete trigger or modifier classes an agent can name (native and loaded Blueprint ones), sorted by short name. */
	TArray<UClass*> Classes(UClass* BaseClass);

	/** The settings @p Class accepts with their defaults, its main (positional) setting first: "HoldTimeThreshold=1 (first argument), bIsOneShot=false, ...". */
	FString SettingsOf(const UClass* Class);
}
