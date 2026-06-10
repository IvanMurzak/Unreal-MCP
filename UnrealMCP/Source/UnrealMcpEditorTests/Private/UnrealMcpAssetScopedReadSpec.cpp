// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tools/UnrealMcpAssetScopedRead.h"

/**
 * §3.2 scoped-read filter specs (docs/ARCHITECTURE.md §3.2). Pure JSON→JSON; no editor state, so
 * these run fast and deterministically as part of the UnrealMcp. Automation suite.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpAssetScopedReadSpec, "UnrealMcp.Tools.Asset.ScopedRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	TSharedPtr<FJsonObject> MakeSource();
END_DEFINE_SPEC(FUnrealMcpAssetScopedReadSpec)

TSharedPtr<FJsonObject> FUnrealMcpAssetScopedReadSpec::MakeSource()
{
	// { name, class, scalars: { Roughness, Metallic }, vectors: { BaseColor: {r,g,b,a} } }
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("name"), TEXT("MI_Test"));
	Root->SetStringField(TEXT("class"), TEXT("/Script/Engine.MaterialInstanceConstant"));

	TSharedPtr<FJsonObject> Scalars = MakeShared<FJsonObject>();
	Scalars->SetNumberField(TEXT("Roughness"), 0.5);
	Scalars->SetNumberField(TEXT("Metallic"), 1.0);
	Root->SetObjectField(TEXT("scalars"), Scalars);

	TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
	Color->SetNumberField(TEXT("r"), 0.1);
	Color->SetNumberField(TEXT("g"), 0.2);
	Color->SetNumberField(TEXT("b"), 0.3);
	Color->SetNumberField(TEXT("a"), 1.0);
	TSharedPtr<FJsonObject> Vectors = MakeShared<FJsonObject>();
	Vectors->SetObjectField(TEXT("BaseColor"), Color);
	Root->SetObjectField(TEXT("vectors"), Vectors);

	return Root;
}

void FUnrealMcpAssetScopedReadSpec::Define()
{
	Describe("Apply", [this]()
	{
		It("returns a deep copy of the whole object when no paths are requested", [this]()
		{
			TSharedPtr<FJsonObject> Source = MakeSource();
			TSharedPtr<FJsonObject> Out = UnrealMcpAssetScopedRead::Apply(Source, {});
			TestTrue(TEXT("has name"), Out->HasField(TEXT("name")));
			TestTrue(TEXT("has scalars"), Out->HasField(TEXT("scalars")));
			TestTrue(TEXT("has vectors"), Out->HasField(TEXT("vectors")));
			// Deep copy: mutating the result must not touch the source.
			Out->SetStringField(TEXT("name"), TEXT("mutated"));
			TestEqual(TEXT("source unchanged"), Source->GetStringField(TEXT("name")), FString(TEXT("MI_Test")));
		});

		It("keeps only a requested top-level leaf, preserving its string value", [this]()
		{
			TSharedPtr<FJsonObject> Out = UnrealMcpAssetScopedRead::Apply(MakeSource(), { TEXT("name") });
			TestTrue(TEXT("has name"), Out->HasField(TEXT("name")));
			TestFalse(TEXT("no scalars"), Out->HasField(TEXT("scalars")));
			TestFalse(TEXT("no class"), Out->HasField(TEXT("class")));
			// Regression guard: the cloned leaf must keep its STRING value (not be coerced to bool/null).
			FString NameValue;
			TestTrue(TEXT("name is a string"), Out->TryGetStringField(TEXT("name"), NameValue));
			TestEqual(TEXT("name value preserved"), NameValue, FString(TEXT("MI_Test")));
		});

		It("keeps a requested nested leaf and prunes its siblings", [this]()
		{
			TSharedPtr<FJsonObject> Out = UnrealMcpAssetScopedRead::Apply(MakeSource(), { TEXT("scalars.Roughness") });
			const TSharedPtr<FJsonObject>* Scalars;
			TestTrue(TEXT("has scalars object"), Out->TryGetObjectField(TEXT("scalars"), Scalars));
			TestTrue(TEXT("has Roughness"), (*Scalars)->HasField(TEXT("Roughness")));
			TestFalse(TEXT("Metallic pruned"), (*Scalars)->HasField(TEXT("Metallic")));
			// Regression guard: the cloned number leaf keeps its value.
			TestEqual(TEXT("Roughness value preserved"), (*Scalars)->GetNumberField(TEXT("Roughness")), 0.5);
		});

		It("merges overlapping paths under a shared parent", [this]()
		{
			TSharedPtr<FJsonObject> Out = UnrealMcpAssetScopedRead::Apply(MakeSource(),
				{ TEXT("scalars.Roughness"), TEXT("scalars.Metallic") });
			const TSharedPtr<FJsonObject>* Scalars;
			TestTrue(TEXT("has scalars object"), Out->TryGetObjectField(TEXT("scalars"), Scalars));
			TestTrue(TEXT("has Roughness"), (*Scalars)->HasField(TEXT("Roughness")));
			TestTrue(TEXT("has Metallic"), (*Scalars)->HasField(TEXT("Metallic")));
		});

		It("keeps a whole requested sub-object", [this]()
		{
			TSharedPtr<FJsonObject> Out = UnrealMcpAssetScopedRead::Apply(MakeSource(), { TEXT("vectors") });
			const TSharedPtr<FJsonObject>* Vectors;
			TestTrue(TEXT("has vectors"), Out->TryGetObjectField(TEXT("vectors"), Vectors));
			TestTrue(TEXT("has BaseColor"), (*Vectors)->HasField(TEXT("BaseColor")));
			TestFalse(TEXT("no scalars"), Out->HasField(TEXT("scalars")));
		});

		It("silently skips a path that does not resolve", [this]()
		{
			TSharedPtr<FJsonObject> Out = UnrealMcpAssetScopedRead::Apply(MakeSource(),
				{ TEXT("does.not.exist"), TEXT("name") });
			TestTrue(TEXT("has name"), Out->HasField(TEXT("name")));
			TestFalse(TEXT("no does"), Out->HasField(TEXT("does")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
