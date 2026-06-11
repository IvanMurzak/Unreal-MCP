// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Level / map tool family specs (docs/ARCHITECTURE.md §10, issue #16 — Unity Scene.* analog).
 *
 * Three groups:
 *  - registration   — every declared tool is present with a kebab-case id + correct hints (no live editor).
 *  - error paths     — each tool's required-arg / not-found guard returns an error result.
 *  - live round-trip — a real in-editor lifecycle (create -> list-loaded -> get-data -> set-current),
 *    driven entirely through GEditor->NewMap() (the level-create no-'path' branch), so NOTHING is
 *    written to disk and the testbed working tree stays clean. Disk save / open is proven separately by
 *    the live bridge e2e (which saves under a temp content path and cleans up).
 *
 * Headless note: every body uses the editor UWorld + UEditorLoadingAndSavingUtils + UEditorLevelUtils
 * (no ULevelEditorSubsystem / UEditorActorSubsystem), so these specs run under -nullrhi without crashing.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpLevelToolsSpec, "UnrealMcp.Tools.Level",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	TSharedPtr<FJsonObject> Args() const { return MakeShared<FJsonObject>(); }
END_DEFINE_SPEC(FUnrealMcpLevelToolsSpec)

namespace
{
	// Uniquely named (not a bare `Run`): the tests module UNITY-builds every *Spec.cpp into one TU, so an
	// anonymous-namespace `Run` with the same signature as another spec's (e.g. the asset spec's) ODR-collides.
	FUnrealMcpToolResult RunLevel(FUnrealMcpToolRegistry& Registry, const FString& Name, const TSharedPtr<FJsonObject>& Args)
	{
		return Registry.Execute(Name, FUnrealMcpToolCall(Args));
	}
}

void FUnrealMcpLevelToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers all seven level tools with kebab-case ids", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpLevelTools::Register(Registry);

			const TArray<FString> Expected = {
				TEXT("level-create"), TEXT("level-open"), TEXT("level-save"),
				TEXT("level-get-data"), TEXT("level-list-loaded"), TEXT("level-set-current"),
				TEXT("level-unload-sublevel")
			};
			for (const FString& Name : Expected)
			{
				TestTrue(FString::Printf(TEXT("has %s"), *Name), Registry.HasTool(Name));
				TestTrue(FString::Printf(TEXT("%s is valid kebab id"), *Name), FUnrealMcpToolRegistry::IsValidToolName(Name));
			}
			TestEqual(TEXT("exactly seven tools"), Registry.Num(), Expected.Num());
		});

		It("marks read-only and destructive tools correctly", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpLevelTools::Register(Registry);
			TestTrue(TEXT("get-data read-only"), Registry.Find(TEXT("level-get-data"))->bReadOnlyHint);
			TestTrue(TEXT("list-loaded read-only"), Registry.Find(TEXT("level-list-loaded"))->bReadOnlyHint);
			TestTrue(TEXT("unload-sublevel destructive"), Registry.Find(TEXT("level-unload-sublevel"))->bDestructiveHint);
		});
	});

	Describe("error paths", [this]()
	{
		It("level-open errors when path is missing", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpLevelTools::Register(Registry);
			TestFalse(TEXT("not success"), RunLevel(Registry, TEXT("level-open"), Args()).bSuccess);
		});

		It("level-open errors for a non-existent level package", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpLevelTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("path"), TEXT("/Game/__definitely_missing__/Nope"));
			TestFalse(TEXT("not success"), RunLevel(Registry, TEXT("level-open"), A).bSuccess);
		});

		It("level-open errors on a malformed asset path", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpLevelTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("path"), TEXT("not a valid path"));
			TestFalse(TEXT("not success"), RunLevel(Registry, TEXT("level-open"), A).bSuccess);
		});

		It("level-create errors for a missing template", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpLevelTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("template"), TEXT("/Game/__definitely_missing__/NoTemplate"));
			TestFalse(TEXT("not success"), RunLevel(Registry, TEXT("level-create"), A).bSuccess);
		});

		It("level-set-current errors when name is missing", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpLevelTools::Register(Registry);
			TestFalse(TEXT("not success"), RunLevel(Registry, TEXT("level-set-current"), Args()).bSuccess);
		});

		It("level-unload-sublevel errors when name is missing", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpLevelTools::Register(Registry);
			TestFalse(TEXT("not success"), RunLevel(Registry, TEXT("level-unload-sublevel"), Args()).bSuccess);
		});

		It("level-get-data errors on a non-string paths entry", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpLevelTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			// An object entry is genuinely non-stringable — unlike a JSON number, which UE's
			// FJsonValue::TryGetString coerces to its text form ("42") and would NOT trip the guard.
			TArray<TSharedPtr<FJsonValue>> Paths; Paths.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
			A->SetArrayField(TEXT("paths"), Paths);
			TestFalse(TEXT("not success"), RunLevel(Registry, TEXT("level-get-data"), A).bSuccess);
		});
	});

	Describe("live round-trip (in-memory, no disk writes)", [this]()
	{
		It("creates a blank level, lists it, snapshots it, and sets it current", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpLevelTools::Register(Registry);

			// level-create with no 'path' -> GEditor->NewMap(): a fresh in-memory world, nothing saved.
			FString LevelName;
			{
				const FUnrealMcpToolResult R = RunLevel(Registry, TEXT("level-create"), Args());
				if (!R.bSuccess)
				{
					AddError(FString::Printf(TEXT("level-create failed: %s"), *R.Message));
					return;
				}
				if (!R.Structured.IsValid())
				{
					AddError(TEXT("level-create returned no structured payload"));
					return;
				}
				TestTrue(TEXT("create reports not-saved"), !R.Structured->GetBoolField(TEXT("saved")));
				TestTrue(TEXT("create reports a persistent level"), R.Structured->GetBoolField(TEXT("isPersistent")));
				LevelName = R.Structured->GetStringField(TEXT("name"));
				TestTrue(TEXT("create returns a level name"), !LevelName.IsEmpty());
			}

			// level-list-loaded: at least the persistent level, not partitioned, persistent flagged.
			{
				const FUnrealMcpToolResult R = RunLevel(Registry, TEXT("level-list-loaded"), Args());
				TestTrue(TEXT("list-loaded ok"), R.bSuccess);
				if (!R.Structured.IsValid())
				{
					AddError(TEXT("level-list-loaded returned no structured payload"));
					return;
				}
				double Count = 0;
				TestTrue(TEXT("list-loaded has count"), R.Structured->TryGetNumberField(TEXT("count"), Count));
				TestTrue(TEXT("at least one level loaded"), Count >= 1.0);
				TestFalse(TEXT("blank world is not partitioned"), R.Structured->GetBoolField(TEXT("isPartitionedWorld")));

				const TArray<TSharedPtr<FJsonValue>>* Levels = nullptr;
				TestTrue(TEXT("has levels array"), R.Structured->TryGetArrayField(TEXT("levels"), Levels) && Levels);
				bool bSawPersistent = false;
				if (Levels)
					for (const TSharedPtr<FJsonValue>& V : *Levels)
						if (V.IsValid() && V->AsObject().IsValid() && V->AsObject()->GetBoolField(TEXT("isPersistent")))
							bSawPersistent = true;
				TestTrue(TEXT("persistent level present in list"), bSawPersistent);
			}

			// level-get-data whole-world snapshot: an actors array + numeric total.
			{
				const FUnrealMcpToolResult R = RunLevel(Registry, TEXT("level-get-data"), Args());
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
			}

			// level-set-current on the persistent level: already current -> success (idempotent).
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("name"), LevelName);
				const FUnrealMcpToolResult R = RunLevel(Registry, TEXT("level-set-current"), A);
				TestTrue(TEXT("set-current on persistent ok"), R.bSuccess);
				TestTrue(TEXT("set-current reports it current"), R.Structured.IsValid() && R.Structured->GetBoolField(TEXT("isCurrent")));
			}

			// level-set-current on an unknown name -> error.
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("name"), TEXT("__NoSuchLevel__"));
				TestFalse(TEXT("set-current unknown -> error"), RunLevel(Registry, TEXT("level-set-current"), A).bSuccess);
			}

			// level-unload-sublevel with no sublevels present -> error (nothing matches).
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("name"), TEXT("__NoSuchSublevel__"));
				TestFalse(TEXT("unload missing sublevel -> error"), RunLevel(Registry, TEXT("level-unload-sublevel"), A).bSuccess);
			}

			// level-save in place on the transient world -> error (never saved; no path).
			{
				TestFalse(TEXT("save transient in-place -> error"), RunLevel(Registry, TEXT("level-save"), Args()).bSuccess);
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
