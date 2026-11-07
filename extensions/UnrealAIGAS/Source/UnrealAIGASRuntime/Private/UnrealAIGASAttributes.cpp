// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIGASAttributes.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemTestAttributeSet.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"

UE_DEFINE_GAMEPLAY_TAG_COMMENT(TAG_UnrealAI_SetByCaller_Damage, "SetByCaller.Damage",
	"The damage (or other) amount an Unreal AI ability or projectile passes to its hit effect");

namespace UnrealAIGAS
{
	namespace
	{
		/** The class name an agent writes: no U prefix, Blueprint classes keep _C. */
		FString SetName(const UClass* SetClass)
		{
			return SetClass ? SetClass->GetName() : FString();
		}

		bool SetMatches(const UClass* SetClass, const FString& Name)
		{
			const FString Class = SetName(SetClass);
			return Name.Equals(Class, ESearchCase::IgnoreCase) || Name.Equals(TEXT("U") + Class, ESearchCase::IgnoreCase)
				|| (Class.EndsWith(TEXT("_C")) && Name.Equals(Class.LeftChop(2), ESearchCase::IgnoreCase));
		}
	}

	TArray<FGameplayAttribute> AttributesOf(const UClass* SetClass)
	{
		TArray<FGameplayAttribute> Out;
		if (!SetClass)
			return Out;
		for (TFieldIterator<FProperty> It(SetClass); It; ++It)
		{
			if (FGameplayAttribute::IsGameplayAttributeDataProperty(*It))
				Out.Add(FGameplayAttribute(*It));
		}
		return Out;
	}

	FString QualifiedName(const FGameplayAttribute& Attribute)
	{
		return FString::Printf(TEXT("%s.%s"), *SetName(Attribute.GetAttributeSetClass()), *Attribute.GetName());
	}

	FGameplayAttribute FindAttribute(const FString& Name, const TArray<const UClass*>& SetClasses, FString* OutError)
	{
		FString SetPart, AttributePart = Name.TrimStartAndEnd();
		if (AttributePart.Contains(TEXT(".")))
			AttributePart.Split(TEXT("."), &SetPart, &AttributePart, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

		TArray<FGameplayAttribute> Matches;
		TArray<FString> Known;
		for (const UClass* SetClass : SetClasses)
		{
			for (const FGameplayAttribute& Attribute : AttributesOf(SetClass))
			{
				Known.AddUnique(QualifiedName(Attribute));
				if (Attribute.GetName().Equals(AttributePart, ESearchCase::IgnoreCase) && (SetPart.IsEmpty() || SetMatches(SetClass, SetPart)))
					Matches.AddUnique(Attribute);
			}
		}
		if (Matches.Num() == 1)
			return Matches[0];
		if (OutError)
		{
			if (Matches.Num() > 1)
			{
				TArray<FString> Names;
				for (const FGameplayAttribute& Match : Matches)
					Names.Add(QualifiedName(Match));
				*OutError = FString::Printf(TEXT("the attribute '%s' is ambiguous: name one of %s"), *Name, *FString::Join(Names, TEXT(", ")));
			}
			else
			{
				*OutError = Known.IsEmpty()
					? FString::Printf(TEXT("no attribute '%s': no attribute set is loaded (use UnrealAIAttributeSet: Health, MaxHealth, Mana, MaxMana, Stamina, MaxStamina, Damage, Healing)"), *Name)
					: FString::Printf(TEXT("no attribute '%s' (known: %s)"), *Name, *FString::Join(Known, TEXT(", ")));
			}
		}
		return FGameplayAttribute();
	}

	FGameplayAttribute FindAttribute(const UAbilitySystemComponent& Asc, const FString& Name, FString* OutError)
	{
		TArray<const UClass*> Classes;
		for (const UAttributeSet* Set : Asc.GetSpawnedAttributes())
		{
			if (Set)
				Classes.AddUnique(Set->GetClass());
		}
		return FindAttribute(Name, Classes, OutError);
	}

	TArray<const UClass*> LoadedAttributeSetClasses()
	{
		TArray<const UClass*> Out;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			const UClass* Class = *It;
			if (!Class->IsChildOf(UAttributeSet::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				|| Class->IsChildOf(UAbilitySystemTestAttributeSet::StaticClass()) || Class->GetName().StartsWith(TEXT("SKEL_"))
				|| Class->GetName().StartsWith(TEXT("REINST_")))
				continue;
			Out.Add(Class);
		}
		return Out;
	}
}

IMPLEMENT_MODULE(FDefaultModuleImpl, UnrealAIGASRuntime)
