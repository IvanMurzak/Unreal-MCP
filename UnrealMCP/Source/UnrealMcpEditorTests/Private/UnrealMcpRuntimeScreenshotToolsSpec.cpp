// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRuntimeCoreTools.h" // §12.7: the runtime screenshot subset lives in the runtime module (R4)
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"

/**
 * Runtime screenshot subset specs (docs/ARCHITECTURE.md §10, issue #17, §12.7). Covers the two
 * runtime-safe tools — screenshot-game-view, screenshot-camera — moved DOWN into the runtime module in
 * R4. GPU-free: registration, the dimension clamp/cap logic, and every argument / precondition error
 * branch (missing 'camera', unresolved camera ref, no active game viewport). The real pixel-capture
 * paths are LIVE-VERIFIED WINDOWED over the bridge (no GPU under `-nullrhi`); the headless harness asserts
 * a clean error (never a crash) when rendering is unavailable.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRuntimeScreenshotToolsSpec, "UnrealMcp.Tools.RuntimeScreenshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	TSharedPtr<FJsonObject> Args() const { return MakeShared<FJsonObject>(); }
END_DEFINE_SPEC(FUnrealMcpRuntimeScreenshotToolsSpec)

namespace
{
	FUnrealMcpToolResult RunRuntimeScreenshot(FUnrealMcpToolRegistry& Registry, const FString& Name, const TSharedPtr<FJsonObject>& Args)
	{
		return Registry.Execute(Name, FUnrealMcpToolCall(Args));
	}
}

void FUnrealMcpRuntimeScreenshotToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers the two runtime screenshot tools with kebab-case ids", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpRuntimeScreenshotTools::Register(Registry);

			const TArray<FString> Expected = {
				TEXT("screenshot-game-view"), TEXT("screenshot-camera")
			};
			for (const FString& Name : Expected)
			{
				TestTrue(FString::Printf(TEXT("has %s"), *Name), Registry.HasTool(Name));
				TestTrue(FString::Printf(TEXT("%s is a valid kebab id"), *Name), FUnrealMcpToolRegistry::IsValidToolName(Name));
			}
			TestEqual(TEXT("exactly two tools"), Registry.Num(), Expected.Num());
		});

		It("marks every runtime screenshot tool read-only (capture never mutates the scene)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpRuntimeScreenshotTools::Register(Registry);
			const TArray<FString> Names = {
				TEXT("screenshot-game-view"), TEXT("screenshot-camera")
			};
			for (const FString& Name : Names)
			{
				const FUnrealMcpRegisteredTool* Tool = Registry.Find(Name);
				TestNotNull(FString::Printf(TEXT("%s present"), *Name), Tool);
				if (Tool)
					TestTrue(FString::Printf(TEXT("%s read-only"), *Name), Tool->bReadOnlyHint);
			}
		});
	});

	Describe("dimension logic", [this]()
	{
		It("ResolveCaptureDimension clamps to [1, 2048] and defaults unset to 1024", [this]()
		{
			TestEqual(TEXT("zero -> default"), UnrealMcpRuntimeScreenshotTools::ResolveCaptureDimension(0), (int32)UnrealMcpRuntimeScreenshotTools::DefaultCaptureDimension);
			TestEqual(TEXT("negative -> default"), UnrealMcpRuntimeScreenshotTools::ResolveCaptureDimension(-7), (int32)UnrealMcpRuntimeScreenshotTools::DefaultCaptureDimension);
			TestEqual(TEXT("one stays one"), UnrealMcpRuntimeScreenshotTools::ResolveCaptureDimension(1), 1);
			TestEqual(TEXT("mid passes through"), UnrealMcpRuntimeScreenshotTools::ResolveCaptureDimension(800), 800);
			TestEqual(TEXT("at cap"), UnrealMcpRuntimeScreenshotTools::ResolveCaptureDimension(2048), 2048);
			TestEqual(TEXT("over cap clamps"), UnrealMcpRuntimeScreenshotTools::ResolveCaptureDimension(5000), (int32)UnrealMcpRuntimeScreenshotTools::MaxCaptureDimension);
		});

		It("CapToMaxDimension downscales proportionally only when the longest side exceeds the cap", [this]()
		{
			int32 W = 0, H = 0;
			UnrealMcpRuntimeScreenshotTools::CapToMaxDimension(1920, 1080, W, H);
			TestEqual(TEXT("within cap width unchanged"), W, 1920);
			TestEqual(TEXT("within cap height unchanged"), H, 1080);

			UnrealMcpRuntimeScreenshotTools::CapToMaxDimension(4096, 2048, W, H);
			TestEqual(TEXT("over cap width = 2048"), W, 2048);
			TestEqual(TEXT("over cap height scaled"), H, 1024);

			UnrealMcpRuntimeScreenshotTools::CapToMaxDimension(0, 0, W, H);
			TestEqual(TEXT("zero floors to 1 width"), W, 1);
			TestEqual(TEXT("zero floors to 1 height"), H, 1);
		});
	});

	Describe("error paths (GPU-free)", [this]()
	{
		It("screenshot-camera errors when 'camera' is missing", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpRuntimeScreenshotTools::Register(Registry);
			TestFalse(TEXT("not success"), RunRuntimeScreenshot(Registry, TEXT("screenshot-camera"), Args()).bSuccess);
		});

		It("screenshot-camera errors for an unresolved camera reference", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpRuntimeScreenshotTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("camera"), TEXT("__definitely_missing_camera__"));
			TestFalse(TEXT("not success"), RunRuntimeScreenshot(Registry, TEXT("screenshot-camera"), A).bSuccess);
		});

		It("screenshot-game-view errors with no active game viewport", [this]()
		{
			// No PIE / game viewport runs under the Automation harness, so this is the documented
			// no-game-viewport error branch (validated before the GPU guard).
			FUnrealMcpToolRegistry Registry; UnrealMcpRuntimeScreenshotTools::Register(Registry);
			TestFalse(TEXT("not success"), RunRuntimeScreenshot(Registry, TEXT("screenshot-game-view"), Args()).bSuccess);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
