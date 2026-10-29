// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpRuntimeCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * `unreal-tool-list` specs: it lists every callable tool, the ones hidden from tools/list included (marked hidden, with the
 * hint that they still run by name), leaves system tools out, filters by regex, adds descriptions / inputs on request
 * and never changes visibility.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpToolListToolsSpec, "UnrealMcp.Tools.ToolList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	FUnrealMcpToolResult List(FUnrealMcpToolRegistry& Registry, TFunction<void(FJsonObject&)> Configure = nullptr)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		if (Configure)
			Configure(*Args);
		return Registry.Execute(TEXT("unreal-tool-list"), FUnrealMcpToolCall(Args));
	}

END_DEFINE_SPEC(FUnrealMcpToolListToolsSpec)

void FUnrealMcpToolListToolsSpec::Define()
{
	It("lists hidden tools marked hidden, leaves system tools out and does not change visibility", [this]()
	{
		FUnrealMcpToolRegistry Registry;
		UnrealMcpPingTool::Register(Registry); // a SYSTEM tool: never callable through tools/call
		UnrealMcpEditorTools::Register(Registry);
		UnrealMcpPythonTools::Register(Registry);
		UnrealMcpToolListTools::Register(Registry);
		TestTrue(TEXT("hide python-execute"), Registry.SetToolVisible(TEXT("python-execute"), false));

		const FUnrealMcpToolResult Result = List(Registry);
		TestTrue(TEXT("succeeds"), Result.bSuccess);
		const FString Text = Result.Message;
		TestTrue(*FString::Printf(TEXT("python-execute is listed as hidden (%s)"), *Text.Left(400)), Text.Contains(TEXT("python-execute | Execute Python | hidden")));
		TestTrue(TEXT("console-run-command is listed as visible"), Text.Contains(TEXT("console-run-command | Run Console Command | visible")));
		TestTrue(TEXT("lists itself"), Text.Contains(TEXT("unreal-tool-list | List Tools | visible")));
		TestTrue(TEXT("says hidden tools still run by name"), Text.Contains(TEXT("still run when called by name")));
		TestFalse(TEXT("leaves the system tool out"), Text.Contains(TEXT("ping |")));
		TestTrue(TEXT("counts one hidden"), Text.StartsWith(TEXT("3 tool(s), 1 hidden")));
		const FUnrealMcpRegisteredTool* Python = Registry.Find(TEXT("python-execute"));
		TestTrue(TEXT("still hidden afterwards"), Python && !Python->bVisible);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		TestTrue(TEXT("structured rows"), Result.Structured.IsValid() && Result.Structured->TryGetArrayField(TEXT("tools"), Rows) && Rows->Num() == 3);
	});

	It("filters by a case-insensitive regex and adds descriptions and inputs on request", [this]()
	{
		FUnrealMcpToolRegistry Registry;
		UnrealMcpEditorTools::Register(Registry);
		UnrealMcpPythonTools::Register(Registry);
		UnrealMcpToolListTools::Register(Registry);

		const FString Filtered = List(Registry, [](FJsonObject& Args) { Args.SetStringField(TEXT("filter"), TEXT("^PYTHON")); }).Message;
		TestTrue(*FString::Printf(TEXT("only python-execute (%s)"), *Filtered.Left(300)), Filtered.Contains(TEXT("python-execute |")) && !Filtered.Contains(TEXT("console-run-command |")));
		TestFalse(TEXT("no description by default"), Filtered.Contains(TEXT("Run Python inside the Unreal Editor")));

		const FString Detailed = List(Registry, [](FJsonObject& Args)
		{
			Args.SetStringField(TEXT("filter"), TEXT("console"));
			Args.SetBoolField(TEXT("includeDescription"), true);
			Args.SetBoolField(TEXT("includeInputs"), true);
		}).Message;
		TestTrue(TEXT("description added"), Detailed.Contains(TEXT("Execute an Unreal console command")));
		TestTrue(TEXT("inputs added"), Detailed.Contains(TEXT("inputs: command: string (required)")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
