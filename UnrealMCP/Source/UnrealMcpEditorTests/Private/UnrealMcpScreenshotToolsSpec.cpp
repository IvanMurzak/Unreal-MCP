// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/PostProcessVolume.h"
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

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

	// Pins the exact public signature of the DEPRECATED structured form (see the header): re-typing or deleting
	// it fails the build here rather than in an extension author's project. The call forms (3/4-arg, nullptr)
	// are pinned by the "deprecated SuccessWithImage" spec below simply by compiling — except that the nullptr
	// overload only matters on clang/GCC, and this module is built by MSVC alone in CI, so CI cannot catch its
	// removal (see the overload's doc comment).
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	using FLegacySuccessWithImageFn = FUnrealMcpToolResult (*)(const FString&, const FString&, const TSharedPtr<FJsonObject>&, const FString&);
	static_assert(std::is_same_v<FLegacySuccessWithImageFn, decltype(static_cast<FLegacySuccessWithImageFn>(&FUnrealMcpToolResult::SuccessWithImage))>,
		"The deprecated SuccessWithImage(Message, Base64, TSharedPtr<FJsonObject> Structured, MimeType) overload must keep existing");
PRAGMA_ENABLE_DEPRECATION_WARNINGS
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

		It("screenshot-isolated rejects a malformed background hex after the actor resolves", [this]()
		{
			// The background hex is parsed BEFORE the GPU guard, so a malformed value is a GPU-free error
			// branch — but it is only reachable once the 'actor' ref resolves. Spawn a transient probe actor
			// in the editor world, reference it by name, and pass invalid hex to exercise that branch.
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				// No editor world under this harness configuration; the branch is covered windowed.
				return;
			}
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
			SpawnParams.bTemporaryEditorActor = true;
			SpawnParams.bHideFromSceneOutliner = true;
			AActor* Probe = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
			TestNotNull(TEXT("spawned a probe actor"), Probe);
			if (!Probe)
				return;
			ON_SCOPE_EXIT { if (IsValid(Probe)) Probe->Destroy(); };

			FUnrealMcpToolRegistry Registry; UnrealMcpScreenshotTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("actor"), Probe->GetName());
			A->SetStringField(TEXT("background"), TEXT("not-a-hex-color"));
			const FUnrealMcpToolResult Result = RunScreenshot(Registry, TEXT("screenshot-isolated"), A);
			TestFalse(TEXT("malformed background hex is rejected"), Result.bSuccess);
			TestTrue(TEXT("error names the background hex branch"), Result.Message.Contains(TEXT("hex color")));
			TestTrue(TEXT("no image content on the error path"), Result.Images.Num() == 0);
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

	Describe("image content result shape (GPU-free)", [this]()
	{
		// The app never saw screenshots because a result carrying structured content makes McpPlugin.Server
		// return {status, structured} and drop the content array. The screenshot tools must therefore return
		// the image + a metadata text block and NO structured content.
		It("MakeImageResult returns an image block and a metadata text block with no structured content", [this]()
		{
			const FUnrealMcpToolResult Result = UnrealMcpScreenshotTools::MakeImageResult(
				TEXT("camera 'Cam1'"), 640, 480, TEXT("aGVsbG8="), 1234);

			TestTrue(TEXT("success"), Result.bSuccess);
			TestFalse(TEXT("no structured content (it would drop the image server-side)"), Result.Structured.IsValid());
			TestEqual(TEXT("exactly one image block"), Result.Images.Num(), 1);
			if (Result.Images.Num() == 1)
			{
				TestEqual(TEXT("base64 carried verbatim"), Result.Images[0].Base64Data, FString(TEXT("aGVsbG8=")));
				TestEqual(TEXT("mimeType"), Result.Images[0].MimeType, FString(TEXT("image/png")));
			}
			TestTrue(TEXT("text carries the source"), Result.Message.Contains(TEXT("camera 'Cam1'")));
			TestTrue(TEXT("text carries the dimensions"), Result.Message.Contains(TEXT("640x480")));
			TestTrue(TEXT("text carries the mimeType"), Result.Message.Contains(TEXT("image/png")));
			TestTrue(TEXT("text carries the byte size"), Result.Message.Contains(TEXT("1234")));
		});
		It("MakeImageResult appends the freshness line after the metadata", [this]()
		{
			const FString Freshness = UnrealMcpScreenshotTools::FormatFreshness(true, FString(), true, FString());
			const FUnrealMcpToolResult Result = UnrealMcpScreenshotTools::MakeImageResult(
				TEXT("editor viewport"), 64, 32, TEXT("aGVsbG8="), 12, Freshness);
			TestTrue(TEXT("metadata first"), Result.Message.StartsWith(TEXT("Captured editor viewport (64x32")));
			TestTrue(TEXT("freshness last"), Result.Message.EndsWith(Freshness));
			TestFalse(TEXT("still no structured content"), Result.Structured.IsValid());
		});

		// The freshness line is the agent's only way to tell a current frame from a stale one: its prefixes are a
		// contract (the tool descriptions quote them), so pin them.
		It("FormatFreshness states frame and simulation freshness with stable prefixes and the reasons", [this]()
		{
			const FString Fresh = UnrealMcpScreenshotTools::FormatFreshness(true, FString(), true, FString());
			TestEqual(TEXT("fresh + live"), Fresh, FString(TEXT("Frame: fresh (rendered at capture time). Simulation: live.")));

			const FString Stale = UnrealMcpScreenshotTools::FormatFreshness(false, TEXT("window minimized"), false, TEXT("throttled"));
			TestTrue(TEXT("stale prefix + frame reason"), Stale.StartsWith(TEXT("Frame: possibly stale (window minimized).")));
			TestTrue(TEXT("paused prefix + simulation reason"), Stale.Contains(TEXT("Simulation: paused (throttled)")));
			TestFalse(TEXT("never claims fresh when stale"), Stale.Contains(TEXT("Frame: fresh")));
		});

		// A blown-out frame looks "fresh" (it was rendered now), so its uniformity is the only warning an agent gets.
		// The real frames only exist with a GPU and PIE (see the file comment): these pin the measurement and the text.
		It("AnalyzeFrameUniformity flags a blank frame, also under a little overlay or noise", [this]()
		{
			using namespace UnrealMcpScreenshotTools;
			constexpr int32 Num = 1000;
			TArray<FColor> White;
			White.Init(FColor::White, Num);
			const FFrameUniformity AllWhite = AnalyzeFrameUniformity(White);
			TestTrue(TEXT("all white"), AllWhite.IsUniform());
			TestEqual(TEXT("all white color"), AllWhite.Color, FColor::White);
			TestEqual(TEXT("all white share"), AllWhite.Share, 1.0);

			TArray<FColor> Black;
			Black.Init(FColor(0, 0, 0, 0), Num);
			TestTrue(TEXT("all black, with the zero alpha of a raw read-back"), AnalyzeFrameUniformity(Black).IsUniform());

			TArray<FColor> MixedAlpha = White;
			for (int32 Index = 0; Index < Num; ++Index)
				MixedAlpha[Index].A = (uint8)(Index * 255 / Num);
			TestTrue(TEXT("alpha is ignored"), AnalyzeFrameUniformity(MixedAlpha).IsUniform());

			TArray<FColor> Overlaid = White;
			for (int32 Index = 0; Index < 9; ++Index)
				Overlaid[Index] = FColor(234, 161, 20);
			const FFrameUniformity WithOverlay = AnalyzeFrameUniformity(Overlaid);
			TestTrue(TEXT("0.9% overlay stays uniform"), WithOverlay.IsUniform());
			TestEqual(TEXT("the overlay does not move the color, a mean would"), WithOverlay.Color, FColor::White);

			TArray<FColor> Noisy;
			for (int32 Index = 0; Index < Num; ++Index)
				Noisy.Add(FColor(251 + Index % 5, 255, 255));
			TestTrue(TEXT("near-white noise is uniform"), AnalyzeFrameUniformity(Noisy).IsUniform());
		});

		It("AnalyzeFrameUniformity leaves a frame with real content alone", [this]()
		{
			using namespace UnrealMcpScreenshotTools;
			constexpr int32 Num = 1000;
			TestFalse(TEXT("an empty frame is not uniform"), AnalyzeFrameUniformity(TArray<FColor>()).IsUniform());

			TArray<FColor> JustOverAllowance;
			JustOverAllowance.Init(FColor::White, Num);
			for (int32 Index = 0; Index < 11; ++Index)
				JustOverAllowance[Index] = FColor::Black;
			TestFalse(TEXT("1.1% of other pixels"), AnalyzeFrameUniformity(JustOverAllowance).IsUniform());

			TArray<FColor> DarkGradient;
			for (int32 Index = 0; Index < Num; ++Index)
				DarkGradient.Add(FColor(Index * 10 / Num, Index * 10 / Num, Index * 10 / Num));
			TestFalse(TEXT("a low-contrast gradient, like a night sky"), AnalyzeFrameUniformity(DarkGradient).IsUniform());

			TArray<FColor> MixedChannels;
			for (int32 Index = 0; Index < Num; ++Index)
				MixedChannels.Add(Index % 2 ? FColor::Red : FColor::Blue);
			TestFalse(TEXT("two colors, half each"), AnalyzeFrameUniformity(MixedChannels).IsUniform());
		});

		It("FormatUniformFrameNote names the color, suggests screenshot-camera and adds the fix only for the known cause", [this]()
		{
			using namespace UnrealMcpScreenshotTools;
			FFrameUniformity Uniformity;
			Uniformity.Color = FColor(255, 128, 1);
			Uniformity.Share = 0.995;

			const FString Plain = FormatUniformFrameNote(Uniformity, false);
			TestTrue(TEXT("stable prefix"), Plain.StartsWith(TEXT("Frame content: uniform")));
			TestTrue(TEXT("names the color"), Plain.Contains(TEXT("#FF8001")));
			TestTrue(TEXT("names the share"), Plain.Contains(TEXT("99.5%")));
			TestTrue(TEXT("suggests the camera tool"), Plain.Contains(TEXT("screenshot-camera")));
			TestFalse(TEXT("no cause without evidence for it"), Plain.Contains(TEXT("ExtendDefaultLuminanceRange")));

			const FString Known = FormatUniformFrameNote(Uniformity, true);
			TestTrue(TEXT("still the plain note first"), Known.StartsWith(Plain));
			TestTrue(TEXT("names the project fix"), Known.Contains(TEXT("ExtendDefaultLuminanceRange=True")));
		});

		It("HasOverexposingPostProcessVolume needs the override near 0, an active volume and the CVar off", [this]()
		{
			using namespace UnrealMcpScreenshotTools;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!TestNotNull(TEXT("editor world"), World))
				return;
			TestFalse(TEXT("no volumes"), HasOverexposingPostProcessVolume({}, false));

			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
			SpawnParams.bTemporaryEditorActor = true;
			SpawnParams.bHideFromSceneOutliner = true;
			APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, SpawnParams);
			if (!TestNotNull(TEXT("spawned a volume"), Volume))
				return;
			ON_SCOPE_EXIT { if (IsValid(Volume)) Volume->Destroy(); };
			// Only this volume: the editor world may hold others, like the template level's.
			const TArray<IInterface_PostProcessVolume*> Volumes = { Volume };

			struct FRow { const TCHAR* Label; bool bExtendedRange; bool bOverride; float MaxBrightness; bool bEnabled; bool bUnbound; float Weight; bool bExpected; };
			const FRow Rows[] = {
				{ TEXT("the template's setup, CVar off"), false, true, 0.0f, true, true, 1.0f, true },
				{ TEXT("the same volume with the CVar on, where 0 means EV100 0"), true, true, 0.0f, true, true, 1.0f, false },
				{ TEXT("nearly 0, also measured white"), false, true, 0.001f, true, true, 1.0f, true },
				{ TEXT("washed out but not uniform"), false, true, 0.03f, true, true, 1.0f, false },
				{ TEXT("a normal max brightness"), false, true, 1.0f, true, true, 1.0f, false },
				{ TEXT("the value is not overridden"), false, false, 0.0f, true, true, 1.0f, false },
				{ TEXT("a disabled volume"), false, true, 0.0f, false, true, 1.0f, false },
				{ TEXT("a bounded volume, which may not reach the camera"), false, true, 0.0f, true, false, 1.0f, false },
				{ TEXT("a volume with no weight"), false, true, 0.0f, true, true, 0.0f, false },
				{ TEXT("a partial weight blends the value away from 0"), false, true, 0.0f, true, true, 0.5f, false },
			};
			for (const FRow& Row : Rows)
			{
				Volume->Settings.bOverride_AutoExposureMaxBrightness = Row.bOverride;
				Volume->Settings.AutoExposureMaxBrightness = Row.MaxBrightness;
				Volume->bEnabled = Row.bEnabled;
				Volume->bUnbound = Row.bUnbound;
				Volume->BlendWeight = Row.Weight;
				TestEqual(Row.Label, HasOverexposingPostProcessVolume(Volumes, Row.bExtendedRange), Row.bExpected);
			}

			// The template's setup again, for the checks that are not one volume's own fields.
			Volume->Settings.bOverride_AutoExposureMaxBrightness = true;
			Volume->Settings.AutoExposureMaxBrightness = 0.0f;
			Volume->bEnabled = true;
			Volume->bUnbound = true;
			Volume->BlendWeight = 1.0f;
			TestTrue(TEXT("the capture path's list holds the volume"), GetWorldPostProcessVolumes(*World).Contains(Cast<IInterface_PostProcessVolume>(Volume)));
			TestNotNull(TEXT("the CVar the capture path reads exists"), IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange")));

			APostProcessVolume* Later = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, SpawnParams);
			if (!TestNotNull(TEXT("spawned a second volume"), Later))
				return;
			ON_SCOPE_EXIT { if (IsValid(Later)) Later->Destroy(); };
			Later->bEnabled = true;
			Later->bUnbound = true;
			Later->BlendWeight = 1.0f;
			Later->Settings.bOverride_AutoExposureMaxBrightness = true;
			Later->Settings.AutoExposureMaxBrightness = 1.0f;
			TestFalse(TEXT("a later volume replaces the value"), HasOverexposingPostProcessVolume({ Volume, Later }, false));
			TestTrue(TEXT("the last volume to set it decides"), HasOverexposingPostProcessVolume({ Later, Volume }, false));

			Later->Settings.bOverride_AutoExposureMaxBrightness = false;
			Later->Settings.bOverride_AutoExposureMethod = true;
			Later->Settings.AutoExposureMethod = AEM_Manual;
			TestFalse(TEXT("Manual exposure ignores Max Brightness"), HasOverexposingPostProcessVolume({ Volume, Later }, false));
		});

		// Locks the wire shape the sidecar's ProxyResponseMapper depends on: SuccessWithImage must carry
		// the supplied base64 + mimeType in an image block, alongside (after, per the bridge server's
		// content-array ordering) the human-readable text block. The bridge server appends Images[] after
		// the text block in UnrealMcpBridgeServer.cpp; this asserts the producer side of that contract.
		It("SuccessWithImage carries the base64 + mimeType image block alongside the text block", [this]()
		{
			const FString Base64 = TEXT("aGVsbG8=");
			const FString Mime = TEXT("image/png");
			const FUnrealMcpToolResult Result = FUnrealMcpToolResult::SuccessWithImage(TEXT("captured"), Base64, Mime);

			TestTrue(TEXT("success"), Result.bSuccess);
			TestEqual(TEXT("text block preserved"), Result.Message, FString(TEXT("captured")));
			TestFalse(TEXT("an image result never carries structured content"), Result.Structured.IsValid());
			TestEqual(TEXT("exactly one image block"), Result.Images.Num(), 1);
			if (Result.Images.Num() == 1)
			{
				TestEqual(TEXT("base64 carried verbatim"), Result.Images[0].Base64Data, Base64);
				TestEqual(TEXT("mimeType carried verbatim"), Result.Images[0].MimeType, Mime);
			}
		});

		It("SuccessWithImage defaults the mimeType to image/png", [this]()
		{
			const FUnrealMcpToolResult Result = FUnrealMcpToolResult::SuccessWithImage(TEXT("captured"), TEXT("ZGF0YQ=="));
			TestEqual(TEXT("one image block"), Result.Images.Num(), 1);
			if (Result.Images.Num() == 1)
				TestEqual(TEXT("default mimeType"), Result.Images[0].MimeType, FString(TEXT("image/png")));
		});

		// The DEPRECATED structured form (see the header): every call form is still an image result with NO
		// structured content, the payload folded into the text block. One table, one set of assertions.
		It("deprecated SuccessWithImage(Message, Base64, Structured[, MimeType]) folds the payload into the text", [this]()
		{
			const TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
			Source->SetStringField(TEXT("source"), TEXT("Cam1"));
			const TSharedPtr<FJsonObject> Width = MakeShared<FJsonObject>();
			Width->SetNumberField(TEXT("width"), 64);
			const TSharedPtr<FJsonObject> Ok = MakeShared<FJsonObject>();
			Ok->SetBoolField(TEXT("ok"), true);
			const TSharedPtr<FJsonObject> NullStructured;

			struct FRow { const TCHAR* Label; FUnrealMcpToolResult Result; FString ExpectedMessage; FString ExpectedMime; };
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			const FRow Rows[] = {
				{ TEXT("4-arg"), FUnrealMcpToolResult::SuccessWithImage(TEXT("captured"), TEXT("aGVsbG8="), Source, TEXT("image/jpeg")), TEXT("captured\n{\"source\":\"Cam1\"}"), TEXT("image/jpeg") },
				{ TEXT("3-arg"), FUnrealMcpToolResult::SuccessWithImage(TEXT("captured"), TEXT("aGVsbG8="), Width), TEXT("captured\n{\"width\":64}"), TEXT("image/png") },
				{ TEXT("empty message"), FUnrealMcpToolResult::SuccessWithImage(FString(), TEXT("aGVsbG8="), Ok), TEXT("{\"ok\":true}"), TEXT("image/png") },
				{ TEXT("nullptr"), FUnrealMcpToolResult::SuccessWithImage(TEXT("captured"), TEXT("aGVsbG8="), nullptr), TEXT("captured"), TEXT("image/png") },
				{ TEXT("nullptr + mime"), FUnrealMcpToolResult::SuccessWithImage(TEXT("captured"), TEXT("aGVsbG8="), nullptr, TEXT("image/jpeg")), TEXT("captured"), TEXT("image/jpeg") },
				{ TEXT("null TSharedPtr"), FUnrealMcpToolResult::SuccessWithImage(TEXT("captured"), TEXT("aGVsbG8="), NullStructured), TEXT("captured"), TEXT("image/png") },
				{ TEXT("empty object"), FUnrealMcpToolResult::SuccessWithImage(TEXT("captured"), TEXT("aGVsbG8="), MakeShared<FJsonObject>()), TEXT("captured"), TEXT("image/png") },
			};
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			for (const FRow& Row : Rows)
			{
				const FUnrealMcpToolResult& Result = Row.Result;
				TestTrue(FString::Printf(TEXT("%s: success"), Row.Label), Result.bSuccess);
				TestFalse(FString::Printf(TEXT("%s: never structured content alongside an image"), Row.Label), Result.Structured.IsValid());
				TestEqual(FString::Printf(TEXT("%s: text block"), Row.Label), Result.Message, Row.ExpectedMessage);
				if (TestEqual(FString::Printf(TEXT("%s: exactly one image block"), Row.Label), Result.Images.Num(), 1))
				{
					TestEqual(FString::Printf(TEXT("%s: base64 carried verbatim"), Row.Label), Result.Images[0].Base64Data, FString(TEXT("aGVsbG8=")));
					TestEqual(FString::Printf(TEXT("%s: mimeType"), Row.Label), Result.Images[0].MimeType, Row.ExpectedMime);
				}
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
