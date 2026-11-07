// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIEnhancedInputRows.h"

#include "UnrealAIEnhancedInputText.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "PlayerMappableKeySettings.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"

namespace UnrealAIEnhancedInputRowsLocal
{
	TSharedPtr<FJsonObject> SettingsJson(FName Name, const FText& DisplayName, const FText& Category)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("Name"), Name.ToString());
		Out->SetStringField(TEXT("DisplayName"), DisplayName.ToString());
		Out->SetStringField(TEXT("DisplayCategory"), Category.ToString());
		return Out;
	}

	void AddTypes(TArray<UObject*>& Out, UClass* BaseClass, const TCHAR* Kind)
	{
		for (UClass* Class : UnrealAIEnhancedInputText::Classes(BaseClass))
		{
			UUnrealAIEnhancedInputTypeRow* Row = NewObject<UUnrealAIEnhancedInputTypeRow>(GetTransientPackage());
			Row->Name = UnrealAIEnhancedInputText::ShortName(Class);
			Row->Kind = Kind;
			Row->Class = Class->GetName();
			Row->Settings = UnrealAIEnhancedInputText::SettingsOf(Class);
#if WITH_EDITOR
			Row->Description = Class->GetToolTipText(/*bShortTooltip*/ true).ToString();
#endif
			Out.Add(Row);
		}
	}
}

using namespace UnrealAIEnhancedInputRowsLocal;

FString UnrealAIEnhancedInputRows::SettingBehavior(const FEnhancedActionKeyMapping& Mapping)
{
	// Protected in FEnhancedActionKeyMapping: read through reflection, as the Details panel does.
	static const FEnumProperty* Property = CastField<FEnumProperty>(FEnhancedActionKeyMapping::StaticStruct()->FindPropertyByName(TEXT("SettingBehavior")));
	if (!Property)
		return TEXT("Inherit");
	const int64 Value = Property->GetUnderlyingProperty()->GetSignedIntPropertyValue(Property->ContainerPtrToValuePtr<void>(&Mapping));
	switch (static_cast<EPlayerMappableKeySettingBehaviors>(Value))
	{
	case EPlayerMappableKeySettingBehaviors::OverrideSettings: return TEXT("Override");
	case EPlayerMappableKeySettingBehaviors::IgnoreSettings: return TEXT("Ignore");
	default: return TEXT("Inherit");
	}
}

TSharedPtr<FJsonValue> UnrealAIEnhancedInputRows::PlayerMappableJson(const UInputAction* Action)
{
	const UPlayerMappableKeySettings* Settings = Action ? Action->GetPlayerMappableKeySettings().Get() : nullptr;
	if (!Settings)
		return MakeShared<FJsonValueNull>();
	return MakeShared<FJsonValueObject>(SettingsJson(Settings->Name, Settings->DisplayName, Settings->DisplayCategory));
}

TSharedPtr<FJsonValue> UnrealAIEnhancedInputRows::MappingJson(const FEnhancedActionKeyMapping& Mapping)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("Action"), Mapping.Action ? Mapping.Action->GetName() : FString());
	Entry->SetStringField(TEXT("Key"), Mapping.Key.GetFName().ToString());
	const FString Triggers = UnrealAIEnhancedInputText::DescribeList(Mapping.Triggers);
	if (!Triggers.IsEmpty())
		Entry->SetStringField(TEXT("Triggers"), Triggers);
	const FString Modifiers = UnrealAIEnhancedInputText::DescribeList(Mapping.Modifiers);
	if (!Modifiers.IsEmpty())
		Entry->SetStringField(TEXT("Modifiers"), Modifiers);
	if (Mapping.IsPlayerMappable())
	{
		TSharedPtr<FJsonObject> Settings = SettingsJson(Mapping.GetMappingName(), Mapping.GetDisplayName(), Mapping.GetDisplayCategory());
		Settings->SetStringField(TEXT("From"), SettingBehavior(Mapping) == TEXT("Override") ? TEXT("mapping") : TEXT("action"));
		Entry->SetObjectField(TEXT("PlayerMappable"), Settings);
	}
	return MakeShared<FJsonValueObject>(Entry);
}

TArray<UObject*> UnrealAIEnhancedInputRows::Mappings()
{
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByClass(UInputMappingContext::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses*/ true);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	TArray<UObject*> Rows;
	for (int32 Index = 0; Index < Assets.Num() && Index < 200; ++Index)
	{
		const UInputMappingContext* Context = Cast<UInputMappingContext>(Assets[Index].GetAsset());
		if (!Context)
			continue;
		for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
		{
			UUnrealAIEnhancedInputMappingRow* Row = NewObject<UUnrealAIEnhancedInputMappingRow>(GetTransientPackage());
			Row->Context = Context->GetName();
			Row->ContextPath = Context->GetPackage()->GetName();
			Row->Action = Mapping.Action ? Mapping.Action->GetName() : FString();
			Row->ActionPath = Mapping.Action ? Mapping.Action->GetPackage()->GetName() : FString();
			Row->Key = Mapping.Key.GetFName().ToString();
			Row->Triggers = UnrealAIEnhancedInputText::DescribeList(Mapping.Triggers);
			Row->Modifiers = UnrealAIEnhancedInputText::DescribeList(Mapping.Modifiers);
			Row->PlayerMappable = Mapping.IsPlayerMappable();
			Row->Settings = SettingBehavior(Mapping);
			if (Row->PlayerMappable)
			{
				Row->MappingName = Mapping.GetMappingName().ToString();
				Row->DisplayName = Mapping.GetDisplayName().ToString();
				Row->DisplayCategory = Mapping.GetDisplayCategory().ToString();
			}
			Rows.Add(Row);
		}
	}
	return Rows;
}

TArray<UObject*> UnrealAIEnhancedInputRows::Types(bool bTriggers)
{
	TArray<UObject*> Out;
	if (bTriggers)
		AddTypes(Out, UInputTrigger::StaticClass(), TEXT("Trigger"));
	else
		AddTypes(Out, UInputModifier::StaticClass(), TEXT("Modifier"));
	return Out;
}
