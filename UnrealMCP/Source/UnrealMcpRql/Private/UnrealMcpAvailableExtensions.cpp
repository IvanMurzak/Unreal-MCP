// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpAvailableExtensions.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"

#include "Interfaces/IPluginManager.h"

namespace
{
	/**
	 * The first-party extensions (extensions/UnrealAI*): ids, names and plugin names as in their extension.json
	 * (scripts/test_extension_monorepo.py keeps them in step), what they add as their RQL modules register it.
	 */
	const TArray<FUnrealMcpKnownExtension>& Bundled()
	{
		static const TArray<FUnrealMcpKnownExtension> Extensions = {
			{ TEXT("com.ivanmurzak.unreal-ai-niagara"), TEXT("Niagara Tools"), TEXT("UnrealAINiagara"),
				TEXT("Niagara VFX: niagaraSystems (systems and their emitters), niagara | call SpawnSystem"),
				{ TEXT("niagara") }, true },
			{ TEXT("com.ivanmurzak.unreal-ai-enhanced-input"), TEXT("EnhancedInput Tools"), TEXT("UnrealAIEnhancedInput"),
				TEXT("Enhanced Input: inputActions, inputContexts (key mappings), enhancedInput | call CreateAction / CreateContext / "
					"AddMapping / AddActionEvent / AddMappingContextOnBeginPlay"),
				{ TEXT("enhancedinput"), TEXT("inputaction"), TEXT("inputcontext"), TEXT("inputmapping"), TEXT("mappingcontext"),
					TEXT("addmappingcontext"), TEXT("addactionevent"), TEXT("ia_"), TEXT("imc_") }, true },
			{ TEXT("com.ivanmurzak.unreal-ai-pcg"), TEXT("PCG Tools"), TEXT("UnrealAIPCG"),
				TEXT("Procedural Content Generation: pcgGraphs, PCG components, pcg | call AddComponent / Generate"),
				{ TEXT("pcg") }, true },
			{ TEXT("com.ivanmurzak.unreal-ai-gas"), TEXT("GAS Tools"), TEXT("UnrealAIGAS"),
				TEXT("Gameplay Ability System: gasAbilities, gasEffects, gasAttributeSets, granted abilities, gas | call GrantAbility"),
				{ TEXT("gas$"), TEXT("gasabilit"), TEXT("gaseffect"), TEXT("gasattribute"), TEXT("gameplayabilit"), TEXT("gameplayeffect"),
					TEXT("attributeset"), TEXT("abilitysystem"), TEXT("grantability") }, true },
			{ TEXT("com.ivanmurzak.unreal-ai-landscape"), TEXT("Landscape Tools"), TEXT("UnrealAILandscape"),
				TEXT("Landscape and Water: landscapes, waterBodies, waterZones, landscape | call Create / Sculpt / Paint / CreateLayerMaterial"),
				{ TEXT("landscape"), TEXT("waterbod"), TEXT("waterzone") }, true },
			{ TEXT("com.ivanmurzak.unreal-ai-control-rig"), TEXT("ControlRig Tools"), TEXT("UnrealAIControlRig"),
				TEXT("Control Rig: controlRigs (hierarchy, controls, bones), controlRig | call CreateRig / AddBone / AddControl / AddTwoBoneIK"),
				{ TEXT("controlrig") }, true },
		};
		return Extensions;
	}

	/** SetCatalog: the online catalog's entries that are not bundled. */
	TArray<FUnrealMcpKnownExtension> GCatalogOnly;

	EUnrealMcpExtensionState ExtensionStateOf(const FUnrealMcpKnownExtension& Extension)
	{
		if (const IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(Extension.ExtensionId))
			return UnrealMcpRqlTools::IsModuleEnabled(*Module) ? EUnrealMcpExtensionState::Ready : EUnrealMcpExtensionState::TurnedOff;
		const TSharedPtr<IPlugin> Plugin = Extension.PluginName.IsEmpty() ? nullptr : IPluginManager::Get().FindPlugin(Extension.PluginName);
		if (!Plugin.IsValid())
			return EUnrealMcpExtensionState::NotInstalled;
		if (!Plugin->IsEnabled())
			return EUnrealMcpExtensionState::PluginDisabled;
		return Extension.bRqlModule ? EUnrealMcpExtensionState::NotLoaded : EUnrealMcpExtensionState::Ready;
	}

	/** How to install the extension @p Id (the agent asks; or the CLI), then restart. */
	FString InstallInstruction(const FString& Id)
	{
		return FString::Printf(TEXT("ask to install extension %s (e.g. through the AI Game Dev app) or run "
			"unreal-mcp-cli install-extension %s in the project folder, then restart the Unreal Editor (until then it shows as not "
			"installed)"), *Id, *Id);
	}

	/**
	 * The identifiers of @p Text, lower-cased: runs of letters, digits and underscores, plus their parts between underscores
	 * (BP_PCGVolume gives bp_pcgvolume, bp, pcgvolume). A quoted string with whitespace in it is prose ("Gas Station"),
	 * not a name, so its words are left out; a name or path in quotes ("PCGGraph", "/Game/Input/IA_Jump") counts.
	 */
	TArray<FString> Identifiers(const FString& Text)
	{
		FString Names;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (Text[Index] != TEXT('"'))
			{
				Names.AppendChar(Text[Index]);
				continue;
			}
			int32 End = Index + 1;
			bool bProse = false;
			for (; End < Text.Len() && Text[End] != TEXT('"'); ++End)
			{
				bProse |= FChar::IsWhitespace(Text[End]);
				End += Text[End] == TEXT('\\') ? 1 : 0; // an escaped character
			}
			Names += bProse ? FString(TEXT(" ")) : TEXT(" ") + Text.Mid(Index + 1, End - Index - 1) + TEXT(" ");
			Index = End;
		}

		TArray<FString> Out;
		FString Current;
		auto Flush = [&Out, &Current]()
		{
			if (Current.IsEmpty())
				return;
			TArray<FString> Parts;
			if (Current.ParseIntoArray(Parts, TEXT("_")) > 1)
				Out.Append(Parts);
			Out.Add(MoveTemp(Current));
			Current.Reset();
		};
		for (const TCHAR Char : Names)
		{
			if (FChar::IsAlnum(Char) || Char == TEXT('_'))
				Current.AppendChar(FChar::ToLower(Char));
			else
				Flush();
		}
		Flush();
		return Out;
	}
}

namespace UnrealMcpAvailableExtensions
{
	TArray<FUnrealMcpKnownExtension> Known()
	{
		TArray<FUnrealMcpKnownExtension> Out = Bundled();
		Out.Append(GCatalogOnly);
		return Out;
	}

	void SetCatalog(TArray<FUnrealMcpKnownExtension> Entries)
	{
		Entries.RemoveAll([](const FUnrealMcpKnownExtension& Entry)
		{
			return Entry.ExtensionId.IsEmpty() || Bundled().ContainsByPredicate([&Entry](const FUnrealMcpKnownExtension& Own)
			{
				return Own.ExtensionId.Equals(Entry.ExtensionId, ESearchCase::IgnoreCase);
			});
		});
		GCatalogOnly = MoveTemp(Entries);
	}

	TArray<FUnrealMcpMissingExtension> Missing()
	{
		TArray<FUnrealMcpMissingExtension> Out;
		auto AddMissing = [&Out](const TArray<FUnrealMcpKnownExtension>& Extensions)
		{
			for (const FUnrealMcpKnownExtension& Extension : Extensions)
			{
				const EUnrealMcpExtensionState State = ExtensionStateOf(Extension);
				if (State != EUnrealMcpExtensionState::Ready)
					Out.Add({ Extension, State });
			}
		};
		AddMissing(Bundled());
		AddMissing(GCatalogOnly);
		return Out;
	}

	FString StateLabel(EUnrealMcpExtensionState ExtensionState)
	{
		switch (ExtensionState)
		{
		case EUnrealMcpExtensionState::Ready: return TEXT("ready");
		case EUnrealMcpExtensionState::TurnedOff: return TEXT("turned off");
		case EUnrealMcpExtensionState::NotLoaded: return TEXT("installed, not loaded yet");
		case EUnrealMcpExtensionState::PluginDisabled: return TEXT("plugin disabled");
		default: return TEXT("not installed");
		}
	}

	FString HowToGet(const FUnrealMcpMissingExtension& Missing)
	{
		const FString& Id = Missing.Extension.ExtensionId;
		switch (Missing.State)
		{
		case EUnrealMcpExtensionState::Ready:
			return FString();
		case EUnrealMcpExtensionState::TurnedOff:
			return TEXT("turn it on in the Unreal-MCP Extensions panel (no restart)");
		case EUnrealMcpExtensionState::NotLoaded:
			return TEXT("restart the Unreal Editor so it builds and loads");
		case EUnrealMcpExtensionState::PluginDisabled:
			return FString::Printf(TEXT("enable the plugin %s (Edit > Plugins), then restart the Unreal Editor"), *Missing.Extension.PluginName);
		default:
			return InstallInstruction(Id);
		}
	}

	FString Line(const FUnrealMcpMissingExtension& Missing)
	{
		FString Out = FString::Printf(TEXT("%s (%s): %s - %s"), *Missing.Extension.ExtensionId, *Missing.Extension.Name,
			*Missing.Extension.Adds, *StateLabel(Missing.State));
		if (Missing.State != EUnrealMcpExtensionState::NotInstalled)
			Out += TEXT(": ") + HowToGet(Missing);
		return Out;
	}

	FString InstallNote()
	{
		return TEXT("Unreal-MCP extensions not available in this editor (each adds RQL sources and operations). Install one: ")
			+ InstallInstruction(TEXT("<id>")) + TEXT(".");
	}

	const FUnrealMcpMissingExtension* MatchDomain(const FString& Text, const TArray<FUnrealMcpMissingExtension>& Candidates)
	{
		const TArray<FString> Words = Identifiers(Text);
		for (const FUnrealMcpMissingExtension& Candidate : Candidates)
		{
			for (const FString& Keyword : Candidate.Extension.Keywords)
			{
				// A keyword prefixes a word ("niagara" matches niagarasystems); one ending in $ is the whole word ("gas$").
				const bool bWhole = Keyword.EndsWith(TEXT("$"));
				const FString Key = bWhole ? Keyword.LeftChop(1) : Keyword;
				if (Words.ContainsByPredicate([&Key, bWhole](const FString& Word) { return bWhole ? Word == Key : Word.StartsWith(Key, ESearchCase::CaseSensitive); }))
					return &Candidate;
			}
		}
		return nullptr;
	}

	FString Hint(const FUnrealMcpMissingExtension& Missing)
	{
		return FString::Printf(TEXT("the Unreal-MCP extension %s (%s) adds %s; it is %s: %s"), *Missing.Extension.ExtensionId,
			*Missing.Extension.Name, *Missing.Extension.Adds, *StateLabel(Missing.State), *HowToGet(Missing));
	}
}
