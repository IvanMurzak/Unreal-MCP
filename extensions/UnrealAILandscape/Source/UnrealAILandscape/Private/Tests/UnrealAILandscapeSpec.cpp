// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
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
//  Run via:  Automation RunTests UnrealAILandscape
// ============================================================================================

namespace
{
	// Spec-unique helper names (the module is unity-built — keep file-local helpers uniquely named).
	IUnrealMcpRqlModule* UnrealAILandscape_FindOwnModule()
	{
		for (IUnrealMcpRqlModule* Module : IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpRqlModule>(
			IUnrealMcpRqlModule::GetModularFeatureName()))
		{
			if (Module && Module->GetExtensionId() == TEXT("com.ivanmurzak.unreal-ai-landscape"))
				return Module;
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> UnrealAILandscape_Run(const FString& Plan, bool bMutate)
	{
		bool bOk = false;
		const FString Json = UnrealMcpRqlTools::ExecutePlan(Plan, bMutate, false, bOk);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result);
		return Result;
	}

	bool UnrealAILandscape_Ok(const TSharedPtr<FJsonObject>& Result)
	{
		return Result.IsValid() && Result->GetBoolField(TEXT("ok"));
	}
}

BEGIN_DEFINE_SPEC(FUnrealAILandscapeSpec, "UnrealAILandscape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAILandscapeSpec)

void FUnrealAILandscapeSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealAILandscape_FindOwnModule();
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("Landscape")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `landscapes` |"), Skill.Contains(TEXT("| `landscapes` |")));
			TestTrue(TEXT("| `waterBodies` |"), Skill.Contains(TEXT("| `waterBodies` |")));
			TestTrue(TEXT("| `waterZones` |"), Skill.Contains(TEXT("| `waterZones` |")));
			TestTrue(TEXT("## Landscape and Water"), Skill.Contains(TEXT("## Landscape and Water")));
		});
	});

	Describe("sources", [this]()
	{
		It("landscapes answers a query", [this]()
		{
			// landscapes | count
			const TSharedPtr<FJsonObject> Result = UnrealAILandscape_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscapes"]},"stages":[{"op":"count"}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAILandscape_Ok(Result));
		});

		It("waterBodies answers a query", [this]()
		{
			// waterBodies | count
			const TSharedPtr<FJsonObject> Result = UnrealAILandscape_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["waterBodies"]},"stages":[{"op":"count"}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAILandscape_Ok(Result));
		});

		It("waterZones answers a query", [this]()
		{
			// waterZones | count
			const TSharedPtr<FJsonObject> Result = UnrealAILandscape_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["waterZones"]},"stages":[{"op":"count"}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAILandscape_Ok(Result));
		});
	});

	Describe("members", [this]()
	{
		It("describes LandscapeProxy with its Sections and PaintLayers members", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("LandscapeProxy"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Sections"), Schema.Contains(TEXT("\"name\":\"Sections\"")));
			TestTrue(TEXT("lists name:PaintLayers"), Schema.Contains(TEXT("\"name\":\"PaintLayers\"")));
		});

		It("describes WaterBody with its Water member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("WaterBody"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Water"), Schema.Contains(TEXT("\"name\":\"Water\"")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
