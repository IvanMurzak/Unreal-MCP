// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/PointLight.h"
#include "Components/PointLightComponent.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Python tool family specs: python-execute through PythonScriptPlugin and the vendored code_exec runtime. Executed in
 * EditorContext so Python and the `unreal` module are live. The contract cases mirror Code-Exec's
 * fixtures/golden/python.json (value, logs, syntax/runtime errors, includes).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpPythonToolsSpec, "UnrealMcp.Tools.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	FUnrealMcpToolResult Run(const FString& Code, const TArray<FString>& Include = TArray<FString>(), int32 CallTimeoutMs = 0,
		TSharedPtr<FJsonValue> Timeout = nullptr)
	{
		FUnrealMcpToolRegistry Registry;
		UnrealMcpPythonTools::Register(Registry);
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("code"), Code);
		if (Timeout.IsValid())
			A->SetField(TEXT("timeout"), Timeout);
		if (Include.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Path : Include)
				Values.Add(MakeShared<FJsonValueString>(Path));
			A->SetArrayField(TEXT("include"), Values);
		}
		FUnrealMcpToolCall Call(A);
		Call.TimeoutMs = CallTimeoutMs;
		return Registry.Execute(TEXT("python-execute"), Call);
	}

END_DEFINE_SPEC(FUnrealMcpPythonToolsSpec)

void FUnrealMcpPythonToolsSpec::Define()
{
	It("registers python-execute as a core tool", [this]()
	{
		FUnrealMcpToolRegistry Registry;
		UnrealMcpPythonTools::Register(Registry);
		TestTrue(TEXT("has python-execute"), Registry.HasTool(TEXT("python-execute")));
		TestEqual(TEXT("tool count"), Registry.Num(), 1);
	});

	It("returns the last expression and the unreal module is pre-imported", [this]()
	{
		const FUnrealMcpToolResult R = Run(TEXT("x = 21\nx * 2"));
		TestTrue(TEXT("success"), R.bSuccess);
		TestTrue(TEXT("value 42"), R.Message.StartsWith(TEXT("42")));
		TestTrue(TEXT("structured response"), R.Structured.IsValid() && R.Structured->GetStringField(TEXT("value")) == TEXT("42"));

		const FUnrealMcpToolResult Version = Run(TEXT("unreal.SystemLibrary.get_engine_version()"));
		TestTrue(TEXT("unreal available"), Version.bSuccess && Version.Message.StartsWith(TEXT("5.")));
	});

	It("returns unreal objects as their paths and print output as logs", [this]()
	{
		const FUnrealMcpToolResult R = Run(TEXT("print('hello from python')\nunreal.EditorAssetLibrary.load_asset('/Engine/BasicShapes/Cube')"));
		TestTrue(TEXT("success"), R.bSuccess);
		TestTrue(TEXT("object path"), R.Message.StartsWith(TEXT("/Engine/BasicShapes/Cube.Cube")));
		TestTrue(TEXT("logs in message"), R.Message.Contains(TEXT("hello from python")));

		const FUnrealMcpToolResult Struct = Run(TEXT("unreal.Vector(1, 2, 3)"));
		TestTrue(TEXT("struct success"), Struct.bSuccess);
		TestTrue(TEXT("struct as UE text"), Struct.Message.StartsWith(TEXT("(X=1.")) && !Struct.Message.Contains(TEXT("0x")));

		const FUnrealMcpToolResult Logged = Run(TEXT("unreal.log('noted via unreal.log')\n1"));
		TestTrue(TEXT("unreal.log success"), Logged.bSuccess);
		TestTrue(TEXT("unreal.log in logs"), Logged.Message.Contains(TEXT("[info] noted via unreal.log")));
	});

	It("reports syntax errors at snippet lines and runtime errors with their frames", [this]()
	{
		const FUnrealMcpToolResult Syntax = Run(TEXT("a = 1\nb = ("));
		TestFalse(TEXT("syntax fails"), Syntax.bSuccess);
		TestTrue(TEXT("Compile kind"), Syntax.Message.StartsWith(TEXT("Compile")));
		TestTrue(TEXT("line 2"), Syntax.Message.Contains(TEXT("snippet.py(2,")));

		const FUnrealMcpToolResult Runtime = Run(TEXT("a = 1\nraise ValueError('boom')"));
		TestFalse(TEXT("runtime fails"), Runtime.bSuccess);
		TestTrue(TEXT("Runtime kind"), Runtime.Message.StartsWith(TEXT("Runtime")));
		TestTrue(TEXT("message"), Runtime.Message.Contains(TEXT("boom")));
		TestTrue(TEXT("frame"), Runtime.Message.Contains(TEXT("snippet.py\", line 2")));

		TestFalse(TEXT("missing code rejected"), Run(TEXT("  ")).bSuccess);
	});

	It("reports KeyboardInterrupt and other BaseExceptions as Runtime errors, never as a bootstrap failure", [this]()
	{
		const FUnrealMcpToolResult Interrupt = Run(TEXT("def stop():\n    raise KeyboardInterrupt('stop')\nstop()"));
		TestFalse(TEXT("fails"), Interrupt.bSuccess);
		TestTrue(*FString::Printf(TEXT("Runtime: KeyboardInterrupt (%s)"), *Interrupt.Message.Left(300)),
			Interrupt.Message.StartsWith(TEXT("Runtime: KeyboardInterrupt: stop")));
		TestTrue(TEXT("snippet frame"), Interrupt.Message.Contains(TEXT("snippet.py\", line 2, in stop")));
		TestFalse(TEXT("no runner internals"), Interrupt.Message.Contains(TEXT("runner.py")));

		for (const TCHAR* Code : { TEXT("raise GeneratorExit()"), TEXT("class Halt(BaseException):\n    pass\nraise Halt('x')"), TEXT("import sys\nsys.exit(0)") })
		{
			const FUnrealMcpToolResult R = Run(Code);
			TestTrue(*FString::Printf(TEXT("Runtime for %s"), Code), !R.bSuccess && R.Message.StartsWith(TEXT("Runtime: ")));
		}
		TestTrue(TEXT("the interpreter still works"), Run(TEXT("6 * 7")).Message.StartsWith(TEXT("42")));
	});

	It("stops an endless loop before the call's timeout and leaves the editor usable", [this]()
	{
		// A 3 s call timeout leaves the snippet half of it: the 2 s margin would leave less (the margin covers the reply).
		const double Start = FPlatformTime::Seconds();
		const FUnrealMcpToolResult R = Run(TEXT("while True:\n    pass"), {}, 3000);
		const double Elapsed = FPlatformTime::Seconds() - Start;
		TestFalse(TEXT("an error"), R.bSuccess);
		TestTrue(*FString::Printf(TEXT("a Timeout naming the budget (%s)"), *R.Message), R.Message.StartsWith(TEXT("Timeout:"))
			&& R.Message.Contains(TEXT("time budget of 1500 ms")));
		TestTrue(*FString::Printf(TEXT("stopped within the call's timeout (%.1f s)"), Elapsed), Elapsed < 3.0);
		const FUnrealMcpToolResult After = Run(TEXT("6 * 7"));
		TestTrue(TEXT("the next run works"), After.bSuccess && After.Message.StartsWith(TEXT("42")));
	});

	It("takes a 'timeout' in seconds: an endless loop with timeout 2 stops after about 2 s, far below the call's timeout", [this]()
	{
		const double Start = FPlatformTime::Seconds();
		const FUnrealMcpToolResult R = Run(TEXT("while True:\n    pass"), {}, 120000, MakeShared<FJsonValueNumber>(2));
		const double Elapsed = FPlatformTime::Seconds() - Start;
		TestTrue(*FString::Printf(TEXT("a Timeout of 2000 ms (%s)"), *R.Message), !R.bSuccess && R.Message.StartsWith(TEXT("Timeout:"))
			&& R.Message.Contains(TEXT("time budget of 2000 ms")));
		TestTrue(*FString::Printf(TEXT("stopped after about 2 s (%.1f s)"), Elapsed), Elapsed >= 1.9 && Elapsed < 20.0);
		TestFalse(TEXT("no cap note when the timeout fits"), R.Message.Contains(TEXT("was capped")));

		// Longer than the call allows: the cap (half of a 3 s call timeout here) wins and the logs say so.
		const FUnrealMcpToolResult Capped = Run(TEXT("while True:\n    pass"), {}, 3000, MakeShared<FJsonValueNumber>(100));
		TestTrue(*FString::Printf(TEXT("capped budget (%s)"), *Capped.Message), Capped.Message.Contains(TEXT("time budget of 1500 ms")));
		TestTrue(*FString::Printf(TEXT("says it was capped (%s)"), *Capped.Message), Capped.Message.Contains(TEXT("timeout 100 s was capped at 1.5 s")));

		// Without 'timeout' the default is 30 s (only the cap of a short call lowers it, silently).
		const FUnrealMcpToolResult Default = Run(TEXT("1"));
		TestTrue(TEXT("default budget 30000 ms"), Default.bSuccess && Default.Structured.IsValid() && Default.Structured->GetNumberField(TEXT("timeoutMs")) == 30000.0);
		const FUnrealMcpToolResult Fraction = Run(TEXT("1"), {}, 0, MakeShared<FJsonValueNumber>(0.07));
		TestTrue(TEXT("0.07 s is 70 ms"), Fraction.Structured.IsValid() && Fraction.Structured->GetNumberField(TEXT("timeoutMs")) == 70.0);

		for (const TSharedPtr<FJsonValue>& Bad : TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(-1),
			MakeShared<FJsonValueString>(TEXT("soon")), MakeShared<FJsonValueBoolean>(true) })
		{
			const FUnrealMcpToolResult Refused = Run(TEXT("1"), {}, 0, Bad);
			TestTrue(*FString::Printf(TEXT("invalid timeout refused (%s)"), *Refused.Message), !Refused.bSuccess
				&& Refused.Message.StartsWith(TEXT("InvalidRequest: 'timeout' must be a number of seconds")));
		}
	});

	It("stops a thread the snippet started once the run ends", [this]()
	{
		const FUnrealMcpToolResult Started = Run(TEXT("import threading\n"
			"def spin():\n    while True:\n        pass\n"
			"threading.Thread(target=spin, name='UnrealMcpSpecSpin', daemon=True).start()\n'started'"));
		TestTrue(*FString::Printf(TEXT("ran (%s)"), *Started.Message), Started.bSuccess && Started.Message.StartsWith(TEXT("started")));
		TestTrue(*FString::Printf(TEXT("warns about the running thread (%s)"), *Started.Message), Started.Message.Contains(TEXT("still running when it ended")));
		const FUnrealMcpToolResult After = Run(TEXT("import threading, time\n"
			"deadline = time.monotonic() + 3\n"
			"while time.monotonic() < deadline and any(t.name == 'UnrealMcpSpecSpin' for t in threading.enumerate()):\n    time.sleep(0.05)\n"
			"[t.name for t in threading.enumerate() if t.name == 'UnrealMcpSpecSpin']"));
		TestTrue(*FString::Printf(TEXT("the thread is gone (%s)"), *After.Message), After.bSuccess && After.Message.StartsWith(TEXT("[]")));
	});

	It("stops a snippet blocked in join, a wait or time.sleep at its budget (Code-Exec affe6ef) and leaves the editor usable", [this]()
	{
		// Before Code-Exec affe6ef each of these held the game thread for good (join / wait) or for the whole sleep.
		const TCHAR* Cases[] = {
			TEXT("import threading\ndef spin():\n    while True:\n        pass\nt = threading.Thread(target=spin, daemon=True)\nt.start()\nt.join()"),
			TEXT("import time\ntime.sleep(60)"),
			TEXT("import threading\nthreading.Event().wait()"),
			TEXT("import queue\nqueue.Queue().get()"),
			TEXT("from concurrent.futures import ThreadPoolExecutor\ndef spin():\n    while True:\n        pass\nThreadPoolExecutor(2).submit(spin).result()"),
		};
		for (const TCHAR* Code : Cases)
		{
			const double Start = FPlatformTime::Seconds();
			const FUnrealMcpToolResult R = Run(Code, {}, 120000, MakeShared<FJsonValueNumber>(1));
			const double Elapsed = FPlatformTime::Seconds() - Start;
			TestTrue(*FString::Printf(TEXT("a Timeout of 1000 ms for %s (%s)"), Code, *R.Message), !R.bSuccess
				&& R.Message.StartsWith(TEXT("Timeout:")) && R.Message.Contains(TEXT("time budget of 1000 ms")));
			TestTrue(*FString::Printf(TEXT("stopped after about 1 s for %s (%.1f s)"), Code, Elapsed), Elapsed >= 0.9 && Elapsed < 5.0);
			TestTrue(TEXT("the next run works"), Run(TEXT("6 * 7")).Message.StartsWith(TEXT("42")));
		}
		// The spinning thread and the pool worker were stopped at the deadline: no snippet thread outlives its run.
		const FUnrealMcpToolResult Threads = Run(TEXT("import threading, time\n"
			"names = lambda: [t.name for t in threading.enumerate() if t.name.startswith(('Thread-', 'ThreadPoolExecutor'))]\n"
			"deadline = time.monotonic() + 2\n"
			"while time.monotonic() < deadline and names():\n    time.sleep(0.05)\n"
			"names()"));
		TestTrue(*FString::Printf(TEXT("the snippets' threads are gone (%s)"), *Threads.Message), Threads.bSuccess && Threads.Message.StartsWith(TEXT("[]")));

		// A snippet blocked in a C lock that swallows its stop and would finish past the budget is a Timeout, not a late success.
		const FUnrealMcpToolResult Overrun = Run(TEXT("import _thread\nlock = _thread.allocate_lock()\nlock.acquire()\n"
			"try:\n    lock.acquire(timeout=2)\nexcept BaseException:\n    pass\n'late'"),
			{}, 120000, MakeShared<FJsonValueNumber>(1));
		TestTrue(*FString::Printf(TEXT("an overrun is a Timeout (%s)"), *Overrun.Message), !Overrun.bSuccess
			&& Overrun.Message.StartsWith(TEXT("Timeout:")) && Overrun.Message.Contains(TEXT("time budget of 1000 ms")));
	});

	It("caps sys.setrecursionlimit while a snippet runs (Python 3.11 would overflow the C stack) and restores it after", [this]()
	{
		const FUnrealMcpToolResult Before = Run(TEXT("import sys\nsys.getrecursionlimit()"));
		const FUnrealMcpToolResult R = Run(TEXT("import sys\nsys.setrecursionlimit(1000000)\nsys.getrecursionlimit()"));
		TestTrue(*FString::Printf(TEXT("success (%s)"), *R.Message), R.bSuccess);
		TestTrue(*FString::Printf(TEXT("capped at 3000 (%s)"), *R.Message), R.Message.StartsWith(TEXT("3000")));
		TestTrue(TEXT("says so in a warning"), R.Message.Contains(TEXT("was capped at 3000")));
		const FUnrealMcpToolResult After = Run(TEXT("import sys\nsys.getrecursionlimit()"));
		TestEqual(TEXT("the host's limit is back"), After.Message, Before.Message);
	});

	It("imports helpers from an include folder", [this]()
	{
		const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpPythonSpec"), FGuid::NewGuid().ToString());
		const FString Helper = FPaths::Combine(Folder, TEXT("mcp_spec_helpers.py"));
		TestTrue(TEXT("helper written"), FFileHelper::SaveStringToFile(TEXT("def triple(v):\n    return v * 3\n"), *Helper));
		const FUnrealMcpToolResult R = Run(TEXT("import mcp_spec_helpers\nmcp_spec_helpers.triple(14)"), { FPaths::ConvertRelativePathToFull(Folder) });
		TestTrue(TEXT("success"), R.bSuccess);
		TestTrue(TEXT("value 42"), R.Message.StartsWith(TEXT("42")));
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
	});

	It("makes a snippet's edits one undo step, and a read-only snippet adds none", [this]()
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!TestNotNull(TEXT("editor world"), World))
			return;
		// Spawned here, not from Python: EditorActorSubsystem spawning crashes a -nullrhi editor inside the engine.
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transactional;
		APointLight* Lamp = World->SpawnActor<APointLight>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!TestNotNull(TEXT("lamp spawned"), Lamp))
			return;
		Lamp->SetActorLabel(TEXT("McpPythonSpecLamp"));
		Lamp->PointLightComponent->Intensity = 1.f;
		const FString Find = TEXT("lamp = [a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() "
		                          "if a.get_actor_label() == 'McpPythonSpecLamp'][0]\n"
		                          "light = lamp.get_component_by_class(unreal.PointLightComponent)\n");

		TestTrue(TEXT("edit ok"), Run(Find + TEXT("light.set_editor_property('intensity', 7.0)")).bSuccess);
		TestEqual(TEXT("edited"), Lamp->PointLightComponent->Intensity, 7.f);
		TestTrue(TEXT("read-only ok"), Run(Find + TEXT("light.intensity")).bSuccess);
		// The step is named after the snippet's first statement, so the Undo History says which run it reverts.
		const FString Title = GEditor->Trans ? GEditor->Trans->GetUndoContext().Title.ToString() : FString();
		TestTrue(*FString::Printf(TEXT("undo step named after the code (%s)"), *Title), Title.StartsWith(TEXT("python-execute: lamp = [a for a in")));

		// The read-only snippet left no transaction behind, so one undo reverts the edit.
		TestTrue(TEXT("undo"), GEditor->UndoTransaction());
		TestEqual(TEXT("undone"), Lamp->PointLightComponent->Intensity, 1.f);
		TestFalse(TEXT("a recorded edit gets no no-undo note"), Run(Find + TEXT("light.set_editor_property('intensity', 3.0)")).Message.Contains(TEXT("recorded no undo step")));

		// A direct setter records nothing (no Modify): the result says so, since an undo would revert the previous edit.
		const FUnrealMcpToolResult Direct = Run(Find + TEXT("lamp.set_actor_location(unreal.Vector(0, 0, 321), False, False)"));
		TestTrue(TEXT("direct setter ok"), Direct.bSuccess);
		TestEqual(TEXT("moved"), Lamp->GetActorLocation().Z, 321.0);
		TestTrue(*FString::Printf(TEXT("warns that nothing was recorded (%s)"), *Direct.Message.Left(500)), Direct.Message.Contains(TEXT("recorded no undo step")));
		const FUnrealMcpToolResult Modified = Run(Find + TEXT("lamp.modify()\nlamp.set_actor_location(unreal.Vector(0, 0, 654), False, False)"));
		TestFalse(TEXT("modify() first records it: no note"), Modified.Message.Contains(TEXT("recorded no undo step")));
		// A record without changes (the actor Modify()'d by a no-op write; the location lives on the root component) is
		// dropped with the transaction: still nothing to undo, still the note.
		const FUnrealMcpToolResult Unchanged = Run(Find + TEXT("lamp.set_editor_property('tags', lamp.tags)\nlamp.set_actor_location(unreal.Vector(0, 0, 987), False, False)"));
		TestTrue(*FString::Printf(TEXT("a change-less record still warns (%s)"), *Unchanged.Message.Left(500)), Unchanged.Message.Contains(TEXT("recorded no undo step")));
		TestFalse(TEXT("a read-only snippet gets no note"), Run(Find + TEXT("lamp.get_actor_location().z")).Message.Contains(TEXT("recorded no undo step")));
		World->EditorDestroyActor(Lamp, true);
	});

	It("caps runaway output: the first and last log lines, a clipped value, and says so", [this]()
	{
		const FUnrealMcpToolResult R = Run(TEXT("for i in range(5000):\n    print('line', i, 'x' * 50)\n'y' * 50000"));
		TestTrue(TEXT("success"), R.bSuccess);
		TestTrue(*FString::Printf(TEXT("bounded (%d characters)"), R.Message.Len()), R.Message.Len() < 50000);
		TestTrue(TEXT("value clipped"), R.Message.Contains(TEXT("[truncated: 20000 of 50000 characters shown")));
		TestTrue(TEXT("lines left out"), R.Message.Contains(TEXT("log line(s) left out")));
		TestTrue(TEXT("first line kept"), R.Message.Contains(TEXT("[info] line 0 ")));
		TestTrue(TEXT("last line kept"), R.Message.Contains(TEXT("[info] line 4999 ")));
		FString Value;
		TestTrue(TEXT("structured value clipped too"), R.Structured.IsValid() && R.Structured->TryGetStringField(TEXT("value"), Value) && Value.Len() < 21000);
	});

	It("clips a long line once, and reports the plugin's own cuts in 'truncated'", [this]()
	{
		const FUnrealMcpToolResult Printed = Run(TEXT("print('x' * 5000)"));
		TestTrue(TEXT("print clipped by the runtime, real length"), Printed.Message.Contains(TEXT("[truncated: 2000 of 5000 characters shown; print less]")));
		TestFalse(TEXT("not clipped a second time"), Printed.Message.Contains(TEXT("of 2058 characters")));

		const FUnrealMcpToolResult Logged = Run(TEXT("unreal.log('z' * 5000)\n1"));
		TestTrue(TEXT("unreal.log line clipped by the plugin"), Logged.Message.Contains(TEXT("[truncated: 2000 of 5000 characters shown; print less]")));
		const TArray<TSharedPtr<FJsonValue>>* Truncated = nullptr;
		TestTrue(TEXT("truncated says logs"), Logged.Structured.IsValid() && Logged.Structured->TryGetArrayField(TEXT("truncated"), Truncated)
			&& Truncated->Num() == 1 && (*Truncated)[0]->AsString() == TEXT("logs"));
	});

	It("returns engine warnings and errors logged while the snippet ran", [this]()
	{
		// A warning, not an error: Automation rewrites expected errors to Verbose before log devices see them. An empty
		// list returns before ConsolidateAssets touches anything.
		const FUnrealMcpToolResult R = Run(TEXT("unreal.EditorAssetLibrary.consolidate_assets(unreal.load_asset('/Engine/BasicShapes/Cube'), [])"));
		TestTrue(TEXT("success"), R.bSuccess);
		TestTrue(TEXT("engine line in logs"), R.Message.Contains(TEXT("[warning] LogEditorAssetSubsystem: ConsolidateAssets: No assets to consolidate.")));

		const FUnrealMcpToolResult Quiet = Run(TEXT("1 + 1"));
		TestFalse(TEXT("earlier lines are not repeated"), Quiet.Message.Contains(TEXT("ConsolidateAssets")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
