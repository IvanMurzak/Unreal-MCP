// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAINiagaraTemplates.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"

namespace UnrealAINiagaraTemplatesLocal
{
	const TCHAR* FluidsPlugin = TEXT("NiagaraFluids");

	/** The template folders of the engine's Niagara plugins: every asset below them is a template. */
	const TCHAR* TemplateRoots[] = {
		TEXT("/Niagara/DefaultAssets/Templates/"),
		TEXT("/NiagaraFluids/Templates/"),
	};

	/** What the Niagara Fluids templates make, by name part, when the asset carries no description of its own. */
	FString FluidsDescription(const FString& Name, const FString& Category)
	{
		const bool b3D = Category.Contains(TEXT("3D"));
		const FString Grid = b3D ? TEXT("3D grid (volumetric, GPU)") : Category.Contains(TEXT("2D")) ? TEXT("2D grid (GPU)") : TEXT("GPU");
		if (Category.StartsWith(TEXT("Gas")))
		{
			FString What = TEXT("gas simulation");
			if (Name.Contains(TEXT("Fire")))
				What = TEXT("fire with smoke: a gas simulation driven by temperature; flows around scene geometry (World Collision)");
			else if (Name.Contains(TEXT("Smoke")))
				What = TEXT("smoke: a gas simulation that flows around scene geometry (World Collision)");
			else if (Name.Contains(TEXT("Explosion")))
				What = TEXT("explosion: a gas burst with fire and smoke");
			return FString::Printf(TEXT("Niagara Fluids %s on a %s"), *What, *Grid);
		}
		if (Category.StartsWith(TEXT("Liquid")))
			return FString::Printf(TEXT("Niagara Fluids liquid simulation (%s) on a %s"), *Name, *Grid);
		return FString::Printf(TEXT("Niagara Fluids %s (%s)"), *Category.ToLower(), *Grid);
	}

	void Add(TArray<UObject*>& Out, const FAssetData& Data, const TCHAR* Kind)
	{
		const FString Path = Data.PackageName.ToString();
		FString Root;
		for (const TCHAR* Candidate : TemplateRoots)
		{
			if (Path.StartsWith(Candidate))
				Root = Candidate;
		}
		if (Root.IsEmpty())
		{
			// Project and marketplace templates carry the TemplateSpecification tag (Template or Behavior).
			FString Specification;
			if (!Data.GetTagValue(FName(TEXT("TemplateSpecification")), Specification)
				|| !(Specification == TEXT("Template") || Specification == TEXT("Behavior")))
				return;
		}
		UUnrealAINiagaraTemplate* Row = NewObject<UUnrealAINiagaraTemplate>(GetTransientPackage());
		Row->Name = Data.AssetName.ToString();
		Row->Path = Path;
		Row->Kind = Kind;
		Row->Plugin = Path.Mid(1, Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1) - 1);
		Row->Category = Root.IsEmpty() ? FPaths::GetPath(Path) : FPaths::GetPath(Path.Mid(Root.Len()));
		FText Description;
		if (Data.GetTagValue(FName(TEXT("TemplateAssetDescription")), Description))
			Row->Description = Description.ToString();
		if (Row->Description.IsEmpty() && Row->Plugin == FluidsPlugin)
			Row->Description = FluidsDescription(Row->Name, Row->Category);
		if (Row->Description.IsEmpty() && Row->Name.EndsWith(TEXT("Lightweight")))
			Row->Description = FString::Printf(TEXT("a lightweight (stateless, cheaper, fewer modules) version of %s"), *Row->Name.LeftChop(11));
		if (Row->Description.IsEmpty())
			Row->Description = FString::Printf(TEXT("%s %s template"), *Row->Plugin, *Row->Kind.ToLower());
		if (Row->Plugin == FluidsPlugin && !UnrealAINiagaraTemplates::IsFluidsEnabled())
		{
			// Its content is mounted, but its modules use ChaosNiagara types that are not loaded.
			Row->Available = false;
			Row->Description += TEXT(" (unavailable until niagara | call EnableFluids() and an editor restart)");
		}
		Out.Add(Row);
	}
}

bool UnrealAINiagaraTemplates::IsFluidsInstalled()
{
	return IPluginManager::Get().FindPlugin(UnrealAINiagaraTemplatesLocal::FluidsPlugin).IsValid();
}

bool UnrealAINiagaraTemplates::IsFluidsEnabled()
{
	// The Fluids modules use ChaosNiagara's data interfaces without declaring the dependency: both must be loaded.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(UnrealAINiagaraTemplatesLocal::FluidsPlugin);
	const TSharedPtr<IPlugin> Chaos = IPluginManager::Get().FindPlugin(TEXT("ChaosNiagara"));
	return Plugin.IsValid() && Plugin->IsEnabled() && Chaos.IsValid() && Chaos->IsEnabled();
}

FString UnrealAINiagaraTemplates::DisabledPluginMessage(const FString& Path)
{
	if (!Path.StartsWith(TEXT("/NiagaraFluids/")) || IsFluidsEnabled())
		return FString();
	return IsFluidsInstalled()
		? TEXT("the NiagaraFluids plugin (with ChaosNiagara, which its modules use) is not enabled in this project: niagara | call EnableFluids(), then restart the editor; until then use a Niagara template (niagaraTemplates | where Plugin == \"Niagara\")")
		: TEXT("the NiagaraFluids plugin is not installed with this engine (it ships with Unreal Engine 5.1+ under Engine/Plugins/FX/NiagaraFluids)");
}

TArray<UObject*> UnrealAINiagaraTemplates::List()
{
	using namespace UnrealAINiagaraTemplatesLocal;
	TArray<UObject*> Out;
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UNiagaraSystem::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& Data : Assets)
		Add(Out, Data, TEXT("System"));
	Assets.Reset();
	Registry.GetAssetsByClass(UNiagaraEmitter::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& Data : Assets)
		Add(Out, Data, TEXT("Emitter"));

	if (IsFluidsInstalled() && !IsFluidsEnabled())
	{
		UUnrealAINiagaraTemplate* Row = NewObject<UUnrealAINiagaraTemplate>(GetTransientPackage());
		Row->Name = FluidsPlugin;
		Row->Kind = TEXT("Plugin");
		Row->Plugin = FluidsPlugin;
		Row->Available = false;
		Row->Description = TEXT("Niagara Fluids templates (volumetric 3D gas: fire, smoke, explosions that flow around scene geometry; 2D gas; liquids) are installed but not enabled (they need the NiagaraFluids and ChaosNiagara plugins): niagara | call EnableFluids(), then restart the editor");
		Out.Add(Row);
	}

	Out.Sort([](const UObject& A, const UObject& B)
	{
		const UUnrealAINiagaraTemplate& L = static_cast<const UUnrealAINiagaraTemplate&>(A);
		const UUnrealAINiagaraTemplate& R = static_cast<const UUnrealAINiagaraTemplate&>(B);
		if (L.Plugin != R.Plugin)
			return L.Plugin < R.Plugin;
		if (L.Kind != R.Kind)
			return L.Kind > R.Kind; // System before Plugin before Emitter: whole effects first
		if (L.Category != R.Category)
			return L.Category < R.Category;
		return L.Name < R.Name;
	});
	return Out;
}
