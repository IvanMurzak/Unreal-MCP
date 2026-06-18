// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRuntimeCoreTools.h" // §12.7: read-only level-get-data lives in the runtime module (R4)
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Runtime level-data tool specs (docs/ARCHITECTURE.md §10 "level family" read-only runtime subset, §12.7).
 * Covers the single runtime-safe tool, level-get-data, moved DOWN into the runtime module in R4. Runs in
 * EditorContext so the editor world resolver the editor coordinator installs (FUnrealMcpWorldProvider) is
 * live — the whole-world snapshot reads that editor world (the same code path serves the live game world
 * over a runtime connection).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRuntimeLevelToolsSpec, "UnrealMcp.Tools.RuntimeLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	TSharedPtr<FJsonObject> Args() const { return MakeShared<FJsonObject>(); }
END_DEFINE_SPEC(FUnrealMcpRuntimeLevelToolsSpec)

namespace
{
	// Uniquely named (not a bare `Run`): the tests module UNITY-builds every *Spec.cpp into one TU, so an
	// anonymous-namespace `Run` with the same signature as another spec's would ODR-collide.
	FUnrealMcpToolResult RunRuntimeLevel(FUnrealMcpToolRegistry& Registry, const FString& Name, const TSharedPtr<FJsonObject>& Args)
	{
		return Registry.Execute(Name, FUnrealMcpToolCall(Args));
	}
}

void FUnrealMcpRuntimeLevelToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers level-get-data as a read-only core tool and bumps the revision", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpRuntimeLevelTools::Register(Registry);

			TestTrue(TEXT("has level-get-data"), Registry.HasTool(TEXT("level-get-data")));
			TestTrue(TEXT("level-get-data is a valid kebab id"), FUnrealMcpToolRegistry::IsValidToolName(TEXT("level-get-data")));
			TestEqual(TEXT("exactly one tool"), Registry.Num(), 1);
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);

			const FUnrealMcpRegisteredTool* GetData = Registry.Find(TEXT("level-get-data"));
			if (TestNotNull(TEXT("level-get-data registered"), GetData))
			{
				TestEqual(TEXT("core extension id"), GetData->ExtensionId, FString(TEXT("core")));
				TestFalse(TEXT("schema hash non-empty"), GetData->SchemaHash.IsEmpty());
				TestTrue(TEXT("get-data read-only"), GetData->bReadOnlyHint);
			}
		});
	});

	Describe("error paths", [this]()
	{
		It("level-get-data errors on a non-string paths entry", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpRuntimeLevelTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			// An object entry is genuinely non-stringable — unlike a JSON number, which UE's
			// FJsonValue::TryGetString coerces to its text form ("42") and would NOT trip the guard.
			TArray<TSharedPtr<FJsonValue>> Paths; Paths.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
			A->SetArrayField(TEXT("paths"), Paths);
			TestFalse(TEXT("not success"), RunRuntimeLevel(Registry, TEXT("level-get-data"), A).bSuccess);
		});

		It("level-get-data errors for an unresolved single-actor ref", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpRuntimeLevelTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("actor"), TEXT("__definitely_missing_actor__"));
			TestFalse(TEXT("not success"), RunRuntimeLevel(Registry, TEXT("level-get-data"), A).bSuccess);
		});
	});

	Describe("whole-world snapshot", [this]()
	{
		It("returns an actors array + numeric total for the resolved world", [this]()
		{
			// The editor coordinator installed the editor-world resolver on plugin startup, so GetEditorWorld()
			// resolves the live editor world here (the same path serves the game world over a runtime connection).
			FUnrealMcpToolRegistry Registry; UnrealMcpRuntimeLevelTools::Register(Registry);
			const FUnrealMcpToolResult R = RunRuntimeLevel(Registry, TEXT("level-get-data"), Args());
			TestTrue(TEXT("get-data ok"), R.bSuccess);
			if (!R.Structured.IsValid())
			{
				AddError(TEXT("level-get-data returned no structured payload"));
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			TestTrue(TEXT("get-data has actors array"), R.Structured->TryGetArrayField(TEXT("actors"), Actors) && Actors);
			double Total = -1;
			TestTrue(TEXT("get-data has total"), R.Structured->TryGetNumberField(TEXT("total"), Total) && Total >= 0.0);
			FString World;
			TestTrue(TEXT("get-data names the world"), R.Structured->TryGetStringField(TEXT("world"), World) && !World.IsEmpty());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
