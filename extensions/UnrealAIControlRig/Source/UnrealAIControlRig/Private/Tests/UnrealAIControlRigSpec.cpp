// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
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
//  Run via:  Automation RunTests UnrealAIControlRig
// ============================================================================================

namespace
{
	// Spec-unique helper names (the module is unity-built — keep file-local helpers uniquely named).
	IUnrealMcpRqlModule* UnrealAIControlRig_FindOwnModule()
	{
		for (IUnrealMcpRqlModule* Module : IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpRqlModule>(
			IUnrealMcpRqlModule::GetModularFeatureName()))
		{
			if (Module && Module->GetExtensionId() == TEXT("com.ivanmurzak.unreal-ai-control-rig"))
				return Module;
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> UnrealAIControlRig_Run(const FString& Plan, bool bMutate)
	{
		bool bOk = false;
		const FString Json = UnrealMcpRqlTools::ExecutePlan(Plan, bMutate, false, bOk);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result);
		return Result;
	}

	bool UnrealAIControlRig_Ok(const TSharedPtr<FJsonObject>& Result)
	{
		return Result.IsValid() && Result->GetBoolField(TEXT("ok"));
	}
}

BEGIN_DEFINE_SPEC(FUnrealAIControlRigSpec, "UnrealAIControlRig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIControlRigSpec)

void FUnrealAIControlRigSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealAIControlRig_FindOwnModule();
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("ControlRig")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `controlRigs` |"), Skill.Contains(TEXT("| `controlRigs` |")));
			TestTrue(TEXT("## Control Rig"), Skill.Contains(TEXT("## Control Rig")));
		});
	});

	Describe("source: controlRigs", [this]()
	{
		It("answers a query (count of the project's Control Rig blueprints)", [this]()
		{
			// controlRigs | count
			const TSharedPtr<FJsonObject> Result = UnrealAIControlRig_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRigs"]},"stages":[{"op":"count"}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAIControlRig_Ok(Result));
		});

		It("reads the hierarchy members of every rig", [this]()
		{
			// controlRigs | select Name Elements Controls Bones
			const TSharedPtr<FJsonObject> Result = UnrealAIControlRig_Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRigs"]},"stages":[{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Elements","expr":{"path":["Elements"]}},{"name":"Controls","expr":{"path":["Controls"]}},{"name":"Bones","expr":{"path":["Bones"]}}]}]})RQL"), false);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), UnrealAIControlRig_Ok(Result));
		});
	});

	Describe("members", [this]()
	{
		It("describes ControlRigBlueprint with its hierarchy members", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("ControlRigBlueprint"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Elements"), Schema.Contains(TEXT("\"name\":\"Elements\"")));
			TestTrue(TEXT("lists name:Hierarchy"), Schema.Contains(TEXT("\"name\":\"Hierarchy\"")));
			TestTrue(TEXT("lists name:Controls"), Schema.Contains(TEXT("\"name\":\"Controls\"")));
			TestTrue(TEXT("lists name:Bones"), Schema.Contains(TEXT("\"name\":\"Bones\"")));
		});

		It("describes ControlRigComponent with its BoundRig member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("ControlRigComponent"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:BoundRig"), Schema.Contains(TEXT("\"name\":\"BoundRig\"")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
