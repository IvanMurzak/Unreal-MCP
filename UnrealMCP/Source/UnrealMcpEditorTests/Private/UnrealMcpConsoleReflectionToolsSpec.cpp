// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRuntimeCoreTools.h" // §12.7: the console + reflection runtime subset lives in the runtime module (R4)
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

/**
 * Console + reflection tool family specs (docs/ARCHITECTURE.md §10 "editor/reflection family" runtime
 * subset, §12.7). The runtime-safe console-* and reflection-method-* tools moved DOWN into the runtime
 * module in R4; these specs register UnrealMcpConsoleReflectionTools and exercise the same happy/error
 * paths the original editor-family specs covered. Executed in EditorContext so GEditor + the editor
 * world are live (the family also works over a runtime connection, but the headless editor is the
 * convenient harness; the world is the editor world via FUnrealMcpWorldProvider).
 *
 * RUNTIME GATE DIFFERENCE: the runtime safety gate accepts BlueprintCallable (instance/target) and
 * additionally Static (CDO/class) functions ONLY — the editor family's CallInEditor allowance is
 * excluded (the metadata is editor-only). The non-callable / instance-on-CDO probes below therefore
 * key off BlueprintCallable/Static, not CallInEditor.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpConsoleReflectionToolsSpec, "UnrealMcp.Tools.ConsoleReflection",
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

END_DEFINE_SPEC(FUnrealMcpConsoleReflectionToolsSpec)

void FUnrealMcpConsoleReflectionToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers the console + reflection family as core tools and bumps the revision", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpConsoleReflectionTools::Register(Registry);

			const TCHAR* const Expected[] = {
				TEXT("console-get-logs"), TEXT("console-clear-logs"), TEXT("console-run-command"),
				TEXT("reflection-method-find"), TEXT("reflection-method-call")
			};
			for (const TCHAR* Name : Expected)
				TestTrue(FString::Printf(TEXT("has %s"), Name), Registry.HasTool(Name));

			TestEqual(TEXT("tool count"), Registry.Num(), (int32)UE_ARRAY_COUNT(Expected));
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);

			const FUnrealMcpRegisteredTool* GetLogs = Registry.Find(TEXT("console-get-logs"));
			TestTrue(TEXT("get-logs found"), GetLogs != nullptr);
			if (GetLogs)
			{
				TestEqual(TEXT("core extension id"), GetLogs->ExtensionId, FString(TEXT("core")));
				TestFalse(TEXT("schema hash non-empty"), GetLogs->SchemaHash.IsEmpty());
				TestTrue(TEXT("get-logs is read-only"), GetLogs->bReadOnlyHint);
			}
		});
	});

	Describe("console", [this]()
	{
		It("runs a console command and captures handled/output", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("command"), TEXT("r.ScreenPercentage")); // read-only CVar query — prints its value
			const FUnrealMcpToolResult R = Run(Registry, TEXT("console-run-command"), A);
			TestTrue(TEXT("command runs"), R.bSuccess);
			TestTrue(TEXT("missing command is rejected"), !Run(Registry, TEXT("console-run-command"), Args()).bSuccess);
		});

		It("captures GLog lines into the ring buffer, filters them, and clears", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);

			// Drive the collector directly (the runtime that normally Startup()s it is not running in specs).
			// NOTE: the collector is a process-wide singleton. Running this spec inside a LIVE editor session
			// (rather than a throwaway -nullrhi automation process) is DESTRUCTIVE to the runtime's buffered
			// logs — the Clear() below and console-clear-logs wipe the shared buffer. Registration state is
			// snapshotted/restored at the end, but buffer CONTENTS are not. Acceptable for automation; do not
			// run against a session whose captured logs you need to keep.
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
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);
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

		It("invokes a safe pure BlueprintCallable static method and returns its result", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);
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
			// Regression guard for the FEditorScriptExecutionGuard around ProcessEvent (editor build of the
			// runtime family). An editor-world actor's BlueprintCallable INSTANCE method is silently skipped by
			// AActor::ProcessEvent unless script execution in the editor is enabled — without the guard the call
			// would still report success but return the zero-initialized default (false here). Proving a 'true'
			// result proves the method actually executed.
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);

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
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("class"), TEXT("KismetMathLibrary"));
			A->SetStringField(TEXT("method"), TEXT("Add_IntInt"));
			TArray<TSharedPtr<FJsonValue>> ArgsArr; ArgsArr.Add(MakeShared<FJsonValueNumber>(1));
			A->SetArrayField(TEXT("args"), ArgsArr); // present but an array, not an object
			TestFalse(TEXT("non-object 'args' is an error"), Run(Registry, TEXT("reflection-method-call"), A).bSuccess);
		});

		It("refuses to call a non-BlueprintCallable function (runtime safety gate)", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);

			// Find a concrete non-BlueprintCallable UFunction on AActor (deterministic: lifecycle/native
			// helpers exist that are not BlueprintCallable) and prove the runtime gate rejects it BEFORE any
			// ProcessEvent runs. (The runtime gate is BlueprintCallable-only — no CallInEditor allowance.)
			UClass* ActorClass = AActor::StaticClass();
			FString TargetName;
			for (TFieldIterator<UFunction> It(ActorClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Fn = *It;
				if (!Fn->HasAnyFunctionFlags(FUNC_BlueprintCallable)) { TargetName = Fn->GetName(); break; }
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
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);

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
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);
			TSharedPtr<FJsonObject> Both = Args();
			Both->SetStringField(TEXT("class"), TEXT("KismetMathLibrary"));
			Both->SetStringField(TEXT("target"), TEXT("AnyObject"));
			Both->SetStringField(TEXT("method"), TEXT("Add_IntInt"));
			TestFalse(TEXT("both target and class is an error"), Run(Registry, TEXT("reflection-method-call"), Both).bSuccess);
		});

		It("refuses an instance method on the class CDO path (steer to 'target')", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpConsoleReflectionTools::Register(Registry);

			// Find a BlueprintCallable, non-static UFunction on AActor — exactly the case the CDO path must
			// refuse at runtime (it would mutate the shared class defaults / need a world). The runtime CDO
			// gate accepts only Static functions.
			UClass* ActorClass = AActor::StaticClass();
			FString InstanceName;
			for (TFieldIterator<UFunction> It(ActorClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Fn = *It;
				if (!Fn->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Fn->HasAnyFunctionFlags(FUNC_Static))
					continue;
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
