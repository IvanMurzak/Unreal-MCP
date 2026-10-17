// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions").
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::ExecutePlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it.
//
//  Run via:  Automation RunTests UnrealAIGAS
// ============================================================================================

namespace
{
	// Spec-unique helper names (the module is unity-built — keep file-local helpers uniquely named).
	IUnrealMcpRqlModule* UnrealAIGAS_FindOwnModule()
	{
		for (IUnrealMcpRqlModule* Module : IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpRqlModule>(
			IUnrealMcpRqlModule::GetModularFeatureName()))
		{
			if (Module && Module->GetExtensionId() == TEXT("com.ivanmurzak.unreal-ai-gas"))
				return Module;
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> UnrealAIGAS_Run(const FString& Plan, bool bMutate)
	{
		bool bOk = false;
		const FString Json = UnrealMcpRqlTools::ExecutePlan(Plan, bMutate, false, bOk);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result);
		return Result;
	}

	bool UnrealAIGAS_Ok(const TSharedPtr<FJsonObject>& Result)
	{
		return Result.IsValid() && Result->GetBoolField(TEXT("ok"));
	}
}

BEGIN_DEFINE_SPEC(FUnrealAIGASSpec, "UnrealAIGAS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIGASSpec)

void FUnrealAIGASSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealAIGAS_FindOwnModule();
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
			const TSharedPtr<FJsonObject> Result = UnrealAIGAS_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasAbilities"]},"stages":[{"op":"count"}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAIGAS_Ok(Result));
		});

		It("gasEffects answers a query", [this]()
		{
			// gasEffects | count
			const TSharedPtr<FJsonObject> Result = UnrealAIGAS_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasEffects"]},"stages":[{"op":"count"}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAIGAS_Ok(Result));
		});

		It("gasAttributeSets answers a query", [this]()
		{
			// gasAttributeSets | count
			const TSharedPtr<FJsonObject> Result = UnrealAIGAS_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasAttributeSets"]},"stages":[{"op":"count"}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAIGAS_Ok(Result));
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
			const TSharedPtr<FJsonObject> Result = UnrealAIGAS_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"GrantAbility","args":[{"lit":"RqlSpecNoActor"},{"lit":"GameplayAbility"}]}]})RQL"), true);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), UnrealAIGAS_Ok(Result));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
