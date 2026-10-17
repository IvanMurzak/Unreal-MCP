// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIEnhancedInputRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "UObject/Package.h"

namespace UnrealAIEnhancedInputRqlLocal
{
	/** A new asset object in its own package at "/Game/Folder/Name"; nullptr + RQL failure on a bad / taken path. */
	template <typename T>
	T* CreateInputAsset(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), Dot))
			Path.LeftInline(Dot);
		int32 Slash = INDEX_NONE;
		const bool bShape = Path.StartsWith(TEXT("/")) && Path.FindLastChar(TEXT('/'), Slash) && Slash > 0 && Slash < Path.Len() - 1;
		const bool bEngine = Path.StartsWith(TEXT("/Engine/")) || Path.StartsWith(TEXT("/Script/")) || Path.StartsWith(TEXT("/Temp/"));
		if (!bShape || bEngine)
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a writable asset path (e.g. /Game/Input/IA_Jump)"), *InPath));
			return nullptr;
		}
		if (UEditorAssetLibrary::DoesAssetExist(Path))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' already exists"), *Path));
			return nullptr;
		}
		UPackage* Package = CreatePackage(*Path);
		T* Asset = Package ? NewObject<T>(Package, T::StaticClass(), *Path.Mid(Slash + 1), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
		if (!Asset)
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("could not create '%s'"), *Path));
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}

	bool ValidKey(const FString& Key, FKey& Out)
	{
		Out = FKey(*Key.TrimStartAndEnd());
		if (Out.IsValid())
			return true;
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a key name (e.g. SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom)"), *Key));
		return false;
	}
}

UInputAction* UUnrealAIEnhancedInputRqlApi::CreateAction(const FString& Path, EInputActionValueType ValueType)
{
	UInputAction* Action = UnrealAIEnhancedInputRqlLocal::CreateInputAsset<UInputAction>(Path);
	if (Action)
		Action->ValueType = ValueType;
	return Action;
}

UInputMappingContext* UUnrealAIEnhancedInputRqlApi::CreateContext(const FString& Path)
{
	return UnrealAIEnhancedInputRqlLocal::CreateInputAsset<UInputMappingContext>(Path);
}

bool UUnrealAIEnhancedInputRqlApi::AddMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key)
{
	FKey Parsed;
	if (!Context || !Action)
	{
		UnrealMcpRql::Fail(TEXT("a mapping context and an input action are required (assets or their paths)"));
		return false;
	}
	if (!UnrealAIEnhancedInputRqlLocal::ValidKey(Key, Parsed))
		return false;
	Context->Modify();
	Context->MapKey(Action, Parsed);
	Context->MarkPackageDirty();
	return true;
}

int32 UUnrealAIEnhancedInputRqlApi::RemoveMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key)
{
	FKey Parsed;
	if (!Context || !Action)
	{
		UnrealMcpRql::Fail(TEXT("a mapping context and an input action are required (assets or their paths)"));
		return 0;
	}
	if (!UnrealAIEnhancedInputRqlLocal::ValidKey(Key, Parsed))
		return 0;
	int32 Removed = 0;
	for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
	{
		if (Mapping.Action == Action && Mapping.Key == Parsed)
			++Removed;
	}
	if (Removed > 0)
	{
		Context->Modify();
		for (int32 Index = 0; Index < Removed; ++Index)
			Context->UnmapKey(Action, Parsed); // one mapping per call
		Context->MarkPackageDirty();
	}
	return Removed;
}
