// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIPCGCatalog.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIPCGGraphEdit.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "PCGGraph.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "UObject/UObjectIterator.h"
#include "Utils/PCGPreconfiguration.h"

namespace UnrealAIPCGCatalogLocal
{
	using UnrealAIPCG::Normalize;

	/** The settings classes the graph editor's palette offers (concrete, not deprecated or hidden, exposed to the library). */
	TArray<const UPCGSettings*> PaletteSettings()
	{
		TArray<const UPCGSettings*> Out;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UPCGSettings::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden | CLASS_NewerVersionExists)
				|| It->GetName().StartsWith(TEXT("SKEL_")) || It->GetName().StartsWith(TEXT("REINST_")))
				continue;
			const UPCGSettings* Default = It->GetDefaultObject<UPCGSettings>();
			if (Default && Default->bExposeToLibrary)
				Out.Add(Default);
		}
		return Out;
	}

	FString Category(const UPCGSettings& Settings)
	{
		return StaticEnum<EPCGSettingsType>()->GetDisplayNameTextByValue(static_cast<int64>(Settings.GetType())).ToString();
	}

	/** The labels of the pins a new node has (override pins come with settings: a node's Settings member names them). */
	TArray<FString> PinLabels(const TArray<FPCGPinProperties>& Pins)
	{
		TArray<FString> Out;
		for (const FPCGPinProperties& Pin : Pins)
		{
			if (!Pin.bInvisiblePin)
				Out.Add(Pin.Label.ToString());
		}
		return Out;
	}

	/** The first sentence-ish of a tooltip: palette tooltips run to paragraphs. */
	FString Short(const FText& Tooltip)
	{
		FString Text = Tooltip.ToString().Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
		while (Text.Contains(TEXT("  ")))
			Text.ReplaceInline(TEXT("  "), TEXT(" "));
		return Text.Len() > 240 ? Text.Left(237) + TEXT("...") : Text;
	}

	UUnrealAIPCGNodeType* Row(const UPCGSettings& Settings, const FString& Name, const FText& Tooltip)
	{
		UUnrealAIPCGNodeType* Row = NewObject<UUnrealAIPCGNodeType>(GetTransientPackage());
		Row->Name = Name;
		Row->Type = UnrealAIPCGCatalog::TypeName(Settings.GetClass());
		Row->Category = Category(Settings);
		Row->Description = Short(Tooltip.IsEmpty() ? Settings.GetNodeTooltipText() : Tooltip);
		Row->Inputs = PinLabels(Settings.DefaultInputPinProperties());
		Row->Outputs = PinLabels(Settings.DefaultOutputPinProperties());
		return Row;
	}
}

FString UnrealAIPCGCatalog::TypeName(const UClass* SettingsClass)
{
	if (!SettingsClass)
		return FString();
	FString Name = SettingsClass->GetName();
	Name.RemoveFromEnd(TEXT("_C"));
	Name.RemoveFromStart(TEXT("PCG"));
	Name.RemoveFromEnd(TEXT("Settings"));
	return Name;
}

TArray<UObject*> UnrealAIPCGCatalog::NodeTypes()
{
	using namespace UnrealAIPCGCatalogLocal;
	TArray<UObject*> Out;
	for (const UPCGSettings* Settings : PaletteSettings())
	{
		const TArray<FPCGPreConfiguredSettingsInfo> Presets = Settings->GetPreconfiguredInfo();
		if (Presets.IsEmpty() || !Settings->OnlyExposePreconfiguredSettings())
		{
			UUnrealAIPCGNodeType* Base = Row(*Settings, Settings->GetDefaultNodeTitle().ToString(), FText());
			for (const FText& Alias : Settings->GetNodeTitleAliases())
				Base->Aliases.Add(Alias.ToString());
			Out.Add(Base);
		}
		for (const FPCGPreConfiguredSettingsInfo& Preset : Presets)
			Out.Add(Row(*Settings, Preset.Label.ToString(), Preset.Tooltip));
	}
	Out.Sort([](const UObject& A, const UObject& B)
	{
		const UUnrealAIPCGNodeType& L = static_cast<const UUnrealAIPCGNodeType&>(A);
		const UUnrealAIPCGNodeType& R = static_cast<const UUnrealAIPCGNodeType&>(B);
		return L.Category != R.Category ? L.Category < R.Category : L.Name < R.Name;
	});
	return Out;
}

TArray<UObject*> UnrealAIPCGCatalog::Templates()
{
	TArray<UObject*> Out;
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByClass(UPCGGraph::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& Data : Assets)
	{
		bool bTemplate = false;
		if (!Data.GetTagValue(GET_MEMBER_NAME_CHECKED(UPCGGraph, bIsTemplate), bTemplate) || !bTemplate)
			continue;
		UUnrealAIPCGTemplate* Row = NewObject<UUnrealAIPCGTemplate>(GetTransientPackage());
		Row->Name = Data.AssetName.ToString();
		Row->Path = Data.PackageName.ToString();
		FText Text;
		if (Data.GetTagValue(GET_MEMBER_NAME_CHECKED(UPCGGraph, Category), Text))
			Row->Category = Text.ToString();
		if (Data.GetTagValue(GET_MEMBER_NAME_CHECKED(UPCGGraph, Description), Text))
			Row->Description = UnrealAIPCGCatalogLocal::Short(Text);
		if (Row->Description.IsEmpty())
			Row->Description = TEXT("PCG graph template");
		Out.Add(Row);
	}
	Out.Sort([](const UObject& A, const UObject& B)
	{
		return static_cast<const UUnrealAIPCGTemplate&>(A).Name < static_cast<const UUnrealAIPCGTemplate&>(B).Name;
	});
	return Out;
}

bool UnrealAIPCGCatalog::FindNodeType(const FString& Name, FNodeType& Out)
{
	using namespace UnrealAIPCGCatalogLocal;
	const FString Wanted = Normalize(Name);
	if (Wanted.IsEmpty())
		return UnrealMcpRql::Failed<bool>(TEXT("the node type is empty (pcgNodeTypes | select Name Type Category Description lists them)"));
	// "Type/Preset" picks a preconfigured variant of a type whose titles collide with another node's.
	FString TypePart = Name, PresetPart;
	Name.Split(TEXT("/"), &TypePart, &PresetPart);
	const TArray<const UPCGSettings*> Palette = PaletteSettings();
	const auto ClassMatches = [](const UPCGSettings& Settings, const FString& Normalized)
	{
		if (Normalize(Settings.GetDefaultNodeTitle().ToString()) == Normalized || Normalize(TypeName(Settings.GetClass())) == Normalized
			|| Normalize(Settings.GetClass()->GetName()) == Normalized || Normalize(Settings.GetDefaultNodeName().ToString()) == Normalized)
			return true;
		for (const FText& Alias : Settings.GetNodeTitleAliases())
		{
			if (Normalize(Alias.ToString()) == Normalized)
				return true;
		}
		return false;
	};
	TArray<FString> Similar;
	// Pass 1: the node itself (unless it only offers its variants); pass 2: a preconfigured variant by its own title.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (const UPCGSettings* Settings : Palette)
		{
			const TArray<FPCGPreConfiguredSettingsInfo> Presets = Settings->GetPreconfiguredInfo();
			const bool bOnlyPresets = !Presets.IsEmpty() && Settings->OnlyExposePreconfiguredSettings();
			if (Pass == 0)
			{
				if (!PresetPart.IsEmpty())
				{
					if (!ClassMatches(*Settings, Normalize(TypePart)))
						continue;
					for (const FPCGPreConfiguredSettingsInfo& Preset : Presets)
					{
						if (Normalize(Preset.Label.ToString()) == Normalize(PresetPart))
						{
							Out = { Settings->GetClass(), Preset.PreconfiguredIndex, Preset.Label };
							return true;
						}
					}
					TArray<FString> Labels;
					for (const FPCGPreConfiguredSettingsInfo& Preset : Presets)
						Labels.Add(Preset.Label.ToString());
					return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no variant '%s'%s"), *Settings->GetDefaultNodeTitle().ToString(), *PresetPart,
						Labels.Num() ? *FString::Printf(TEXT(" (variants: %s)"), *FString::Join(Labels, TEXT(", "))) : TEXT(" (it has no variants)")));
				}
				if (ClassMatches(*Settings, Wanted))
				{
					if (!bOnlyPresets)
					{
						Out = { Settings->GetClass(), INDEX_NONE, FText() };
						return true;
					}
					// A node offered only as variants (Get Graph Parameter per type, ...): its first one.
					Out = { Settings->GetClass(), Presets[0].PreconfiguredIndex, Presets[0].Label };
					return true;
				}
			}
			else
			{
				for (const FPCGPreConfiguredSettingsInfo& Preset : Presets)
				{
					if (Normalize(Preset.Label.ToString()) == Wanted)
					{
						Out = { Settings->GetClass(), Preset.PreconfiguredIndex, Preset.Label };
						return true;
					}
				}
				const FString Title = Settings->GetDefaultNodeTitle().ToString();
				if (Similar.Num() < 10 && (Normalize(Title).Contains(Wanted) || Wanted.Contains(Normalize(Title)) || Normalize(TypeName(Settings->GetClass())).Contains(Wanted)))
					Similar.AddUnique(Title);
			}
		}
	}
	return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("no PCG node type '%s'%s; pcgNodeTypes | where Name contains \"...\" | select Name Type Category Description lists them"),
		*Name, Similar.Num() ? *FString::Printf(TEXT(" (similar: %s)"), *FString::Join(Similar, TEXT(", "))) : TEXT("")));
}
