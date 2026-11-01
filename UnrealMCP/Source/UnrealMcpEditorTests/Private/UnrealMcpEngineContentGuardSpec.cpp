// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Curves/CurveFloat.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Safety/UnrealMcpEngineContentGuard.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UnrealMcpToolRegistry.h"

/** An agent's tool call may not change the installed engine; the user's own editor actions are unaffected. */
BEGIN_DEFINE_SPEC(FUnrealMcpEngineContentGuardSpec, "UnrealMcp.Safety.EngineContentGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpEngineContentGuardSpec)

void FUnrealMcpEngineContentGuardSpec::Define()
{
	Describe("tool call scope", [this]()
	{
		It("reports a running tool only inside its handler", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			bool bInside = false;
			Registry.Tool(TEXT("scope-probe")).Description(TEXT("probe"))
				.Handle([&bInside](const FUnrealMcpToolCall&) { bInside = FUnrealMcpToolRegistry::IsExecutingTool(); return FUnrealMcpToolResult::Success(TEXT("ok")); });
			TestFalse(TEXT("outside before"), FUnrealMcpToolRegistry::IsExecutingTool());
			TestTrue(TEXT("success"), Registry.Execute(TEXT("scope-probe"), FUnrealMcpToolCall()).bSuccess);
			TestTrue(TEXT("inside"), bInside);
			TestFalse(TEXT("outside after"), FUnrealMcpToolRegistry::IsExecutingTool());
		});

		It("turns a call with a refused write into an error that leads with the reason", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			Registry.Tool(TEXT("refused-probe")).Description(TEXT("probe"))
				.Handle([](const FUnrealMcpToolCall&)
				{
					FUnrealMcpToolRegistry::AddCallRefusal(TEXT("Refused to save '/Engine/X'."));
					return FUnrealMcpToolResult::Success(TEXT("saved"));
				});
			FUnrealMcpToolRegistry::AddCallRefusal(TEXT("outside any call: ignored"));
			const FUnrealMcpToolResult Result = Registry.Execute(TEXT("refused-probe"), FUnrealMcpToolCall());
			TestFalse(TEXT("error"), Result.bSuccess);
			TestEqual(TEXT("reason first, handler message kept"), Result.Message, FString(TEXT("Refused to save '/Engine/X'.\nsaved")));
			const FUnrealMcpToolResult Next = Registry.Execute(TEXT("refused-probe"), FUnrealMcpToolCall());
			TestEqual(TEXT("refusals do not leak into the next call"), Next.Message, FString(TEXT("Refused to save '/Engine/X'.\nsaved")));
		});
	});

	Describe("installed engine files", [this]()
	{
		It("matches files inside an installed engine only", [this]()
		{
			const FString EngineMap = FPaths::Combine(FPaths::EngineContentDir(), TEXT("Maps/Entry.umap"));
			TestEqual(TEXT("engine map"), FUnrealMcpEngineContentGuard::IsInstalledEngineFile(EngineMap), FApp::IsEngineInstalled());
			TestFalse(TEXT("project map"), FUnrealMcpEngineContentGuard::IsInstalledEngineFile(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Maps/Arena.umap"))));
			TestFalse(TEXT("empty"), FUnrealMcpEngineContentGuard::IsInstalledEngineFile(FString()));
		});

		It("refuses saving an engine package during a tool call and reports it to the agent", [this]()
		{
			if (!FApp::IsEngineInstalled())
			{
				AddInfo(TEXT("Source-built engine: the guard allows engine edits; nothing to check."));
				return;
			}
			// A NEW probe package, so a failing guard can only add a file (deleted below), never alter shipped content.
			const FString PackageName = FString::Printf(TEXT("/Engine/UnrealMcpGuardProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			UPackage* Package = CreatePackage(*PackageName);
			UCurveFloat* Asset = NewObject<UCurveFloat>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);

			FUnrealMcpToolRegistry Registry;
			bool bSaved = true;
			Registry.Tool(TEXT("save-probe")).Description(TEXT("probe"))
				.Handle([&](const FUnrealMcpToolCall&)
				{
					FSavePackageArgs Args;
					Args.TopLevelFlags = RF_Public | RF_Standalone;
					Args.SaveFlags = SAVE_NoError;
					bSaved = UPackage::SavePackage(Package, Asset, *Filename, Args);
					return FUnrealMcpToolResult::Success(TEXT("saved"));
				});
			const FUnrealMcpToolResult Result = Registry.Execute(TEXT("save-probe"), FUnrealMcpToolCall());

			const bool bFileWritten = IFileManager::Get().FileExists(*Filename);
			if (bFileWritten)
				IFileManager::Get().Delete(*Filename, false, true, true);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();

			TestFalse(TEXT("save refused"), bSaved);
			TestFalse(TEXT("no file in the engine"), bFileWritten);
			TestFalse(TEXT("call is an error"), Result.bSuccess);
			TestTrue(TEXT("names the package"), Result.Message.Contains(PackageName));
			TestTrue(TEXT("says why"), Result.Message.Contains(TEXT("installed Unreal Engine")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
