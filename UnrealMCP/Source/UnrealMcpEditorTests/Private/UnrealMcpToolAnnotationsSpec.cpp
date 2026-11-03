// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpRuntimeCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Session/UnrealMcpEditorSession.h"
#include "Tools/UnrealMcpSkillTools.h"

/**
 * MCP tool annotations of every built-in tool (the Claude Connectors Directory rule: each tool declares a `title`
 * and an explicit `readOnlyHint` / `destructiveHint`, plus `idempotentHint` / `openWorldHint`).
 *
 * Builds the registry the way FUnrealMcpEditorCoordinator does and reads the §2.2 manifest descriptors, i.e. the
 * exact fields the sidecar maps onto the MCP tool annotations. Each tool's four hints are pinned in
 * AnnotationsExpected: a new tool fails here until its hints are decided and listed, so none ships on the
 * builder's defaults by accident. The bridge-side guard is bridge/tests/ToolAnnotationsTests.cs.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpToolAnnotationsSpec, "UnrealMcp.Tools.Annotations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	struct FAnnotationsHints
	{
		const TCHAR* Name;
		bool bReadOnly;
		bool bDestructive;
		bool bIdempotent;
		bool bOpenWorld;
	};

	/** The reviewed hints of every built-in tool (readOnly, destructive, idempotent, openWorld). */
	TMap<FString, FAnnotationsHints> AnnotationsExpected() const
	{
		static const FAnnotationsHints Table[] = {
			{ TEXT("blueprint-add-component"), false, false, false, false },
			{ TEXT("blueprint-add-event"), false, false, false, false },
			{ TEXT("blueprint-add-function"), false, false, false, false },
			{ TEXT("blueprint-add-variable"), false, false, false, false },
			{ TEXT("blueprint-compile"), false, false, true, false },
			{ TEXT("blueprint-create"), false, false, false, false },
			{ TEXT("blueprint-get"), true, false, true, false },
			{ TEXT("blueprint-modify-variable"), false, true, false, false },
			{ TEXT("blueprint-remove-component"), false, true, true, false },
			{ TEXT("blueprint-set-default"), false, true, true, false },
			{ TEXT("blueprint-spawn"), false, false, false, false },
			{ TEXT("console-run-command"), false, true, false, true },
			{ TEXT("editor-quit"), false, true, false, false },
			{ TEXT("ping"), true, false, true, false },
			{ TEXT("python-execute"), false, true, false, true },
			{ TEXT("rql-mutate"), false, true, false, false },
			{ TEXT("rql-query"), true, false, true, false },
			{ TEXT("rql-schema"), true, false, true, false },
			{ TEXT("screenshot-camera"), true, false, true, false },
			{ TEXT("screenshot-game-view"), true, false, true, false },
			{ TEXT("screenshot-isolated"), true, false, true, false },
			{ TEXT("screenshot-viewport"), true, false, true, false },
			{ TEXT("source-compile"), false, false, true, false },
			{ TEXT("source-create-class"), false, true, false, false },
			{ TEXT("source-delete"), false, true, true, false },
			{ TEXT("source-list"), true, false, true, false },
			{ TEXT("source-read"), true, false, true, false },
			{ TEXT("source-update"), false, true, true, false },
			{ TEXT("unreal-skill-create"), false, true, false, false },
			{ TEXT("unreal-tool-list"), true, false, true, false },
		};
		TMap<FString, FAnnotationsHints> Out;
		for (const FAnnotationsHints& Row : Table)
			Out.Add(Row.Name, Row);
		return Out;
	}

	/** The manifest descriptors of every core tool, registered as FUnrealMcpEditorCoordinator::Startup does. */
	TArray<TSharedPtr<FJsonObject>> AnnotationsCoreDescriptors() const
	{
		FUnrealMcpToolRegistry Registry;
		UnrealMcpPingTool::Register(Registry);
		UnrealMcpRqlTools::Register(Registry);
		UnrealMcpBlueprintTools::Register(Registry);
		UnrealMcpEditorTools::Register(Registry);
		UnrealMcpToolListTools::Register(Registry);
		UnrealMcpPythonTools::Register(Registry);
		UnrealMcpScreenshotTools::Register(Registry);
		UnrealMcpSourceTools::Register(Registry);
		UnrealMcpSkillTools::Register(Registry);
		const TSharedRef<FUnrealMcpEditorSession> Session = MakeShared<FUnrealMcpEditorSession>(
			FUnrealMcpSessionEnv::Parse(TEXT(""), TEXT(""), TEXT("")), FPaths::ProjectDir());
		Session->RegisterTools(Registry);

		TArray<TSharedPtr<FJsonObject>> Out;
		const TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (Manifest.IsValid() && Manifest->TryGetArrayField(TEXT("tools"), Tools) && Tools != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Tools)
			{
				if (Value.IsValid() && Value->AsObject().IsValid())
					Out.Add(Value->AsObject());
			}
		}
		return Out;
	}

END_DEFINE_SPEC(FUnrealMcpToolAnnotationsSpec)

void FUnrealMcpToolAnnotationsSpec::Define()
{
	Describe("built-in tools", [this]()
	{
		It("each declares a non-empty title and all four hints as explicit booleans", [this]()
		{
			const TArray<TSharedPtr<FJsonObject>> Descriptors = AnnotationsCoreDescriptors();
			TestTrue(TEXT("manifest lists the core tools"), Descriptors.Num() >= AnnotationsExpected().Num());

			for (const TSharedPtr<FJsonObject>& Descriptor : Descriptors)
			{
				const FString Name = Descriptor->GetStringField(TEXT("name"));
				FString Title;
				TestTrue(*FString::Printf(TEXT("%s has a title"), *Name),
					Descriptor->TryGetStringField(TEXT("title"), Title) && !Title.TrimStartAndEnd().IsEmpty());

				for (const TCHAR* Hint : { TEXT("readOnlyHint"), TEXT("destructiveHint"), TEXT("idempotentHint"), TEXT("openWorldHint") })
				{
					bool bValue = false;
					TestTrue(*FString::Printf(TEXT("%s carries %s as a boolean"), *Name, Hint),
						Descriptor->TryGetBoolField(Hint, bValue));
				}
				TestFalse(*FString::Printf(TEXT("%s is not both read-only and destructive"), *Name),
					Descriptor->GetBoolField(TEXT("readOnlyHint")) && Descriptor->GetBoolField(TEXT("destructiveHint")));
			}
		});

		It("match the reviewed hints, and every tool has a reviewed entry", [this]()
		{
			const TMap<FString, FAnnotationsHints> Expected = AnnotationsExpected();
			TSet<FString> Seen;
			for (const TSharedPtr<FJsonObject>& Descriptor : AnnotationsCoreDescriptors())
			{
				const FString Name = Descriptor->GetStringField(TEXT("name"));
				Seen.Add(Name);
				const FAnnotationsHints* Hints = Expected.Find(Name);
				if (!TestNotNull(*FString::Printf(TEXT("%s has reviewed hints in AnnotationsExpected"), *Name), Hints))
					continue;
				TestTrue(*FString::Printf(TEXT("%s readOnlyHint == %s"), *Name, Hints->bReadOnly ? TEXT("true") : TEXT("false")), Descriptor->GetBoolField(TEXT("readOnlyHint")) == Hints->bReadOnly);
				TestTrue(*FString::Printf(TEXT("%s destructiveHint == %s"), *Name, Hints->bDestructive ? TEXT("true") : TEXT("false")), Descriptor->GetBoolField(TEXT("destructiveHint")) == Hints->bDestructive);
				TestTrue(*FString::Printf(TEXT("%s idempotentHint == %s"), *Name, Hints->bIdempotent ? TEXT("true") : TEXT("false")), Descriptor->GetBoolField(TEXT("idempotentHint")) == Hints->bIdempotent);
				TestTrue(*FString::Printf(TEXT("%s openWorldHint == %s"), *Name, Hints->bOpenWorld ? TEXT("true") : TEXT("false")), Descriptor->GetBoolField(TEXT("openWorldHint")) == Hints->bOpenWorld);
			}
			for (const TPair<FString, FAnnotationsHints>& Pair : Expected)
				TestTrue(*FString::Printf(TEXT("%s is registered"), *Pair.Key), Seen.Contains(Pair.Key));
		});

		It("name RQL and link its docs in every rql-* description", [this]()
		{
			for (const TSharedPtr<FJsonObject>& Descriptor : AnnotationsCoreDescriptors())
			{
				const FString Name = Descriptor->GetStringField(TEXT("name"));
				if (!Name.StartsWith(TEXT("rql-")))
					continue;
				const FString Description = Descriptor->GetStringField(TEXT("description"));
				TestTrue(*FString::Printf(TEXT("%s names RQL"), *Name), Description.Contains(TEXT("RQL"), ESearchCase::CaseSensitive));
				TestTrue(*FString::Printf(TEXT("%s links the RQL docs"), *Name), Description.Contains(TEXT("https://ai-game.dev/docs/rql")));
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
