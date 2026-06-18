// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

/**
 * Editor-application / selection tool family specs (docs/ARCHITECTURE.md §10, issue #19, editor-only
 * subset). Registration shape + per-tool happy/error paths, executed in EditorContext so GEditor + the
 * editor world are live. The runtime-safe console + reflection subset moved to the runtime module in R4
 * (§12.7); its specs live in UnrealMcpConsoleReflectionToolsSpec.cpp.
 *
 * PIE state TRANSITIONS are NOT driven here: starting a play session under headless `-nullrhi` automation
 * emits engine Errors (which count as Automation failures) and cannot tick to completion. Only the
 * deterministic validation/error branches of editor-application-set-state are asserted; the real PIE
 * paths are proven honestly in the live bridge e2e (see targets/unreal-mcp/test.md Suite 7).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpEditorToolsSpec, "UnrealMcp.Tools.EditorApplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static TSharedPtr<FJsonObject> Args() { return MakeShared<FJsonObject>(); }

	FUnrealMcpToolResult Run(FUnrealMcpToolRegistry& Reg, const FString& Tool, const TSharedPtr<FJsonObject>& A)
	{
		return Reg.Execute(Tool, FUnrealMcpToolCall(A));
	}

	/** A throwaway actor in the editor world, cleaned up by the caller. */
	AActor* SpawnTemp(const FString& Label)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return nullptr;
		AActor* Actor = World->SpawnActor<AActor>();
		if (Actor) Actor->SetActorLabel(Label);
		return Actor;
	}

END_DEFINE_SPEC(FUnrealMcpEditorToolsSpec)

void FUnrealMcpEditorToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers the editor-application / selection family as core tools and bumps the revision", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpEditorTools::Register(Registry);

			const TCHAR* const Expected[] = {
				TEXT("editor-application-get-state"), TEXT("editor-application-set-state"),
				TEXT("editor-selection-get"), TEXT("editor-selection-set")
			};
			for (const TCHAR* Name : Expected)
				TestTrue(FString::Printf(TEXT("has %s"), Name), Registry.HasTool(Name));

			TestEqual(TEXT("tool count"), Registry.Num(), (int32)UE_ARRAY_COUNT(Expected));
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);

			const FUnrealMcpRegisteredTool* GetState = Registry.Find(TEXT("editor-application-get-state"));
			TestTrue(TEXT("get-state found"), GetState != nullptr);
			if (GetState)
			{
				TestEqual(TEXT("core extension id"), GetState->ExtensionId, FString(TEXT("core")));
				TestFalse(TEXT("schema hash non-empty"), GetState->SchemaHash.IsEmpty());
				TestTrue(TEXT("get-state is read-only"), GetState->bReadOnlyHint);
			}
		});
	});

	Describe("editor-application", [this]()
	{
		It("reports an idle (not playing) editor state", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			const FUnrealMcpToolResult R = Run(Registry, TEXT("editor-application-get-state"), Args());
			TestTrue(TEXT("succeeds"), R.bSuccess);
			if (R.Structured.IsValid())
			{
				bool bPlaying = true;
				R.Structured->TryGetBoolField(TEXT("isPlaying"), bPlaying);
				TestFalse(TEXT("not playing in an idle editor"), bPlaying);
			}
		});

		It("rejects invalid PIE transitions and unknown actions without starting a play session", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);

			auto SetState = [&](const TCHAR* Action) -> FUnrealMcpToolResult
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("action"), Action);
				return Run(Registry, TEXT("editor-application-set-state"), A);
			};

			TestFalse(TEXT("stop while idle is an error"), SetState(TEXT("stop")).bSuccess);
			TestFalse(TEXT("pause while idle is an error"), SetState(TEXT("pause")).bSuccess);
			TestFalse(TEXT("resume while idle is an error"), SetState(TEXT("resume")).bSuccess);
			TestFalse(TEXT("unknown action is an error"), SetState(TEXT("frobnicate")).bSuccess);
		});
	});

	Describe("editor-selection", [this]()
	{
		It("sets, reads back, and clears the actor selection by ref", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);

			AActor* Actor = SpawnTemp(TEXT("McpSelTarget"));
			TestTrue(TEXT("spawned a target actor"), Actor != nullptr);
			if (!Actor) return;

			// Select by label.
			TSharedPtr<FJsonObject> SetArgs = Args();
			TArray<TSharedPtr<FJsonValue>> Refs; Refs.Add(MakeShared<FJsonValueString>(TEXT("McpSelTarget")));
			SetArgs->SetArrayField(TEXT("actors"), Refs);
			const FUnrealMcpToolResult SetR = Run(Registry, TEXT("editor-selection-set"), SetArgs);
			TestTrue(TEXT("selection-set succeeds"), SetR.bSuccess);

			// Read it back.
			const FUnrealMcpToolResult GetR = Run(Registry, TEXT("editor-selection-get"), Args());
			TestTrue(TEXT("selection-get succeeds"), GetR.bSuccess);
			int32 Count = 0;
			if (GetR.Structured.IsValid())
			{
				double C = 0; GetR.Structured->TryGetNumberField(TEXT("count"), C); Count = (int32)C;
			}
			TestEqual(TEXT("one actor selected"), Count, 1);

			// Clear.
			TSharedPtr<FJsonObject> ClearArgs = Args(); ClearArgs->SetBoolField(TEXT("clear"), true);
			const FUnrealMcpToolResult ClearR = Run(Registry, TEXT("editor-selection-set"), ClearArgs);
			TestTrue(TEXT("clear succeeds"), ClearR.bSuccess);

			const FUnrealMcpToolResult GetR2 = Run(Registry, TEXT("editor-selection-get"), Args());
			int32 Count2 = -1;
			if (GetR2.Structured.IsValid())
			{
				double C = 0; GetR2.Structured->TryGetNumberField(TEXT("count"), C); Count2 = (int32)C;
			}
			TestEqual(TEXT("selection cleared"), Count2, 0);

			Actor->Destroy();
		});

		It("rejects an unknown actor ref and leaves the selection unchanged", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			TArray<TSharedPtr<FJsonValue>> Refs; Refs.Add(MakeShared<FJsonValueString>(TEXT("NoSuchActor_zzz")));
			A->SetArrayField(TEXT("actors"), Refs);
			TestFalse(TEXT("unknown ref is an error"), Run(Registry, TEXT("editor-selection-set"), A).bSuccess);
		});

		It("rejects a non-array 'actors' argument instead of silently clearing the selection", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("actors"), TEXT("not-an-array")); // present but a string, not an array
			TestFalse(TEXT("string 'actors' is an error"), Run(Registry, TEXT("editor-selection-set"), A).bSuccess);
		});

		It("rejects a no-arg call (no actors, no clear) instead of silently deselecting everything", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			// Neither 'actors' nor clear=true: must be an explicit error, not a silent SelectNone reported
			// as success — deselecting the whole selection requires the explicit clear=true intent.
			TestFalse(TEXT("empty no-arg call is an error"), Run(Registry, TEXT("editor-selection-set"), Args()).bSuccess);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
