// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
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
}

#endif // WITH_DEV_AUTOMATION_TESTS
