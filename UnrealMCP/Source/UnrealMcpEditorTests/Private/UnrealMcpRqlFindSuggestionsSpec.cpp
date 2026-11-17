// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UnrealMcpRqlTools.h"

/**
 * unreal.find with a label that names no actor (wave finding: "Islnd", "Island " with a trailing space) failed with a bare
 * `argument 'Terrain': value is error(not-found)` and agents spent turns listing actors. Now the error names up to three
 * close labels, and keeps its message when unreal.find is an argument or a set value (docs/rql.md "Finding actors").
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlFindSuggestionsSpec, "UnrealMcp.Tools.Rql.FindSuggestions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static UWorld* FindSpecWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	static void FindSpecCleanup()
	{
		if (UWorld* World = FindSpecWorld())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel().StartsWith(TEXT("FindSpec")))
					World->EditorDestroyActor(*It, /*bShouldModifyLevel*/ false);
			}
		}
	}

	static bool FindSpecSpawn(const TCHAR* Label)
	{
		AStaticMeshActor* Actor = FindSpecWorld() ? FindSpecWorld()->SpawnActor<AStaticMeshActor>() : nullptr;
		if (Actor)
			Actor->SetActorLabel(Label);
		return Actor != nullptr;
	}

	/** unreal.find("<Key>") | select Label */
	TSharedPtr<FJsonObject> FindSpecFind(const FString& Key)
	{
		return UnrealMcpRqlSpecHelpers::Exec(*this, FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"), *Key));
	}

END_DEFINE_SPEC(FUnrealMcpRqlFindSuggestionsSpec)

void FUnrealMcpRqlFindSuggestionsSpec::Define()
{
	using namespace UnrealMcpRqlSpecHelpers;
	BeforeEach([this]()
	{
		FindSpecCleanup();
		TestTrue(TEXT("spawned the actors"), FindSpecSpawn(TEXT("FindSpecIsland")) && FindSpecSpawn(TEXT("FindSpecIsland_old")));
	});
	AfterEach([]() { FindSpecCleanup(); });

	It("names the nearest labels for a typo", [this]()
	{
		const TSharedPtr<FJsonObject> Typo = FindSpecFind(TEXT("FindSpecIslnd"));
		const FString Said = Codes(Typo);
		TestFalse(TEXT("a typo fails"), IsOk(Typo));
		TestTrue(*FString::Printf(TEXT("not-found, naming the closest label first (%s)"), *Said), Said.Contains(TEXT("not-found"))
			&& Said.Contains(TEXT("did you mean 'FindSpecIsland' (StaticMeshActor), 'FindSpecIsland_old' (StaticMeshActor)")));
	});

	It("does not forgive a trailing space silently: it suggests the label", [this]()
	{
		const TSharedPtr<FJsonObject> Spaced = FindSpecFind(TEXT("FindSpecIsland "));
		TestFalse(TEXT("a trailing space fails"), IsOk(Spaced));
		TestTrue(*FString::Printf(TEXT("and names the label (%s)"), *Codes(Spaced)), Codes(Spaced).Contains(TEXT("did you mean 'FindSpecIsland' (StaticMeshActor)")));
	});

	It("finds a label in another case, and suggests one for a typo in another case", [this]()
	{
		const TSharedPtr<FJsonObject> Lower = FindSpecFind(TEXT("findspecisland"));
		TestTrue(*FString::Printf(TEXT("another case finds it (%s)"), *Codes(Lower)), IsOk(Lower) && CellText(Lower, 0, 0) == TEXT("FindSpecIsland"));
		const TSharedPtr<FJsonObject> Upper = FindSpecFind(TEXT("FINDSPECISLND"));
		TestTrue(*FString::Printf(TEXT("a typo in capitals is suggested the label (%s)"), *Codes(Upper)),
			!IsOk(Upper) && Codes(Upper).Contains(TEXT("did you mean 'FindSpecIsland' (StaticMeshActor)")));
	});

	It("says nothing about similar labels when none is close, and names the open level instead", [this]()
	{
		const TSharedPtr<FJsonObject> Far = FindSpecFind(TEXT("QxvNothingAlikeZ"));
		const FString Said = Codes(Far) + TEXT(" ") + Hints(Far);
		TestTrue(*FString::Printf(TEXT("not-found (%s)"), *Said), !IsOk(Far) && Said.Contains(TEXT("not-found")));
		TestFalse(*FString::Printf(TEXT("no suggestion line (%s)"), *Said), Said.Contains(TEXT("did you mean")));
		TestTrue(*FString::Printf(TEXT("the open-level hint (%s)"), *Said), Said.Contains(TEXT("editor | call OpenLevel(")));
	});

	It("keeps the suggestions when unreal.find is a value or the label is a reference", [this]()
	{
		// actors | where Label == "FindSpecIsland_old" | set Parent = unreal.find("FindSpecIslnd")
		const TSharedPtr<FJsonObject> AsValue = Exec(*this, TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"FindSpecIsland_old"}]}},{"op":"set","assign":[{"target":{"path":["Parent"]},"value":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"FindSpecIslnd"}]}}]}]})RQL"), /*bMutate*/ true);
		TestTrue(*FString::Printf(TEXT("the error value keeps its message (%s)"), *Codes(AsValue)),
			!IsOk(AsValue) && Codes(AsValue).Contains(TEXT("did you mean 'FindSpecIsland' (StaticMeshActor)")));

		// editor | call SpawnActor("PointLight", "FindSpecLamp", [0, 0, 0], [0, 0, 0], unreal.find("FindSpecIslnd"))
		const TSharedPtr<FJsonObject> AsArgument = Exec(*this, TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"FindSpecLamp"},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"FindSpecIslnd"}]}]}]})RQL"), /*bMutate*/ true);
		TestTrue(*FString::Printf(TEXT("as an operation argument too (%s)"), *Codes(AsArgument)),
			!IsOk(AsArgument) && Codes(AsArgument).Contains(TEXT("did you mean 'FindSpecIsland' (StaticMeshActor)")));

		// actors | where Label == "FindSpecIsland_old" | set Parent = "FindSpecIslnd"
		const TSharedPtr<FJsonObject> AsText = Exec(*this, TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"FindSpecIsland_old"}]}},{"op":"set","assign":[{"target":{"path":["Parent"]},"value":{"lit":"FindSpecIslnd"}}]}]})RQL"), /*bMutate*/ true);
		TestTrue(*FString::Printf(TEXT("a label as a reference names the nearest labels (%s)"), *Codes(AsText)),
			!IsOk(AsText) && Codes(AsText).Contains(TEXT("did you mean 'FindSpecIsland' (StaticMeshActor)")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
