// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
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
//  Run via:  Automation RunTests UnrealAIPCG
// ============================================================================================

namespace
{
	// Spec-unique helper names (the module is unity-built — keep file-local helpers uniquely named).
	IUnrealMcpRqlModule* UnrealAIPCG_FindOwnModule()
	{
		for (IUnrealMcpRqlModule* Module : IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpRqlModule>(
			IUnrealMcpRqlModule::GetModularFeatureName()))
		{
			if (Module && Module->GetExtensionId() == TEXT("com.ivanmurzak.unreal-ai-pcg"))
				return Module;
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> UnrealAIPCG_Run(const FString& Plan, bool bMutate)
	{
		bool bOk = false;
		const FString Json = UnrealMcpRqlTools::ExecutePlan(Plan, bMutate, false, bOk);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result);
		return Result;
	}

	bool UnrealAIPCG_Ok(const TSharedPtr<FJsonObject>& Result)
	{
		return Result.IsValid() && Result->GetBoolField(TEXT("ok"));
	}
}

BEGIN_DEFINE_SPEC(FUnrealAIPCGSpec, "UnrealAIPCG",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIPCGSpec)

void FUnrealAIPCGSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealAIPCG_FindOwnModule();
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("PCG")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `pcgGraphs` |"), Skill.Contains(TEXT("| `pcgGraphs` |")));
			TestTrue(TEXT("| `pcg` |"), Skill.Contains(TEXT("| `pcg` |")));
			TestTrue(TEXT("## PCG"), Skill.Contains(TEXT("## PCG")));
		});
	});

	Describe("sources and members", [this]()
	{
		It("pcgGraphs answers a query", [this]()
		{
			// pcgGraphs | count
			const TSharedPtr<FJsonObject> Result = UnrealAIPCG_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcgGraphs"]},"stages":[{"op":"count"}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAIPCG_Ok(Result));
		});

		It("describes PCGGraph with its Nodes member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("PCGGraph"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Nodes"), Schema.Contains(TEXT("\"name\":\"Nodes\"")));
		});

		It("describes PCGComponent with its Generation member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("PCGComponent"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Generation"), Schema.Contains(TEXT("\"name\":\"Generation\"")));
		});
	});

	Describe("operations", [this]()
	{
		It("lists AddComponent and Generate", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("PCGOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists AddComponent("), Schema.Contains(TEXT("AddComponent(")));
			TestTrue(TEXT("lists Generate("), Schema.Contains(TEXT("Generate(")));
		});

		It("AddComponent fails defensively on an unknown actor", [this]()
		{
			// pcg | call AddComponent("RqlSpecNoActor")
			const TSharedPtr<FJsonObject> Result = UnrealAIPCG_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcg"]},"stages":[{"op":"call","method":"AddComponent","args":[{"lit":"RqlSpecNoActor"}]}]})RQL"), true);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), UnrealAIPCG_Ok(Result));
		});

		It("Generate fails defensively on an unknown actor", [this]()
		{
			// pcg | call Generate("RqlSpecNoActor")
			const TSharedPtr<FJsonObject> Result = UnrealAIPCG_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcg"]},"stages":[{"op":"call","method":"Generate","args":[{"lit":"RqlSpecNoActor"}]}]})RQL"), true);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), UnrealAIPCG_Ok(Result));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
