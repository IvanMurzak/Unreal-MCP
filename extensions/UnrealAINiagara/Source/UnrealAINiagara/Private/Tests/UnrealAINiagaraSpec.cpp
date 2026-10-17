// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions").
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::RunPlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it.
//
//  Run via:  Automation RunTests UnrealAINiagara
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAINiagaraSpec, "UnrealAINiagara",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAINiagaraSpec)

void FUnrealAINiagaraSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-niagara"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("Niagara")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("niagaraSystems source"), Skill.Contains(TEXT("| `niagaraSystems` |")));
			TestTrue(TEXT("niagara operations source"), Skill.Contains(TEXT("| `niagara` |")));
			TestTrue(TEXT("guide section"), Skill.Contains(TEXT("## Niagara")));
		});
	});

	Describe("source: niagaraSystems", [this]()
	{
		It("answers a query (count of the project's Niagara systems)", [this]()
		{
			// niagaraSystems | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagaraSystems"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("query ok"), bResultOk);
			TestTrue(TEXT("a count"), Result.IsValid() && Result->HasField(TEXT("value")));
		});

		It("describes NiagaraSystem with its Emitters member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("NiagaraSystem"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("Emitters member"), Schema.Contains(TEXT("\"name\":\"Emitters\"")));
		});
	});

	Describe("operation: SpawnSystem", [this]()
	{
		It("fails defensively on a missing system and places nothing", [this]()
		{
			// niagara | call SpawnSystem("/Game/RqlSpecNoSuchSystem", "(0, 0, 0)", "RqlSpecFx")
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SpawnSystem","args":[{"lit":"/Game/RqlSpecNoSuchSystem"},{"lit":"(0, 0, 0)"},{"lit":"RqlSpecFx"}]}]})RQL"), true, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("reported as a failure"), bResultOk);
		});

		It("is listed as an operation of the niagara source", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("NiagaraOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("SpawnSystem listed"), Schema.Contains(TEXT("SpawnSystem(")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
