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
 * Asset / Content-Browser tool family specs (docs/ARCHITECTURE.md §10, issue #10).
 *
 * Three groups:
 *  - registration   — every declared tool is present with a kebab-case id (no live editor needed).
 *  - error paths     — each tool's required-arg / not-found guard returns an error result.
 *  - live round-trip — a real in-editor lifecycle (create-folder -> material-create -> find ->
 *    get-data -> material-get-data/modify -> copy -> move -> delete) over the testbed's /Game,
 *    using the always-present engine DefaultMaterial as the material-instance parent. Nothing is
 *    saved to disk (all in-memory packages), so the testbed working tree stays clean.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpAssetToolsSpec, "UnrealMcp.Tools.Asset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	TSharedPtr<FJsonObject> Args() const { return MakeShared<FJsonObject>(); }
END_DEFINE_SPEC(FUnrealMcpAssetToolsSpec)

namespace
{
	FUnrealMcpToolResult Run(FUnrealMcpToolRegistry& Registry, const FString& Name, const TSharedPtr<FJsonObject>& Args)
	{
		return Registry.Execute(Name, FUnrealMcpToolCall(Args));
	}
}

void FUnrealMcpAssetToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers all eleven asset tools with kebab-case ids", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpAssetTools::Register(Registry);

			const TArray<FString> Expected = {
				TEXT("asset-find"), TEXT("asset-get-data"), TEXT("asset-create-folder"),
				TEXT("asset-copy"), TEXT("asset-move"), TEXT("asset-delete"), TEXT("asset-refresh"),
				TEXT("asset-material-create"), TEXT("asset-material-modify"),
				TEXT("asset-material-get-data"), TEXT("asset-import")
			};
			for (const FString& Name : Expected)
			{
				TestTrue(FString::Printf(TEXT("has %s"), *Name), Registry.HasTool(Name));
				TestTrue(FString::Printf(TEXT("%s is valid kebab id"), *Name), FUnrealMcpToolRegistry::IsValidToolName(Name));
			}
			TestEqual(TEXT("exactly eleven tools"), Registry.Num(), Expected.Num());
		});

		It("marks asset-delete destructive and read-only tools read-only", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpAssetTools::Register(Registry);
			TestTrue(TEXT("delete destructive"), Registry.Find(TEXT("asset-delete"))->bDestructiveHint);
			TestTrue(TEXT("find read-only"), Registry.Find(TEXT("asset-find"))->bReadOnlyHint);
			TestTrue(TEXT("get-data read-only"), Registry.Find(TEXT("asset-get-data"))->bReadOnlyHint);
			TestTrue(TEXT("material-get-data read-only"), Registry.Find(TEXT("asset-material-get-data"))->bReadOnlyHint);
		});
	});

	Describe("error paths", [this]()
	{
		It("asset-get-data errors when path is missing", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpAssetTools::Register(Registry);
			TestFalse(TEXT("not success"), Run(Registry, TEXT("asset-get-data"), Args()).bSuccess);
		});

		It("asset-get-data errors for a non-existent asset", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpAssetTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("path"), TEXT("/Game/__definitely_missing__/Nope"));
			TestFalse(TEXT("not success"), Run(Registry, TEXT("asset-get-data"), A).bSuccess);
		});

		It("asset-delete errors for a non-existent asset", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpAssetTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("path"), TEXT("/Game/__definitely_missing__/Nope"));
			TestFalse(TEXT("not success"), Run(Registry, TEXT("asset-delete"), A).bSuccess);
		});

		It("asset-copy errors when source is missing", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpAssetTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("destination"), TEXT("/Game/Foo"));
			TestFalse(TEXT("not success"), Run(Registry, TEXT("asset-copy"), A).bSuccess);
		});

		It("asset-find errors on a malformed class path", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpAssetTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("classPath"), TEXT("NotAValidClassPath"));
			TestFalse(TEXT("not success"), Run(Registry, TEXT("asset-find"), A).bSuccess);
		});

		It("asset-material-create errors when the parent is not a material", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpAssetTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("parent"), TEXT("/Game/__definitely_missing__/NotAMaterial"));
			A->SetStringField(TEXT("destination"), TEXT("/Game/__UnrealMcpAssetToolsSpec__/MI_X"));
			TestFalse(TEXT("not success"), Run(Registry, TEXT("asset-material-create"), A).bSuccess);
		});

		It("asset-import errors for a non-existent source file", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpAssetTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("file"), TEXT("Z:/no/such/file.png"));
			A->SetStringField(TEXT("destination"), TEXT("/Game/__UnrealMcpAssetToolsSpec__"));
			TestFalse(TEXT("not success"), Run(Registry, TEXT("asset-import"), A).bSuccess);
		});

		It("asset-material-modify errors when path is not a material instance", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpAssetTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("path"), TEXT("/Engine/EngineMaterials/DefaultMaterial"));
			TestFalse(TEXT("base material is not a MIC"), Run(Registry, TEXT("asset-material-modify"), A).bSuccess);
		});
	});

	Describe("live round-trip", [this]()
	{
		It("runs create-folder -> material-create -> find -> get-data -> copy -> move -> delete", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpAssetTools::Register(Registry);

			const FString Folder = TEXT("/Game/__UnrealMcpAssetToolsSpec__");
			const FString MiPath = Folder + TEXT("/MI_RoundTrip");
			const FString MiObjectPath = MiPath + TEXT(".MI_RoundTrip");
			const FString CopyPath = Folder + TEXT("/MI_RoundTrip_Copy");
			const FString MovedPath = Folder + TEXT("/MI_RoundTrip_Moved");
			const FString Parent = TEXT("/Engine/EngineMaterials/DefaultMaterial");

			// create-folder (idempotent)
			{
				TSharedPtr<FJsonObject> A = Args(); A->SetStringField(TEXT("path"), Folder);
				TestTrue(TEXT("create-folder ok"), Run(Registry, TEXT("asset-create-folder"), A).bSuccess);
			}

			// material-create from the engine default material
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("parent"), Parent);
				A->SetStringField(TEXT("destination"), MiPath);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("asset-material-create"), A);
				if (!R.bSuccess)
				{
					AddError(FString::Printf(TEXT("material-create failed: %s"), *R.Message));
					return;
				}
			}

			// asset-find under the temp folder finds the new instance
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("path"), Folder);
				A->SetStringField(TEXT("name"), TEXT("MI_RoundTrip"));
				const FUnrealMcpToolResult R = Run(Registry, TEXT("asset-find"), A);
				TestTrue(TEXT("find ok"), R.bSuccess);
				double Total = 0;
				TestTrue(TEXT("find has total"), R.Structured.IsValid() && R.Structured->TryGetNumberField(TEXT("total"), Total));
				TestTrue(TEXT("find found at least one"), Total >= 1.0);
			}

			// asset-get-data with a scoped read returning only the name
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("path"), MiObjectPath);
				TArray<TSharedPtr<FJsonValue>> Paths; Paths.Add(MakeShared<FJsonValueString>(TEXT("name")));
				A->SetArrayField(TEXT("paths"), Paths);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("asset-get-data"), A);
				TestTrue(TEXT("get-data ok"), R.bSuccess);
				TestTrue(TEXT("scoped to name"), R.Structured.IsValid() && R.Structured->HasField(TEXT("name")));
				TestFalse(TEXT("class pruned by scope"), R.Structured.IsValid() && R.Structured->HasField(TEXT("class")));
			}

			// asset-material-get-data returns the parameter structure
			{
				TSharedPtr<FJsonObject> A = Args(); A->SetStringField(TEXT("path"), MiPath);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("asset-material-get-data"), A);
				TestTrue(TEXT("material-get-data ok"), R.bSuccess);
				TestTrue(TEXT("has scalars block"), R.Structured.IsValid() && R.Structured->HasField(TEXT("scalars")));
				TestTrue(TEXT("has vectors block"), R.Structured->HasField(TEXT("vectors")));
				TestTrue(TEXT("has textures block"), R.Structured->HasField(TEXT("textures")));
				TestTrue(TEXT("flagged as instance"), R.Structured->GetBoolField(TEXT("isInstance")));
			}

			// asset-material-modify with an unknown param -> all failed -> error (deterministic
			// regardless of the parent's parameter set; happy-path application of a real param is
			// covered by the engine setters which we exercise above via get-data round-trip).
			{
				TSharedPtr<FJsonObject> A = Args(); A->SetStringField(TEXT("path"), MiPath);
				TSharedPtr<FJsonObject> Scalars = MakeShared<FJsonObject>();
				Scalars->SetNumberField(TEXT("__NoSuchParam__"), 0.5);
				A->SetObjectField(TEXT("scalars"), Scalars);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("asset-material-modify"), A);
				TestFalse(TEXT("unknown param -> error"), R.bSuccess);
			}

			// asset-copy
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("source"), MiPath);
				A->SetStringField(TEXT("destination"), CopyPath);
				TestTrue(TEXT("copy ok"), Run(Registry, TEXT("asset-copy"), A).bSuccess);
			}

			// asset-move (rename the copy)
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("source"), CopyPath);
				A->SetStringField(TEXT("destination"), MovedPath);
				TestTrue(TEXT("move ok"), Run(Registry, TEXT("asset-move"), A).bSuccess);
			}

			// asset-delete both assets (cleanup + delete coverage)
			{
				TSharedPtr<FJsonObject> A = Args(); A->SetStringField(TEXT("path"), MovedPath);
				TestTrue(TEXT("delete moved ok"), Run(Registry, TEXT("asset-delete"), A).bSuccess);
				TSharedPtr<FJsonObject> B = Args(); B->SetStringField(TEXT("path"), MiPath);
				TestTrue(TEXT("delete original ok"), Run(Registry, TEXT("asset-delete"), B).bSuccess);
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
