// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Editor.h"
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

	FUnrealMcpToolResult Run(const FString& Code, const TArray<FString>& Include = TArray<FString>())
	{
		FUnrealMcpToolRegistry Registry;
		UnrealMcpPythonTools::Register(Registry);
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("code"), Code);
		if (Include.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Path : Include)
				Values.Add(MakeShared<FJsonValueString>(Path));
			A->SetArrayField(TEXT("include"), Values);
		}
		return Registry.Execute(TEXT("python-execute"), FUnrealMcpToolCall(A));
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

		// The read-only snippet left no transaction behind, so one undo reverts the edit.
		TestTrue(TEXT("undo"), GEditor->UndoTransaction());
		TestEqual(TEXT("undone"), Lamp->PointLightComponent->Intensity, 1.f);
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
