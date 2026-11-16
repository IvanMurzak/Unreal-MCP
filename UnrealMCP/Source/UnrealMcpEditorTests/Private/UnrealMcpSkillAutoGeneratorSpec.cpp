// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Skills/UnrealMcpSkillAutoGenerator.h"

/**
 * The per-agent "auto-generate skills" setting: the requests FUnrealMcpSkillAutoGenerator sends to the sidecar (the panel
 * button's agent-generate-skills), its debounce, and which agent-config results it claims.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpSkillAutoGeneratorSpec, "UnrealMcp.Skills.AutoGenerate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	TArray<TSharedPtr<FJsonObject>> Sent;
	TArray<FString> Agents;
	bool bConnected = true;

	TUniquePtr<FUnrealMcpSkillAutoGenerator> Make()
	{
		FUnrealMcpSkillAutoGenerator::FDeps Deps;
		Deps.Agents = [this]() { return Agents; };
		Deps.CustomSkillsPath = []() { return FString(TEXT(".custom/skills")); };
		Deps.ProjectRoot = TEXT("/Projects/Game");
		Deps.Send = [this](const TSharedPtr<FJsonObject>& Request)
		{
			if (bConnected)
				Sent.Add(Request);
			return bConnected;
		};
		Deps.DelaySeconds = 60.0f; // never fires inside a spec: Flush stands for the ticker
		return MakeUnique<FUnrealMcpSkillAutoGenerator>(MoveTemp(Deps));
	}

END_DEFINE_SPEC(FUnrealMcpSkillAutoGeneratorSpec)

void FUnrealMcpSkillAutoGeneratorSpec::Define()
{
	BeforeEach([this]()
	{
		Sent.Reset();
		Agents = { TEXT("claude-code"), TEXT("other-custom") };
		bConnected = true;
	});

	It("sends the panel's agent-generate-skills request for every enabled agent", [this]()
	{
		TUniquePtr<FUnrealMcpSkillAutoGenerator> Generator = Make();
		TestEqual(TEXT("two requests"), Generator->Flush(), 2);
		if (!TestEqual(TEXT("sent"), Sent.Num(), 2))
			return;
		TestEqual(TEXT("type"), Sent[0]->GetStringField(TEXT("type")), FString(TEXT("agent-generate-skills")));
		TestEqual(TEXT("agent"), Sent[0]->GetStringField(TEXT("agentId")), FString(TEXT("claude-code")));
		TestTrue(TEXT("our request id"), Sent[0]->GetStringField(TEXT("requestId")).StartsWith(FUnrealMcpSkillAutoGenerator::RequestIdPrefix));
		TestEqual(TEXT("project root"), Sent[0]->GetObjectField(TEXT("settings"))->GetStringField(TEXT("projectRootPath")), FString(TEXT("/Projects/Game")));
		TestFalse(TEXT("no custom path for a known agent"), Sent[0]->HasField(TEXT("customSkillsPath")));
		TestEqual(TEXT("the Custom agent's folder"), Sent[1]->GetStringField(TEXT("customSkillsPath")), FString(TEXT(".custom/skills")));
	});

	It("debounces: a burst of changes is one generation", [this]()
	{
		TUniquePtr<FUnrealMcpSkillAutoGenerator> Generator = Make();
		Generator->Schedule(TEXT("connected"));
		Generator->Schedule(TEXT("extensions changed"));
		Generator->Schedule(TEXT("extensions changed"));
		TestTrue(TEXT("scheduled"), Generator->IsScheduled());
		TestEqual(TEXT("nothing sent before the delay"), Sent.Num(), 0);
		Generator->Flush();
		TestFalse(TEXT("the schedule is used up"), Generator->IsScheduled());
		TestEqual(TEXT("one request per agent"), Sent.Num(), 2);
	});

	It("sends nothing without enabled agents or a bridge", [this]()
	{
		TUniquePtr<FUnrealMcpSkillAutoGenerator> Generator = Make();
		Agents.Reset();
		TestEqual(TEXT("no agents"), Generator->Flush(), 0);
		Agents = { TEXT("claude-code") };
		bConnected = false;
		TestEqual(TEXT("not connected"), Generator->Flush(), 0);
	});

	It("claims only its own results", [this]()
	{
		TUniquePtr<FUnrealMcpSkillAutoGenerator> Generator = Make();
		Generator->Flush();
		TSharedPtr<FJsonObject> Ours = MakeShared<FJsonObject>();
		Ours->SetStringField(TEXT("requestId"), Sent[0]->GetStringField(TEXT("requestId")));
		Ours->SetBoolField(TEXT("ok"), true);
		Ours->SetNumberField(TEXT("filesWritten"), 40);
		Ours->SetStringField(TEXT("skillsPath"), TEXT("/Projects/Game/.claude/skills"));
		TestTrue(TEXT("its result"), Generator->HandleResult(Ours));
		TSharedPtr<FJsonObject> Panel = MakeShared<FJsonObject>();
		Panel->SetStringField(TEXT("requestId"), TEXT("req-1"));
		TestFalse(TEXT("the panel's result is not its"), Generator->HandleResult(Panel));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
