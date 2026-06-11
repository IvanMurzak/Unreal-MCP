// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"

/**
 * Screenshot / viewport-capture tool family specs (docs/ARCHITECTURE.md §10, issue #17).
 *
 * These are GPU-free: they cover registration, the dimension clamp/cap logic, and every argument /
 * precondition error branch (missing 'camera'/'actor', unresolved refs, no PIE session). The actual
 * pixel-capture paths CANNOT run headless (`-nullrhi` has no GPU) — those are LIVE-VERIFIED WINDOWED
 * over the bridge. To keep the suite green headless, the capture branch is asserted to return a clean
 * "rendering unavailable" error (never a crash) when FApp::CanEverRender() is false, rather than being
 * exercised for real.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpScreenshotToolsSpec, "UnrealMcp.Tools.Screenshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	TSharedPtr<FJsonObject> Args() const { return MakeShared<FJsonObject>(); }
END_DEFINE_SPEC(FUnrealMcpScreenshotToolsSpec)

namespace
{
	FUnrealMcpToolResult RunScreenshot(FUnrealMcpToolRegistry& Registry, const FString& Name, const TSharedPtr<FJsonObject>& Args)
	{
		return Registry.Execute(Name, FUnrealMcpToolCall(Args));
	}
}

void FUnrealMcpScreenshotToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers all four screenshot tools with kebab-case ids", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpScreenshotTools::Register(Registry);

			const TArray<FString> Expected = {
				TEXT("screenshot-viewport"), TEXT("screenshot-game-view"),
				TEXT("screenshot-camera"), TEXT("screenshot-isolated")
			};
			for (const FString& Name : Expected)
			{
				TestTrue(FString::Printf(TEXT("has %s"), *Name), Registry.HasTool(Name));
				TestTrue(FString::Printf(TEXT("%s is a valid kebab id"), *Name), FUnrealMcpToolRegistry::IsValidToolName(Name));
			}
			TestEqual(TEXT("exactly four tools"), Registry.Num(), Expected.Num());
		});

		It("marks every screenshot tool read-only (capture never mutates the scene)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpScreenshotTools::Register(Registry);
			const TArray<FString> Names = {
				TEXT("screenshot-viewport"), TEXT("screenshot-game-view"),
				TEXT("screenshot-camera"), TEXT("screenshot-isolated")
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
			TestEqual(TEXT("zero -> default"), UnrealMcpScreenshotTools::ResolveCaptureDimension(0), (int32)UnrealMcpScreenshotTools::DefaultCaptureDimension);
			TestEqual(TEXT("negative -> default"), UnrealMcpScreenshotTools::ResolveCaptureDimension(-7), (int32)UnrealMcpScreenshotTools::DefaultCaptureDimension);
			TestEqual(TEXT("one stays one"), UnrealMcpScreenshotTools::ResolveCaptureDimension(1), 1);
			TestEqual(TEXT("mid passes through"), UnrealMcpScreenshotTools::ResolveCaptureDimension(800), 800);
			TestEqual(TEXT("at cap"), UnrealMcpScreenshotTools::ResolveCaptureDimension(2048), 2048);
			TestEqual(TEXT("over cap clamps"), UnrealMcpScreenshotTools::ResolveCaptureDimension(5000), (int32)UnrealMcpScreenshotTools::MaxCaptureDimension);
		});

		It("CapToMaxDimension downscales proportionally only when the longest side exceeds the cap", [this]()
		{
			int32 W = 0, H = 0;
			UnrealMcpScreenshotTools::CapToMaxDimension(1920, 1080, W, H);
			TestEqual(TEXT("within cap width unchanged"), W, 1920);
			TestEqual(TEXT("within cap height unchanged"), H, 1080);

			UnrealMcpScreenshotTools::CapToMaxDimension(4096, 2048, W, H);
			TestEqual(TEXT("over cap width = 2048"), W, 2048);
			TestEqual(TEXT("over cap height scaled"), H, 1024);

			UnrealMcpScreenshotTools::CapToMaxDimension(0, 0, W, H);
			TestEqual(TEXT("zero floors to 1 width"), W, 1);
			TestEqual(TEXT("zero floors to 1 height"), H, 1);
		});
	});

	Describe("error paths (GPU-free)", [this]()
	{
		It("screenshot-camera errors when 'camera' is missing", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpScreenshotTools::Register(Registry);
			TestFalse(TEXT("not success"), RunScreenshot(Registry, TEXT("screenshot-camera"), Args()).bSuccess);
		});

		It("screenshot-camera errors for an unresolved camera reference", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpScreenshotTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("camera"), TEXT("__definitely_missing_camera__"));
			TestFalse(TEXT("not success"), RunScreenshot(Registry, TEXT("screenshot-camera"), A).bSuccess);
		});

		It("screenshot-isolated errors when 'actor' is missing", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpScreenshotTools::Register(Registry);
			TestFalse(TEXT("not success"), RunScreenshot(Registry, TEXT("screenshot-isolated"), Args()).bSuccess);
		});

		It("screenshot-isolated errors for an unresolved actor reference", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpScreenshotTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("actor"), TEXT("__definitely_missing_actor__"));
			TestFalse(TEXT("not success"), RunScreenshot(Registry, TEXT("screenshot-isolated"), A).bSuccess);
		});

		It("screenshot-game-view errors with no active PIE session", [this]()
		{
			// No Play-In-Editor session runs under the Automation harness, so this is the documented
			// no-PIE error branch (validated before the GPU guard).
			FUnrealMcpToolRegistry Registry; UnrealMcpScreenshotTools::Register(Registry);
			TestFalse(TEXT("not success"), RunScreenshot(Registry, TEXT("screenshot-game-view"), Args()).bSuccess);
		});
	});

	Describe("capture path under headless (skips real capture, never fails)", [this]()
	{
		It("screenshot-viewport returns a clean error instead of crashing when rendering is unavailable", [this]()
		{
			if (FApp::CanEverRender())
			{
				// On a GPU-backed (windowed) run the capture path is live-verified separately; nothing to assert here.
				return;
			}
			FUnrealMcpToolRegistry Registry; UnrealMcpScreenshotTools::Register(Registry);
			const FUnrealMcpToolResult Result = RunScreenshot(Registry, TEXT("screenshot-viewport"), Args());
			TestFalse(TEXT("headless capture returns an error, not a success"), Result.bSuccess);
			TestTrue(TEXT("no image content on the error path"), Result.Images.Num() == 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
