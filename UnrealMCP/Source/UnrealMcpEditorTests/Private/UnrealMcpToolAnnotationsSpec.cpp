// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Session/UnrealMcpEditorSession.h"

/**
 * MCP tool annotations of every built-in tool (the Claude Connectors Directory rule: each tool declares a `title`
 * and an explicit `readOnlyHint` / `destructiveHint`, plus `idempotentHint` / `openWorldHint`).
 *
 * Builds the registry the way FUnrealMcpEditorCoordinator does (UnrealMcpCoreTools::RegisterAll + editor-quit). The
 * rule: every tool has a title and DECLARES ReadOnlyHint() and DestructiveHint() (the wire always carries booleans, so
 * the builder's *Declared flags are what tell a decision from a default). The reviewed values are pinned in
 * AnnotationsExpected and checked on the §2.2 manifest descriptors the sidecar maps onto the MCP annotations; a new
 * tool fails here until its hints are decided and listed. The bridge-side guard is bridge/tests/ToolAnnotationsTests.cs.
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
	static const TMap<FString, FAnnotationsHints>& AnnotationsExpected()
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
			{ TEXT("fab-add"), false, true, false, true },
			{ TEXT("fab-library"), true, false, true, true },
			{ TEXT("fab-search"), true, false, true, true },
			{ TEXT("ping"), true, false, true, false },
			{ TEXT("python-execute"), false, true, false, true },
			{ TEXT("rql-mutate"), false, true, false, false },
			{ TEXT("rql-query"), true, false, true, false },
			{ TEXT("rql-schema"), true, false, true, false },
			{ TEXT("screenshot-camera"), false, false, false, false },
			{ TEXT("screenshot-game-view"), false, false, false, false },
			{ TEXT("screenshot-isolated"), true, false, true, false },
			{ TEXT("screenshot-viewport"), false, false, false, false },
			{ TEXT("source-compile"), false, false, true, false },
			{ TEXT("source-create-class"), false, true, false, false },
			{ TEXT("source-delete"), false, true, true, false },
			{ TEXT("source-list"), true, false, true, false },
			{ TEXT("source-read"), true, false, true, false },
			{ TEXT("source-update"), false, true, false, false },
			{ TEXT("unreal-skill-create"), false, true, false, false },
			{ TEXT("unreal-tool-list"), true, false, true, false },
		};
		static const TMap<FString, FAnnotationsHints> Expected = []()
		{
			TMap<FString, FAnnotationsHints> Out;
			for (const FAnnotationsHints& Row : Table)
				Out.Add(Row.Name, Row);
			return Out;
		}();
		return Expected;
	}

	/** The registry FUnrealMcpEditorCoordinator::Startup builds (core families + the session's editor-quit). */
	TUniquePtr<FUnrealMcpToolRegistry> AnnotationsRegistry;
	/** Its manifest descriptors: the fields the sidecar maps onto the MCP tool annotations. */
	TArray<TSharedPtr<FJsonObject>> AnnotationsDescriptors;

END_DEFINE_SPEC(FUnrealMcpToolAnnotationsSpec)

void FUnrealMcpToolAnnotationsSpec::Define()
{
	BeforeEach([this]()
	{
		if (AnnotationsRegistry.IsValid())
			return;
		AnnotationsRegistry = MakeUnique<FUnrealMcpToolRegistry>();
		UnrealMcpCoreTools::RegisterAll(*AnnotationsRegistry);
		const TSharedRef<FUnrealMcpEditorSession> Session = MakeShared<FUnrealMcpEditorSession>(
			FUnrealMcpSessionEnv::Parse(TEXT(""), TEXT(""), TEXT("")), FPaths::ProjectDir());
		Session->RegisterTools(*AnnotationsRegistry);

		const TSharedPtr<FJsonObject> Manifest = AnnotationsRegistry->BuildManifestJson();
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (Manifest.IsValid() && Manifest->TryGetArrayField(TEXT("tools"), Tools) && Tools != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Tools)
			{
				if (Value.IsValid() && Value->AsObject().IsValid())
					AnnotationsDescriptors.Add(Value->AsObject());
			}
		}
	});

	Describe("built-in tools", [this]()
	{
		It("each declares a non-empty title and both a read-only and a destructive hint", [this]()
		{
			TestTrue(TEXT("manifest lists the core tools"), AnnotationsDescriptors.Num() >= AnnotationsExpected().Num());
			for (const TSharedPtr<FJsonObject>& Descriptor : AnnotationsDescriptors)
			{
				const FString Name = Descriptor->GetStringField(TEXT("name"));
				const FUnrealMcpRegisteredTool* Tool = AnnotationsRegistry->Find(Name);
				if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), *Name), Tool))
					continue;
				TestFalse(*FString::Printf(TEXT("%s has a title"), *Name), Tool->Title.TrimStartAndEnd().IsEmpty());
				TestTrue(*FString::Printf(TEXT("%s declares ReadOnlyHint()"), *Name), Tool->bReadOnlyHintDeclared);
				TestTrue(*FString::Printf(TEXT("%s declares DestructiveHint()"), *Name), Tool->bDestructiveHintDeclared);
				TestFalse(*FString::Printf(TEXT("%s is not both read-only and destructive"), *Name), Tool->bReadOnlyHint && Tool->bDestructiveHint);
			}
		});

		It("ship the reviewed hints, and every tool has a reviewed entry", [this]()
		{
			const TMap<FString, FAnnotationsHints>& Expected = AnnotationsExpected();
			TSet<FString> Seen;
			for (const TSharedPtr<FJsonObject>& Descriptor : AnnotationsDescriptors)
			{
				const FString Name = Descriptor->GetStringField(TEXT("name"));
				Seen.Add(Name);
				const FAnnotationsHints* Hints = Expected.Find(Name);
				if (!TestNotNull(*FString::Printf(TEXT("%s has reviewed hints in AnnotationsExpected"), *Name), Hints))
					continue;
				struct FAnnotationsWireHint { const TCHAR* Field; bool bExpected; };
				const FAnnotationsWireHint Wire[] = {
					{ TEXT("readOnlyHint"), Hints->bReadOnly },
					{ TEXT("destructiveHint"), Hints->bDestructive },
					{ TEXT("idempotentHint"), Hints->bIdempotent },
					{ TEXT("openWorldHint"), Hints->bOpenWorld },
				};
				for (const FAnnotationsWireHint& Hint : Wire)
				{
					bool bActual = !Hint.bExpected;
					TestTrue(*FString::Printf(TEXT("%s %s == %s"), *Name, Hint.Field, Hint.bExpected ? TEXT("true") : TEXT("false")),
						Descriptor->TryGetBoolField(Hint.Field, bActual) && bActual == Hint.bExpected);
				}
			}
			for (const TPair<FString, FAnnotationsHints>& Pair : Expected)
				TestTrue(*FString::Printf(TEXT("%s is registered"), *Pair.Key), Seen.Contains(Pair.Key));
		});

		It("name RQL and link its docs in every rql-* description", [this]()
		{
			for (const TSharedPtr<FJsonObject>& Descriptor : AnnotationsDescriptors)
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
