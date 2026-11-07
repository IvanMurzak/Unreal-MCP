// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Extensions/UnrealMcpExtensionCatalog.h"
#include "UnrealMcpAvailableExtensions.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpToolRegistry.h"

/**
 * What an agent learns about the Unreal-MCP extensions this editor does not have: the bundled first-party list (offline
 * too), merged with the online catalog; unreal-tool-list, rql-schema and the unreal-rql skill list them with how to
 * install each; a failed or empty RQL query on the domain of one names it. The live checks hold in both runs: the core
 * host (no extension installed) and the extension suite (all six installed), branching on the extension's state.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpAvailableExtensionsSpec, "UnrealMcp.AvailableExtensions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static constexpr const TCHAR* PcgId = TEXT("com.ivanmurzak.unreal-ai-pcg");
	static constexpr const TCHAR* NiagaraId = TEXT("com.ivanmurzak.unreal-ai-niagara");

	/** Every known extension, as not installed. */
	static TArray<FUnrealMcpMissingExtension> AllNotInstalled()
	{
		TArray<FUnrealMcpMissingExtension> Out;
		for (const FUnrealMcpKnownExtension& Extension : UnrealMcpAvailableExtensions::Known())
			Out.Add({ Extension, EUnrealMcpExtensionState::NotInstalled });
		return Out;
	}

	static FString MatchedId(const FString& Text, const TArray<FUnrealMcpMissingExtension>& Candidates)
	{
		const FUnrealMcpMissingExtension* Match = UnrealMcpAvailableExtensions::MatchDomain(Text, Candidates);
		return Match ? Match->Extension.ExtensionId : FString();
	}

	/** The live state of a first-party extension in this editor. */
	static bool IsMissing(const FString& Id)
	{
		return UnrealMcpAvailableExtensions::Missing().ContainsByPredicate(
			[&Id](const FUnrealMcpMissingExtension& Missing) { return Missing.Extension.ExtensionId == Id; });
	}

	static FString InstallCommand(const FString& Id)
	{
		return TEXT("unreal-mcp-cli install-extension ") + Id;
	}

END_DEFINE_SPEC(FUnrealMcpAvailableExtensionsSpec)

void FUnrealMcpAvailableExtensionsSpec::Define()
{
	It("bundles the six first-party extensions with what they add and how to install them", [this]()
	{
		const TArray<FUnrealMcpKnownExtension> Known = UnrealMcpAvailableExtensions::Known();
		for (const TCHAR* Id : { TEXT("com.ivanmurzak.unreal-ai-niagara"), TEXT("com.ivanmurzak.unreal-ai-enhanced-input"),
			TEXT("com.ivanmurzak.unreal-ai-pcg"), TEXT("com.ivanmurzak.unreal-ai-gas"), TEXT("com.ivanmurzak.unreal-ai-landscape"),
			TEXT("com.ivanmurzak.unreal-ai-control-rig") })
		{
			const FUnrealMcpKnownExtension* Extension = Known.FindByPredicate([Id](const FUnrealMcpKnownExtension& E) { return E.ExtensionId == Id; });
			if (!TestNotNull(*FString::Printf(TEXT("%s is bundled"), Id), Extension))
				continue;
			TestTrue(TEXT("names its plugin"), Extension->PluginName.StartsWith(TEXT("UnrealAI")));
			TestFalse(TEXT("says what it adds"), Extension->Adds.IsEmpty());
			TestTrue(TEXT("ships an RQL module"), Extension->bRqlModule && Extension->Keywords.Num() > 0);
			const FString Line = UnrealMcpAvailableExtensions::Line({ *Extension, EUnrealMcpExtensionState::NotInstalled });
			TestTrue(*FString::Printf(TEXT("line names it and its state (%s)"), *Line), Line.StartsWith(FString(Id) + TEXT(" (")) && Line.EndsWith(TEXT(" - not installed")));
			const FString How = UnrealMcpAvailableExtensions::HowToGet({ *Extension, EUnrealMcpExtensionState::NotInstalled });
			TestTrue(*FString::Printf(TEXT("install: the agent request and the CLI (%s)"), *How),
				How.Contains(FString(TEXT("ask to install extension ")) + Id) && How.Contains(InstallCommand(Id)) && How.Contains(TEXT("restart")));
		}
		const FString Note = UnrealMcpAvailableExtensions::InstallNote();
		TestTrue(TEXT("the note says how to install"), Note.Contains(TEXT("ask to install extension <id>")) && Note.Contains(TEXT("unreal-mcp-cli install-extension <id>")));
	});

	It("matches a query to the extension whose domain it touches", [this]()
	{
		const TArray<FUnrealMcpMissingExtension> All = AllNotInstalled();
		TestEqual(TEXT("a source"), MatchedId(TEXT(R"({"rql":"0.1","source":{"path":["niagaraSystems"]},"stages":[{"op":"count"}]})"), All), FString(NiagaraId));
		TestEqual(TEXT("a type name"), MatchedId(TEXT("PCGGraph"), All), FString(PcgId));
		TestEqual(TEXT("GAS"), MatchedId(TEXT("gasAbilities | first 3"), All), FString(TEXT("com.ivanmurzak.unreal-ai-gas")));
		TestEqual(TEXT("an asset class"), MatchedId(TEXT(R"(unreal.assets("InputMappingContext"))"), All), FString(TEXT("com.ivanmurzak.unreal-ai-enhanced-input")));
		TestEqual(TEXT("an IA_ asset"), MatchedId(TEXT(R"(unreal.asset("/Game/Input/IA_Jump"))"), All), FString(TEXT("com.ivanmurzak.unreal-ai-enhanced-input")));
		TestEqual(TEXT("water"), MatchedId(TEXT("waterBodies | count"), All), FString(TEXT("com.ivanmurzak.unreal-ai-landscape")));
		TestEqual(TEXT("Control Rig"), MatchedId(TEXT(R"(ref("ControlRigBlueprint#12"))"), All), FString(TEXT("com.ivanmurzak.unreal-ai-control-rig")));
		TestEqual(TEXT("core sources match nothing"), MatchedId(TEXT("actors | where Label == \"Megastructure\" | count"), All), FString());
		TestEqual(TEXT("gameplay tags are core"), MatchedId(TEXT("gameplayTags | count"), All), FString());
		TestEqual(TEXT("only a word start counts"), MatchedId(TEXT("actors | where Label == \"SpcgX\""), All), FString());
		TestEqual(TEXT("prose in quotes is not a name"), MatchedId(TEXT(R"({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"Gas Station"}]}}]})"), All), FString());
		TestEqual(TEXT("gas is a whole word"), MatchedId(TEXT("unreal.find(\"Gasoline\")"), All), FString());
		TestEqual(TEXT("a part between underscores"), MatchedId(TEXT(R"(unreal.asset("/Game/BP_PCGVolume"))"), All), FString(PcgId));
		TestEqual(TEXT("the GAS operations source"), MatchedId(TEXT("gas | call GrantAbility"), All), FString(TEXT("com.ivanmurzak.unreal-ai-gas")));
		TestEqual(TEXT("ready extensions are not candidates"), MatchedId(TEXT("niagaraSystems"), {}), FString());
	});

	It("says what to do in each state", [this]()
	{
		const FUnrealMcpKnownExtension Pcg = *UnrealMcpAvailableExtensions::Known().FindByPredicate(
			[](const FUnrealMcpKnownExtension& E) { return E.ExtensionId == PcgId; });
		TestTrue(TEXT("turned off: the Extensions panel"), UnrealMcpAvailableExtensions::HowToGet({ Pcg, EUnrealMcpExtensionState::TurnedOff }).Contains(TEXT("Extensions panel")));
		TestTrue(TEXT("plugin disabled: enable it, restart"), UnrealMcpAvailableExtensions::HowToGet({ Pcg, EUnrealMcpExtensionState::PluginDisabled }).Contains(TEXT("enable the plugin UnrealAIPCG")));
		TestTrue(TEXT("not loaded: restart"), UnrealMcpAvailableExtensions::HowToGet({ Pcg, EUnrealMcpExtensionState::NotLoaded }).Contains(TEXT("restart")));
		TestTrue(TEXT("ready: nothing"), UnrealMcpAvailableExtensions::HowToGet({ Pcg, EUnrealMcpExtensionState::Ready }).IsEmpty());
		const FString Line = UnrealMcpAvailableExtensions::Line({ Pcg, EUnrealMcpExtensionState::PluginDisabled });
		TestTrue(*FString::Printf(TEXT("a line of another state says what to do (%s)"), *Line), Line.Contains(TEXT("plugin disabled: enable the plugin")));
		const FString Hint = UnrealMcpAvailableExtensions::Hint({ Pcg, EUnrealMcpExtensionState::NotInstalled });
		TestTrue(*FString::Printf(TEXT("the hint names it and how to install (%s)"), *Hint), Hint.Contains(PcgId) && Hint.Contains(InstallCommand(PcgId)));
	});

	It("adds the online catalog's other extensions and keeps the bundled ones", [this]()
	{
		const int32 Bundled = 6;
		TArray<FUnrealMcpKnownExtension> Fetched = UnrealMcpAvailableExtensions::Known(); // restored at the end: an interactive editor may have fetched it
		Fetched.RemoveAt(0, Bundled);
		TArray<FUnrealMcpCatalogEntry> Catalog;
		FUnrealMcpCatalogEntry& Niagara = Catalog.AddDefaulted_GetRef();
		Niagara.ExtensionId = NiagaraId;
		Niagara.Name = TEXT("Renamed Niagara");
		Niagara.PluginName = TEXT("UnrealAINiagara");
		FUnrealMcpCatalogEntry& Other = Catalog.AddDefaulted_GetRef();
		Other.ExtensionId = TEXT("com.example.unreal-mcp-spec-weather");
		Other.Name = TEXT("Weather Tools");
		Other.PluginName = TEXT("UnrealMcpSpecWeatherPluginThatIsNotInstalled");
		Other.Description = TEXT("weather systems");
		FUnrealMcpExtensionCatalog::PublishToAgents(Catalog);

		const TArray<FUnrealMcpKnownExtension> Known = UnrealMcpAvailableExtensions::Known();
		TestEqual(TEXT("the bundled six plus the other one"), Known.Num(), Bundled + 1);
		const FUnrealMcpKnownExtension* KnownNiagara = Known.FindByPredicate([](const FUnrealMcpKnownExtension& E) { return E.ExtensionId == NiagaraId; });
		TestTrue(TEXT("the bundled entry stays"), KnownNiagara && KnownNiagara->Name == TEXT("Niagara Tools"));
		const TArray<FUnrealMcpMissingExtension> Missing = UnrealMcpAvailableExtensions::Missing();
		const FUnrealMcpMissingExtension* Weather = Missing.FindByPredicate(
			[](const FUnrealMcpMissingExtension& M) { return M.Extension.ExtensionId == TEXT("com.example.unreal-mcp-spec-weather"); });
		TestTrue(TEXT("the other one is listed as not installed"), Weather && Weather->State == EUnrealMcpExtensionState::NotInstalled
			&& UnrealMcpAvailableExtensions::Line(*Weather).Contains(TEXT("(Weather Tools): weather systems - not installed")));

		FUnrealMcpExtensionCatalog::PublishToAgents({});
		TestEqual(TEXT("an empty catalog leaves the bundled six"), UnrealMcpAvailableExtensions::Known().Num(), Bundled);
		UnrealMcpAvailableExtensions::SetCatalog(MoveTemp(Fetched));
	});

	It("names a missing extension when a query on its source fails, and warns when one on its domain finds nothing", [this]()
	{
		using namespace UnrealMcpRqlSpecHelpers;
		// pcgGraphs | count
		const TSharedPtr<FJsonObject> Source = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["pcgGraphs"]},"stages":[{"op":"count"}]})"));
		// actors | where Label == "PcgSpecNoSuchActor" | count
		const TSharedPtr<FJsonObject> Empty = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"PcgSpecNoSuchActor"}]}},{"op":"count"}]})"));
		// actors | where Label == "SpecNoSuchActor" | count
		const TSharedPtr<FJsonObject> Unrelated = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"SpecNoSuchActor"}]}},{"op":"count"}]})"));
		TestTrue(TEXT("the empty queries succeed"), IsOk(Empty) && IsOk(Unrelated));
		TestFalse(*FString::Printf(TEXT("no hint off the domain (%s)"), *Hints(Unrelated)), Hints(Unrelated).Contains(TEXT("install")));
		// actors | where Label == "PcgSpecNoSuchActor" | set HiddenInEditor = true: an empty mutation is no missing domain
		const TSharedPtr<FJsonObject> Mutation = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"PcgSpecNoSuchActor"}]}},{"op":"set","assign":[{"target":{"path":["HiddenInEditor"]},"value":{"lit":true}}]}]})"), /*bMutate*/ true);
		TestFalse(*FString::Printf(TEXT("no warning on a mutation (%s)"), *Codes(Mutation)), Codes(Mutation).Contains(TEXT("extension-missing")));
		if (IsMissing(PcgId))
		{
			TestFalse(TEXT("the source is unknown"), IsOk(Source));
			TestTrue(*FString::Printf(TEXT("the error names the extension (%s)"), *Hints(Source)), Hints(Source).Contains(PcgId) && Hints(Source).Contains(InstallCommand(PcgId)));
			TestTrue(*FString::Printf(TEXT("the empty result warns (%s)"), *Codes(Empty)), Codes(Empty).Contains(TEXT("extension-missing")) && Hints(Empty).Contains(PcgId));
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("the installed source answers (%s)"), *Codes(Source)), IsOk(Source));
			TestFalse(TEXT("no warning for an installed extension"), Codes(Empty).Contains(TEXT("extension-missing")));
		}
	});

	It("lists the missing extensions in rql-schema, the unreal-rql skill and unreal-tool-list", [this]()
	{
		const TArray<FUnrealMcpMissingExtension> Missing = UnrealMcpAvailableExtensions::Missing();
		bool bOk = false;
		const TSharedPtr<FJsonObject> Schema = UnrealMcpRqlSpecHelpers::Parse(UnrealMcpRqlTools::Schema(FString(), bOk));
		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
		const bool bListed = Schema.IsValid() && Schema->TryGetArrayField(TEXT("extensions"), Listed);
		const FString Skill = UnrealMcpRqlTools::SkillMarkdown();

		FUnrealMcpToolRegistry Registry;
		UnrealMcpToolListTools::Register(Registry);
		const FUnrealMcpToolResult All = Registry.Execute(TEXT("unreal-tool-list"), FUnrealMcpToolCall(MakeShared<FJsonObject>()));
		TestTrue(TEXT("tool-list succeeds"), All.bSuccess);

		if (Missing.IsEmpty())
		{
			TestFalse(TEXT("rql-schema: no extension section"), bListed);
			TestFalse(TEXT("skill: no extension section"), Skill.Contains(TEXT("## Extensions you can add")));
			TestFalse(TEXT("tool-list: no extension section"), All.Message.Contains(UnrealMcpAvailableExtensions::InstallNote()));
			return;
		}
		TestTrue(TEXT("rql-schema lists them"), bListed && Listed->Num() == Missing.Num());
		TestTrue(TEXT("rql-schema says how to install"), Schema.IsValid() && Schema->GetStringField(TEXT("extensionsNote")) == UnrealMcpAvailableExtensions::InstallNote());
		TestTrue(TEXT("the skill has the section"), Skill.Contains(TEXT("## Extensions you can add")));
		TestTrue(TEXT("tool-list has the note"), All.Message.Contains(UnrealMcpAvailableExtensions::InstallNote()));
		for (const FUnrealMcpMissingExtension& Extension : Missing)
		{
			const FString Line = UnrealMcpAvailableExtensions::Line(Extension);
			TestTrue(*FString::Printf(TEXT("tool-list lists %s"), *Extension.Extension.ExtensionId), All.Message.Contains(Line));
			TestTrue(*FString::Printf(TEXT("the skill lists %s"), *Extension.Extension.ExtensionId), Skill.Contains(TEXT("- ") + Line));
		}
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		TestTrue(TEXT("tool-list structured rows"), All.Structured.IsValid() && All.Structured->TryGetArrayField(TEXT("availableExtensions"), Rows) && Rows->Num() == Missing.Num());

		// The filter narrows the extensions as it narrows the tools: an agent searching "pcg" finds the PCG extension.
		if (IsMissing(PcgId))
		{
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("filter"), TEXT("pcg"));
			const FString Filtered = Registry.Execute(TEXT("unreal-tool-list"), FUnrealMcpToolCall(Args)).Message;
			TestTrue(*FString::Printf(TEXT("the PCG extension (%s)"), *Filtered), Filtered.Contains(FString(PcgId) + TEXT(" (")));
			TestFalse(TEXT("not the Niagara one"), Filtered.Contains(FString(NiagaraId) + TEXT(" (")));

			bool bSchemaOk = true;
			const TSharedPtr<FJsonObject> Unknown = UnrealMcpRqlSpecHelpers::Parse(UnrealMcpRqlTools::Schema(TEXT("PCGSpecNoSuchType"), bSchemaOk));
			FString Hint;
			TestTrue(TEXT("an unknown type of the domain names the extension"), !bSchemaOk && Unknown.IsValid()
				&& Unknown->TryGetStringField(TEXT("hint"), Hint) && Hint.Contains(InstallCommand(PcgId)));
		}
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
