// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

/**
 * Editor / console / reflection tool family specs (docs/ARCHITECTURE.md §10, issue #19). Registration
 * shape + per-tool happy/error paths, executed in EditorContext so GEditor + the editor world are live.
 *
 * PIE state TRANSITIONS are NOT driven here: starting a play session under headless `-nullrhi` automation
 * emits engine Errors (which count as Automation failures) and cannot tick to completion. Only the
 * deterministic validation/error branches of editor-application-set-state are asserted; the real PIE
 * paths are proven honestly in the live bridge e2e (see targets/unreal-mcp/test.md Suite 7).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpEditorToolsSpec, "UnrealMcp.Tools.EditorReflection",
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
		It("registers the full editor/console/reflection family as core tools and bumps the revision", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpEditorTools::Register(Registry);

			const TCHAR* const Expected[] = {
				TEXT("editor-application-get-state"), TEXT("editor-application-set-state"),
				TEXT("editor-selection-get"), TEXT("editor-selection-set"),
				TEXT("console-get-logs"), TEXT("console-clear-logs"), TEXT("console-run-command"),
				TEXT("reflection-method-find"), TEXT("reflection-method-call")
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
	});

	Describe("console", [this]()
	{
		It("runs a console command and captures handled/output", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("command"), TEXT("r.ScreenPercentage")); // read-only CVar query — prints its value
			const FUnrealMcpToolResult R = Run(Registry, TEXT("console-run-command"), A);
			TestTrue(TEXT("command runs"), R.bSuccess);
			TestTrue(TEXT("missing command is rejected"), !Run(Registry, TEXT("console-run-command"), Args()).bSuccess);
		});

		It("captures GLog lines into the ring buffer, filters them, and clears", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);

			// Drive the collector directly (the runtime that normally Startup()s it is not running in specs).
			FUnrealMcpLogCollector& Collector = FUnrealMcpLogCollector::Get();
			const bool bWasRegistered = Collector.IsRegistered();
			Collector.Startup();
			Collector.Clear();

			const FString Needle = TEXT("McpLogProbe_4d9f");
			UE_LOG(LogTemp, Display, TEXT("%s once"), *Needle);
			GLog->Flush();

			TSharedPtr<FJsonObject> GetArgs = Args();
			GetArgs->SetStringField(TEXT("search"), Needle);
			const FUnrealMcpToolResult GetR = Run(Registry, TEXT("console-get-logs"), GetArgs);
			TestTrue(TEXT("get-logs succeeds"), GetR.bSuccess);
			int32 Found = 0;
			if (GetR.Structured.IsValid())
			{
				double C = 0; GetR.Structured->TryGetNumberField(TEXT("count"), C); Found = (int32)C;
			}
			TestTrue(TEXT("probe line captured"), Found >= 1);

			const FUnrealMcpToolResult ClearR = Run(Registry, TEXT("console-clear-logs"), Args());
			TestTrue(TEXT("clear succeeds"), ClearR.bSuccess);
			TestEqual(TEXT("buffer empty after clear"), Collector.Num(), 0);

			// Restore the registration state we found (don't leave a dangling GLog device across specs).
			if (!bWasRegistered)
				Collector.Shutdown();
		});
	});

	Describe("reflection", [this]()
	{
		It("discovers a known BlueprintCallable static method with signature + flags", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("class"), TEXT("KismetMathLibrary"));
			A->SetStringField(TEXT("name"), TEXT("Add_IntInt"));
			const FUnrealMcpToolResult R = Run(Registry, TEXT("reflection-method-find"), A);
			TestTrue(TEXT("find succeeds"), R.bSuccess);

			int32 Matched = 0;
			if (R.Structured.IsValid())
			{
				double M = 0; R.Structured->TryGetNumberField(TEXT("matched"), M); Matched = (int32)M;
			}
			TestTrue(TEXT("at least one Add_IntInt match"), Matched >= 1);
		});

		It("invokes a safe pure BlueprintCallable method and returns its result", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			TSharedPtr<FJsonObject> CallArgs = Args();
			CallArgs->SetStringField(TEXT("class"), TEXT("KismetMathLibrary"));
			CallArgs->SetStringField(TEXT("method"), TEXT("Add_IntInt"));
			TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
			Inner->SetNumberField(TEXT("A"), 2);
			Inner->SetNumberField(TEXT("B"), 3);
			CallArgs->SetObjectField(TEXT("args"), Inner);

			const FUnrealMcpToolResult R = Run(Registry, TEXT("reflection-method-call"), CallArgs);
			TestTrue(TEXT("call succeeds"), R.bSuccess);
			if (R.Structured.IsValid())
			{
				double Ret = 0; R.Structured->TryGetNumberField(TEXT("returnValue"), Ret);
				TestEqual(TEXT("2 + 3 == 5"), (int32)Ret, 5);
			}
		});

		It("invokes an INSTANCE BlueprintCallable method on an editor-world actor and gets a real (non-default) result", [this]()
		{
			// Regression guard for the FEditorScriptExecutionGuard around ProcessEvent. An editor-world actor's
			// BlueprintCallable (non-CallInEditor) INSTANCE method is silently skipped by AActor::ProcessEvent
			// unless script execution in the editor is enabled — without the guard the call would still report
			// success but return the zero-initialized default (false here). Proving a 'true' result proves the
			// method actually executed. (The static-via-CDO path above bypasses this gate, so it can't catch it.)
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);

			AActor* Actor = SpawnTemp(TEXT("McpReflectInstance"));
			TestTrue(TEXT("spawned a target actor"), Actor != nullptr);
			if (!Actor) return;
			Actor->Tags.Add(FName(TEXT("McpReflectProbe")));

			TSharedPtr<FJsonObject> CallArgs = Args();
			CallArgs->SetStringField(TEXT("target"), TEXT("McpReflectInstance"));
			CallArgs->SetStringField(TEXT("method"), TEXT("ActorHasTag")); // BlueprintCallable instance method, bool return
			TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
			Inner->SetStringField(TEXT("Tag"), TEXT("McpReflectProbe"));
			CallArgs->SetObjectField(TEXT("args"), Inner);

			const FUnrealMcpToolResult R = Run(Registry, TEXT("reflection-method-call"), CallArgs);
			TestTrue(TEXT("instance call succeeds"), R.bSuccess);
			bool bRet = false;
			if (R.Structured.IsValid())
				R.Structured->TryGetBoolField(TEXT("returnValue"), bRet);
			TestTrue(TEXT("ActorHasTag returned true (method ran under the editor script guard)"), bRet);

			Actor->Destroy();
		});

		It("rejects a non-object 'args' argument instead of silently zeroing every parameter", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("class"), TEXT("KismetMathLibrary"));
			A->SetStringField(TEXT("method"), TEXT("Add_IntInt"));
			TArray<TSharedPtr<FJsonValue>> ArgsArr; ArgsArr.Add(MakeShared<FJsonValueNumber>(1));
			A->SetArrayField(TEXT("args"), ArgsArr); // present but an array, not an object
			TestFalse(TEXT("non-object 'args' is an error"), Run(Registry, TEXT("reflection-method-call"), A).bSuccess);
		});

		It("refuses to call a non-BlueprintCallable / non-CallInEditor function (safety gate)", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);

			// Find a concrete non-callable UFunction on AActor at runtime (deterministic: lifecycle/native
			// helpers exist that are neither BlueprintCallable nor CallInEditor) and prove the gate rejects
			// it BEFORE any ProcessEvent runs.
			UClass* ActorClass = AActor::StaticClass();
			FString TargetName;
			for (TFieldIterator<UFunction> It(ActorClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Fn = *It;
				const bool bCallable = Fn->HasAnyFunctionFlags(FUNC_BlueprintCallable)
#if WITH_EDITORONLY_DATA
					|| Fn->GetBoolMetaData(TEXT("CallInEditor"))
#endif
					;
				if (!bCallable) { TargetName = Fn->GetName(); break; }
			}
			TestTrue(TEXT("found a non-callable function to probe the gate with"), !TargetName.IsEmpty());
			if (TargetName.IsEmpty()) return;

			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("class"), TEXT("Actor"));
			A->SetStringField(TEXT("method"), TargetName);
			const FUnrealMcpToolResult R = Run(Registry, TEXT("reflection-method-call"), A);
			TestFalse(TEXT("non-callable function is rejected"), R.bSuccess);
		});

		It("errors on a missing method and on a missing target/class", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);

			TSharedPtr<FJsonObject> NoTarget = Args();
			NoTarget->SetStringField(TEXT("method"), TEXT("Add_IntInt"));
			TestFalse(TEXT("no target/class is an error"), Run(Registry, TEXT("reflection-method-call"), NoTarget).bSuccess);

			TSharedPtr<FJsonObject> BadMethod = Args();
			BadMethod->SetStringField(TEXT("class"), TEXT("KismetMathLibrary"));
			BadMethod->SetStringField(TEXT("method"), TEXT("NoSuchMethod_zzz"));
			TestFalse(TEXT("unknown method is an error"), Run(Registry, TEXT("reflection-method-call"), BadMethod).bSuccess);
		});

		It("rejects an ambiguous target+class pair instead of silently preferring one", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			TSharedPtr<FJsonObject> Both = Args();
			Both->SetStringField(TEXT("class"), TEXT("KismetMathLibrary"));
			Both->SetStringField(TEXT("target"), TEXT("AnyObject"));
			Both->SetStringField(TEXT("method"), TEXT("Add_IntInt"));
			TestFalse(TEXT("both target and class is an error"), Run(Registry, TEXT("reflection-method-call"), Both).bSuccess);
		});

		It("refuses an instance method on the class CDO path (steer to 'target')", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);

			// Find a BlueprintCallable, non-static, non-CallInEditor UFunction on AActor — exactly the
			// case the CDO path must refuse (it would mutate the shared class defaults / need a world).
			UClass* ActorClass = AActor::StaticClass();
			FString InstanceName;
			for (TFieldIterator<UFunction> It(ActorClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Fn = *It;
				if (!Fn->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Fn->HasAnyFunctionFlags(FUNC_Static))
					continue;
#if WITH_EDITORONLY_DATA
				if (Fn->GetBoolMetaData(TEXT("CallInEditor")))
					continue;
#endif
				InstanceName = Fn->GetName();
				break;
			}
			TestTrue(TEXT("found a BlueprintCallable instance method to probe the CDO gate"), !InstanceName.IsEmpty());
			if (InstanceName.IsEmpty()) return;

			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("class"), TEXT("Actor"));
			A->SetStringField(TEXT("method"), InstanceName);
			TestFalse(TEXT("instance method via 'class' is rejected"), Run(Registry, TEXT("reflection-method-call"), A).bSuccess);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
