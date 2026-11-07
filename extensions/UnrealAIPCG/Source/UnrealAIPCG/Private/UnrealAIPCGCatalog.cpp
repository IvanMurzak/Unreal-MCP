// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIPCGCatalog.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIPCGGraphEdit.h"

#include "Algo/AnyOf.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "PCGGraph.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "UObject/UObjectHash.h"
#include "Utils/PCGPreconfiguration.h"

namespace UnrealAIPCGCatalogLocal
{
	using UnrealAIPCG::Normalize;

	/** A palette entry: a settings class default object and its preconfigured variants (fetched once: they are built per call). */
	struct FPaletteEntry
	{
		const UPCGSettings* Settings = nullptr;
		TArray<FPCGPreConfiguredSettingsInfo> Presets;

		/** Offered only as its variants (Get Graph Parameter per type, ...), as the palette does. */
		bool OnlyPresets() const { return !Presets.IsEmpty() && Settings->OnlyExposePreconfiguredSettings(); }
	};

	/** The settings classes the graph editor's palette offers (concrete, not deprecated or hidden, exposed to the library). */
	TArray<FPaletteEntry> Palette()
	{
		TArray<UClass*> Classes;
		GetDerivedClasses(UPCGSettings::StaticClass(), Classes, /*bRecursive*/ true);
		TArray<FPaletteEntry> Out;
		for (const UClass* Class : Classes)
		{
			if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden | CLASS_NewerVersionExists)
				|| Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
				continue;
			const UPCGSettings* Default = Class->GetDefaultObject<UPCGSettings>();
			if (Default && Default->bExposeToLibrary)
				Out.Add({ Default, Default->GetPreconfiguredInfo() });
		}
		return Out;
	}

	const FPCGPreConfiguredSettingsInfo* FindPreset(const TArray<FPCGPreConfiguredSettingsInfo>& Presets, const FString& Normalized)
	{
		return Presets.FindByPredicate([&Normalized](const FPCGPreConfiguredSettingsInfo& Preset) { return Normalize(Preset.Label.ToString()) == Normalized; });
	}

	/** Whether @p Normalized names the node itself: its title, Type, class, default node name or an alias. */
	bool NamesNode(const UPCGSettings& Settings, const FString& Normalized)
	{
		if (Normalize(Settings.GetDefaultNodeTitle().ToString()) == Normalized || Normalize(UnrealAIPCGCatalog::TypeName(Settings.GetClass())) == Normalized
			|| Normalize(Settings.GetClass()->GetName()) == Normalized || Normalize(Settings.GetDefaultNodeName().ToString()) == Normalized)
			return true;
		return Algo::AnyOf(Settings.GetNodeTitleAliases(), [&Normalized](const FText& Alias) { return Normalize(Alias.ToString()) == Normalized; });
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
	for (const FPaletteEntry& Entry : Palette())
	{
		if (!Entry.OnlyPresets())
		{
			UUnrealAIPCGNodeType* Base = Row(*Entry.Settings, Entry.Settings->GetDefaultNodeTitle().ToString(), FText());
			for (const FText& Alias : Entry.Settings->GetNodeTitleAliases())
				Base->Aliases.Add(Alias.ToString());
			Out.Add(Base);
		}
		for (const FPCGPreConfiguredSettingsInfo& Preset : Entry.Presets)
			Out.Add(Row(*Entry.Settings, Preset.Label.ToString(), Preset.Tooltip));
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
	const TArray<FPaletteEntry> Entries = Palette();

	// "Type/Preset": a preconfigured variant of one type, when its title collides with another node's.
	FString TypePart, PresetPart;
	if (Name.Split(TEXT("/"), &TypePart, &PresetPart))
	{
		const FString WantedType = Normalize(TypePart);
		for (const FPaletteEntry& Entry : Entries)
		{
			if (!NamesNode(*Entry.Settings, WantedType))
				continue;
			if (const FPCGPreConfiguredSettingsInfo* Preset = FindPreset(Entry.Presets, Normalize(PresetPart)))
				return (Out = { Entry.Settings->GetClass(), *Preset }), true;
			const FString Labels = FString::JoinBy(Entry.Presets, TEXT(", "), [](const FPCGPreConfiguredSettingsInfo& Preset) { return Preset.Label.ToString(); });
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no variant '%s'%s"), *Entry.Settings->GetDefaultNodeTitle().ToString(), *PresetPart,
				Labels.IsEmpty() ? TEXT(" (it has no variants)") : *FString::Printf(TEXT(" (variants: %s)"), *Labels)));
		}
	}

	// The node itself (a node offered only as variants gives its first one).
	for (const FPaletteEntry& Entry : Entries)
	{
		if (NamesNode(*Entry.Settings, Wanted))
		{
			Out.Class = Entry.Settings->GetClass();
			Out.Preset.Reset();
			if (Entry.OnlyPresets())
				Out.Preset = Entry.Presets[0];
			return true;
		}
	}
	// A preconfigured variant by its own title.
	TArray<FString> Similar;
	for (const FPaletteEntry& Entry : Entries)
	{
		if (const FPCGPreConfiguredSettingsInfo* Preset = FindPreset(Entry.Presets, Wanted))
			return (Out = { Entry.Settings->GetClass(), *Preset }), true;
		const FString Title = Entry.Settings->GetDefaultNodeTitle().ToString();
		const FString Normalized = Normalize(Title);
		if (Similar.Num() < 10 && (Normalized.Contains(Wanted) || Wanted.Contains(Normalized) || Normalize(TypeName(Entry.Settings->GetClass())).Contains(Wanted)))
			Similar.AddUnique(Title);
	}
	return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("no PCG node type '%s'%s; pcgNodeTypes | where Name contains \"...\" | select Name Type Category Description lists them"),
		*Name, Similar.Num() ? *FString::Printf(TEXT(" (similar: %s)"), *FString::Join(Similar, TEXT(", "))) : TEXT("")));
}
