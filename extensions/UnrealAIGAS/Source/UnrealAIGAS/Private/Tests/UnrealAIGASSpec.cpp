// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
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
//  Run via:  Automation RunTests UnrealAIGAS
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIGASSpec, "UnrealAIGAS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIGASSpec)

void FUnrealAIGASSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-gas"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("GAS")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `gasAbilities` |"), Skill.Contains(TEXT("| `gasAbilities` |")));
			TestTrue(TEXT("| `gasEffects` |"), Skill.Contains(TEXT("| `gasEffects` |")));
			TestTrue(TEXT("| `gasAttributeSets` |"), Skill.Contains(TEXT("| `gasAttributeSets` |")));
			TestTrue(TEXT("| `gas` |"), Skill.Contains(TEXT("| `gas` |")));
			TestTrue(TEXT("## Gameplay Ability System"), Skill.Contains(TEXT("## Gameplay Ability System")));
		});
	});

	Describe("sources", [this]()
	{
		It("gasAbilities answers a query", [this]()
		{
			// gasAbilities | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasAbilities"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("gasEffects answers a query", [this]()
		{
			// gasEffects | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasEffects"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("gasAttributeSets answers a query", [this]()
		{
			// gasAttributeSets | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasAttributeSets"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("describes AbilitySystemComponent with its GrantedAbilities member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("AbilitySystemComponent"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:GrantedAbilities"), Schema.Contains(TEXT("\"name\":\"GrantedAbilities\"")));
		});
	});

	Describe("operation: GrantAbility", [this]()
	{
		It("is listed as an operation of the gas source", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("GASOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists GrantAbility("), Schema.Contains(TEXT("GrantAbility(")));
		});

		It("fails defensively on an unknown actor", [this]()
		{
			// gas | call GrantAbility("RqlSpecNoActor", "GameplayAbility")
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"GrantAbility","args":[{"lit":"RqlSpecNoActor"},{"lit":"GameplayAbility"}]}]})RQL"), true, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), bResultOk);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
