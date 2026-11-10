// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UnrealMcpRqlTools.h"

/**
 * actors covers only the level open in the editor. An agent that asked about another map got nothing back and fell back to
 * Python (DeepSeek eval), so an empty read of actors names the open level and how to open another one.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlOpenLevelHintSpec, "UnrealMcp.Tools.Rql.OpenLevelHint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpRqlOpenLevelHintSpec)

void FUnrealMcpRqlOpenLevelHintSpec::Define()
{
	It("names the open level and OpenLevel when a read of actors finds nothing, and stays quiet otherwise", [this]()
	{
		using namespace UnrealMcpRqlSpecHelpers;
		// actors | where Label == "OpenLevelSpecNoSuchActor" | select Label
		const TSharedPtr<FJsonObject> Empty = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"OpenLevelSpecNoSuchActor"}]}},{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})"));
		TestTrue(TEXT("the empty query succeeds"), IsOk(Empty));
		TestTrue(*FString::Printf(TEXT("it warns (%s)"), *Codes(Empty)), Codes(Empty).Contains(TEXT("open-level-only")));
		const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World)
			TestTrue(*FString::Printf(TEXT("naming the open level (%s)"), *Codes(Empty)), Codes(Empty).Contains(World->GetOutermost()->GetName()));
		TestTrue(*FString::Printf(TEXT("and how to open another (%s)"), *Hints(Empty)), Hints(Empty).Contains(TEXT("editor | call OpenLevel(")));

		// actors | where Label == "OpenLevelSpecNoSuchActor" | count
		const TSharedPtr<FJsonObject> Count = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"OpenLevelSpecNoSuchActor"}]}},{"op":"count"}]})"));
		TestTrue(*FString::Printf(TEXT("a count of 0 warns too (%s)"), *Codes(Count)), Codes(Count).Contains(TEXT("open-level-only")));

		// actors | count: a level always has actors (the world settings)
		const TSharedPtr<FJsonObject> Found = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"count"}]})"));
		TestFalse(*FString::Printf(TEXT("no warning when something is found (%s)"), *Codes(Found)), Codes(Found).Contains(TEXT("open-level-only")));

		// levels | where Name == "OpenLevelSpecNoSuchLevel" | count: other sources are not about the open level's actors
		const TSharedPtr<FJsonObject> Levels = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["levels"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"OpenLevelSpecNoSuchLevel"}]}},{"op":"count"}]})"));
		TestFalse(*FString::Printf(TEXT("no warning on another source (%s)"), *Codes(Levels)), Codes(Levels).Contains(TEXT("open-level-only")));

		// actors | where Label == "OpenLevelSpecNoSuchActor" | set HiddenInEditor = true: a mutation reports its own no-match
		const TSharedPtr<FJsonObject> Mutation = Exec(*this, TEXT(R"({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"OpenLevelSpecNoSuchActor"}]}},{"op":"set","assign":[{"target":{"path":["HiddenInEditor"]},"value":{"lit":true}}]}]})"), /*bMutate*/ true);
		TestFalse(*FString::Printf(TEXT("no warning on a mutation (%s)"), *Codes(Mutation)), Codes(Mutation).Contains(TEXT("open-level-only")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
