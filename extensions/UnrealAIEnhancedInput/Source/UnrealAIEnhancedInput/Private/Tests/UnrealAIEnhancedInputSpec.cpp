// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions").
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::RunPlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it.
//
//  Run via:  Automation RunTests UnrealAIEnhancedInput
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIEnhancedInputSpec, "UnrealAIEnhancedInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIEnhancedInputSpec)

void FUnrealAIEnhancedInputSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-enhanced-input"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("EnhancedInput")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `inputActions` |"), Skill.Contains(TEXT("| `inputActions` |")));
			TestTrue(TEXT("| `inputContexts` |"), Skill.Contains(TEXT("| `inputContexts` |")));
			TestTrue(TEXT("| `enhancedInput` |"), Skill.Contains(TEXT("| `enhancedInput` |")));
			TestTrue(TEXT("## Enhanced Input"), Skill.Contains(TEXT("## Enhanced Input")));
		});
	});

	Describe("sources", [this]()
	{
		It("inputActions answers a query", [this]()
		{
			// inputActions | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["inputActions"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("inputContexts answers a query", [this]()
		{
			// inputContexts | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["inputContexts"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("describes InputMappingContext with its KeyMappings member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("InputMappingContext"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:KeyMappings"), Schema.Contains(TEXT("\"name\":\"KeyMappings\"")));
		});
	});

	Describe("operations", [this]()
	{
		It("lists the authoring operations", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("EnhancedInputOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists CreateAction("), Schema.Contains(TEXT("CreateAction(")));
			TestTrue(TEXT("lists CreateContext("), Schema.Contains(TEXT("CreateContext(")));
			TestTrue(TEXT("lists AddMapping("), Schema.Contains(TEXT("AddMapping(")));
			TestTrue(TEXT("lists RemoveMapping("), Schema.Contains(TEXT("RemoveMapping(")));
		});

		It("creates an action and a context, maps a key and refuses an unknown key", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString ActionPath = FString::Printf(TEXT("/Game/RqlSpec/IA_%s"), *Id);
			const FString ContextPath = FString::Printf(TEXT("/Game/RqlSpec/IMC_%s"), *Id);
			// enhancedInput | call CreateAction("<action>", "Axis2D")
			TestTrue(TEXT("action created"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateAction","args":[{"lit":"%s"},{"lit":"Axis2D"}]}]})RQL"), *ActionPath), true));
			// unreal.asset("<action>") | select ValueType
			TSharedPtr<FJsonObject> Action;
			const bool bActionOk = UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},"stages":[{"op":"select","fields":[{"name":"ValueType","expr":{"path":["ValueType"]}}]}]})RQL"), *ActionPath), false, &Action);
			TestTrue(TEXT("action read"), bActionOk);
			FString ValueType;
			if (Action.IsValid() && Action->HasField(TEXT("rows")) && Action->GetArrayField(TEXT("rows")).Num() == 1)
				ValueType = Action->GetArrayField(TEXT("rows"))[0]->AsArray()[0]->AsString();
			TestEqual(TEXT("value type written"), ValueType, FString(TEXT("Axis2D")));
			// enhancedInput | call CreateContext("<context>")
			TestTrue(TEXT("context created"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateContext","args":[{"lit":"%s"}]}]})RQL"), *ContextPath), true));
			// enhancedInput | call AddMapping("<context>", "<action>", "SpaceBar")
			TestTrue(TEXT("key mapped"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddMapping","args":[{"lit":"%s"},{"lit":"%s"},{"lit":"SpaceBar"}]}]})RQL"), *ContextPath, *ActionPath), true));
			// unreal.asset("<context>") | select KeyMappings
			TSharedPtr<FJsonObject> Mappings;
			const bool bMappingsOk = UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},"stages":[{"op":"select","fields":[{"name":"KeyMappings","expr":{"path":["KeyMappings"]}}]}]})RQL"), *ContextPath), false, &Mappings);
			TestTrue(TEXT("context read"), bMappingsOk);
			FString Text;
			if (Mappings.IsValid())
				FJsonSerializer::Serialize(Mappings.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
			TestTrue(TEXT("the mapping is listed"), Text.Contains(TEXT("SpaceBar")));
			// unreal.asset("<context>") | select Mappings — the reflected array is deprecated (empty) since 5.6: it reads the live mappings
			TSharedPtr<FJsonObject> Reflected;
			TestTrue(TEXT("Mappings read"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},"stages":[{"op":"select","fields":[{"name":"Mappings","expr":{"path":["Mappings"]}}]}]})RQL"), *ContextPath), false, &Reflected));
			FString ReflectedText;
			if (Reflected.IsValid())
				FJsonSerializer::Serialize(Reflected.ToSharedRef(), TJsonWriterFactory<>::Create(&ReflectedText));
			TestTrue(*FString::Printf(TEXT("Mappings lists the mapping too (%s)"), *ReflectedText.Left(300)), ReflectedText.Contains(TEXT("SpaceBar")));
			// enhancedInput | call AddMapping("<context>", "<action>", "NoSuchKey")
			TestFalse(TEXT("unknown key refused"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddMapping","args":[{"lit":"%s"},{"lit":"%s"},{"lit":"NoSuchKeyRqlSpec"}]}]})RQL"), *ContextPath, *ActionPath), true));
		});

		It("refuses an invalid asset path", [this]()
		{
			// enhancedInput | call CreateAction("/Engine/IA_RqlSpec")
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateAction","args":[{"lit":"/Engine/IA_RqlSpec"}]}]})RQL"), true, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), bResultOk);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
