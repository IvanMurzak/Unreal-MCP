// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntimeCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"        // UPackage complete type (GetTransientPackage()) — editor PCH supplied it transitively; the Game target needs it explicitly

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "TextureResource.h"

#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Camera/CameraComponent.h"

/**
 * The runtime screenshot subset (docs/ARCHITECTURE.md §10 "screenshot family" runtime subset, §12.7).
 * The two runtime-safe screenshot tools, moved DOWN into the Type=Runtime module so an LLM can inspect
 * what a running game (PIE or packaged Development) is rendering:
 *
 *  - `screenshot-game-view` — the live game viewport via GEngine->GameViewport->Viewport (the
 *    GameViewportClient exists in PIE and in a packaged game; in the editor coordinator the editor's PIE
 *    game viewport is the same FViewport). Replaces the editor family's GEditor->GetPIEViewport() so it
 *    works without GEditor.
 *  - `screenshot-camera` — render from a §3.2-resolved camera actor through a transient
 *    USceneCaptureComponent2D into a render target, then read back. SceneCapture2D + UWorld::SpawnActor
 *    are runtime-available; the world is the FUnrealMcpWorldProvider-resolved world.
 *
 * The editor-only screenshot-viewport (GEditor->GetActiveViewport) and screenshot-isolated stay in the
 * editor module's UnrealMcpScreenshotTools. Every handler runs ON the game thread (the §4 dispatcher).
 * Captures are dimension-capped (default 1024, hard cap 2048 per side; width/height clamped). Actual
 * pixel capture needs a GPU — headless `-nullrhi` cannot render, so each capture path validates its
 * arguments first (those branches are GPU-free and headless-spec-covered) and only then attempts the
 * GPU read-back, returning a structured "rendering unavailable" error under `-nullrhi`. The capture
 * paths themselves are LIVE-VERIFIED WINDOWED (issue #17 verification model). Screenshot APIs
 * (USceneCaptureComponent2D, UGameViewportClient, FViewport::ReadPixels) are stable 5.5→5.7 (§12.10).
 */
namespace UnrealMcpRuntimeScreenshotTools
{
	// ---- GPU-free dimension logic (exported in UnrealMcpRuntimeCoreTools.h for headless specs) -------

	int32 ResolveCaptureDimension(int64 Requested)
	{
		if (Requested <= 0)
			return DefaultCaptureDimension;
		return (int32)FMath::Clamp<int64>(Requested, (int64)1, (int64)MaxCaptureDimension);
	}

	void CapToMaxDimension(int32 InW, int32 InH, int32& OutW, int32& OutH)
	{
		OutW = FMath::Max(InW, 1);
		OutH = FMath::Max(InH, 1);
		const int32 LongestSide = FMath::Max(OutW, OutH);
		if (LongestSide > MaxCaptureDimension)
		{
			const double Scale = (double)MaxCaptureDimension / (double)LongestSide;
			OutW = FMath::Max(1, FMath::RoundToInt(OutW * Scale));
			OutH = FMath::Max(1, FMath::RoundToInt(OutH * Scale));
		}
	}

	// ---- Local helpers (family-unique names per the unity-build ODR rule) --------------------------

	namespace
	{
		// Keep the encoded payload well under the §1.2 64 MiB IPC line cap (base64 expands ~4/3).
		static constexpr int64 RuntimeScreenshotMaxEncodedBytes = 40 * 1024 * 1024;

		/** True when the engine can render; false under headless `-nullrhi`. */
		bool RuntimeScreenshotEnsureRenderingAvailable(FString& OutError)
		{
			if (!FApp::CanEverRender())
			{
				OutError = TEXT("Rendering is unavailable (headless/-nullrhi). Screenshot capture requires a "
				                "GPU-backed game/editor; run windowed and retry.");
				return false;
			}
			return true;
		}

		/** Force every pixel opaque so a viewport/render-target alpha of 0 does not yield a transparent PNG. */
		void RuntimeScreenshotForceOpaque(TArray<FColor>& Pixels)
		{
			for (FColor& Pixel : Pixels)
				Pixel.A = 255;
		}

		/** The structured block every screenshot tool returns alongside the image content. */
		TSharedPtr<FJsonObject> RuntimeScreenshotMakeStructured(const FString& Source, int32 Width, int32 Height, int32 EncodedBytes)
		{
			TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
			Structured->SetStringField(TEXT("source"), Source);
			Structured->SetNumberField(TEXT("width"), Width);
			Structured->SetNumberField(TEXT("height"), Height);
			Structured->SetStringField(TEXT("mimeType"), TEXT("image/png"));
			Structured->SetNumberField(TEXT("byteSize"), EncodedBytes);
			return Structured;
		}

		/** Resample a captured buffer to (DstW, DstH) when it differs from the source size. */
		void RuntimeScreenshotResizeIfNeeded(TArray<FColor>& Pixels, int32 SrcW, int32 SrcH, int32 DstW, int32 DstH)
		{
			if ((SrcW == DstW && SrcH == DstH) || DstW <= 0 || DstH <= 0)
				return;
			TArray<FColor> Resized;
			Resized.SetNumUninitialized(DstW * DstH);
			FImageUtils::ImageResize(SrcW, SrcH, Pixels, DstW, DstH, Resized, /*bResizeSRGBinLinearSpace*/ false);
			Pixels = MoveTemp(Resized);
		}

		/** Encode a BGRA8 FColor buffer to a base64 PNG (forcing opaque first). */
		bool RuntimeScreenshotEncodePngBase64(TArray<FColor>& Pixels, int32 Width, int32 Height,
			FString& OutBase64, int32& OutEncodedBytes, FString& OutError)
		{
			if (Width <= 0 || Height <= 0)
			{
				OutError = TEXT("Capture produced a zero-sized image.");
				return false;
			}
			if (Pixels.Num() < Width * Height)
			{
				OutError = FString::Printf(TEXT("Capture pixel buffer (%d) is smaller than %dx%d."), Pixels.Num(), Width, Height);
				return false;
			}
			RuntimeScreenshotForceOpaque(Pixels);

			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Width, Height,
				TArrayView64<const FColor>(Pixels.GetData(), (int64)Width * (int64)Height), Png);
			if (Png.Num() == 0)
			{
				OutError = TEXT("PNG encoding produced no bytes.");
				return false;
			}
			if (Png.Num() > RuntimeScreenshotMaxEncodedBytes)
			{
				OutError = FString::Printf(
					TEXT("Encoded PNG (%lld bytes) exceeds the %lld-byte cap; request a smaller width/height."),
					(int64)Png.Num(), RuntimeScreenshotMaxEncodedBytes);
				return false;
			}
			OutBase64 = FBase64::Encode(Png.GetData(), Png.Num());
			OutEncodedBytes = (int32)Png.Num();
			return true;
		}

		/**
		 * Effective output size for a viewport-style capture. When BOTH width and height are supplied they
		 * are each clamped to [1, 2048]; when only ONE is supplied the other is derived from the native
		 * aspect ratio; when NEITHER is supplied the native size is used. The result is then hard-capped.
		 */
		void RuntimeScreenshotEffectiveViewportSize(const FUnrealMcpToolCall& Call, int32 NativeW, int32 NativeH, int32& OutW, int32& OutH)
		{
			const int64 ReqW = Call.GetInt(TEXT("width"), 0);
			const int64 ReqH = Call.GetInt(TEXT("height"), 0);
			int32 W, H;
			if (ReqW > 0 && ReqH > 0)
			{
				W = ResolveCaptureDimension(ReqW);
				H = ResolveCaptureDimension(ReqH);
			}
			else if (ReqW > 0)
			{
				W = ResolveCaptureDimension(ReqW);
				H = NativeW > 0 ? FMath::Max(1, FMath::RoundToInt(W * (double)NativeH / (double)NativeW)) : NativeH;
			}
			else if (ReqH > 0)
			{
				H = ResolveCaptureDimension(ReqH);
				W = NativeH > 0 ? FMath::Max(1, FMath::RoundToInt(H * (double)NativeW / (double)NativeH)) : NativeW;
			}
			else
			{
				W = NativeW;
				H = NativeH;
			}
			CapToMaxDimension(W, H, OutW, OutH);
		}

		/** Read + encode an FViewport (the live game viewport). Called only on the GPU-available path. */
		FUnrealMcpToolResult RuntimeScreenshotCaptureFromViewport(const FUnrealMcpToolCall& Call, FViewport* Viewport, const FString& Source)
		{
			const FIntPoint Size = Viewport->GetSizeXY();
			if (Size.X <= 0 || Size.Y <= 0)
				return FUnrealMcpToolResult::Error(FString::Printf(TEXT("The %s has a zero-sized render area."), *Source));

			Viewport->Draw(); // ensure a current frame is present before the read-back

			TArray<FColor> Pixels;
			if (!Viewport->ReadPixels(Pixels, FReadSurfaceDataFlags(), FIntRect(0, 0, Size.X, Size.Y)))
				return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to read pixels from the %s."), *Source));

			int32 OutW = 0, OutH = 0;
			RuntimeScreenshotEffectiveViewportSize(Call, Size.X, Size.Y, OutW, OutH);
			RuntimeScreenshotResizeIfNeeded(Pixels, Size.X, Size.Y, OutW, OutH);

			FString Base64; int32 Bytes = 0; FString Error;
			if (!RuntimeScreenshotEncodePngBase64(Pixels, OutW, OutH, Base64, Bytes, Error))
				return FUnrealMcpToolResult::Error(Error);

			const FString Message = FString::Printf(TEXT("Captured %s (%dx%d PNG, %d bytes)."), *Source, OutW, OutH, Bytes);
			UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] %s"), *Message);
			return FUnrealMcpToolResult::SuccessWithImage(Message, Base64, RuntimeScreenshotMakeStructured(Source, OutW, OutH, Bytes));
		}

		/**
		 * Render through a transient SceneCapture2D into a transient render target, then read back. The
		 * capture actor is spawned RF_Transient + hidden from the outliner and destroyed on every path, so
		 * the world is never dirtied. Called only on the GPU-available path. screenshot-camera passes no
		 * background and keeps the cheaper tonemapped LDR path.
		 */
		FUnrealMcpToolResult RuntimeScreenshotCaptureWithSceneCapture(
			UWorld* World, const FTransform& CaptureXform, float FovDeg, int32 Width, int32 Height, const FString& Source)
		{
			// NewObject's single-arg overload takes a UObject* Outer; GetTransientPackage() returns UPackage*.
			// In the editor target the shared PCH made the conversion implicit, but the Game target compiles this
			// module standalone, so spell the Outer as a UObject* explicitly (UPackage is a UObject).
			UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(static_cast<UObject*>(GetTransientPackage()));
			RenderTarget->RenderTargetFormat = RTF_RGBA8;
			RenderTarget->ClearColor = FLinearColor::Black;
			RenderTarget->bAutoGenerateMips = false;
			RenderTarget->InitAutoFormat(Width, Height);
			RenderTarget->UpdateResourceImmediate(true);
			FGCObjectScopeGuard RenderTargetGuard(RenderTarget);

			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
#if WITH_EDITOR
			// bHideFromSceneOutliner is an editor-only FActorSpawnParameters member (the outliner does not exist
			// in a packaged game); guard it so the Game target compiles. RF_Transient above already keeps the
			// throwaway capture actor out of any save in both configs.
			SpawnParams.bHideFromSceneOutliner = true;
#endif
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(
				ASceneCapture2D::StaticClass(), CaptureXform, SpawnParams);
			if (!CaptureActor)
				return FUnrealMcpToolResult::Error(TEXT("Failed to spawn a transient SceneCapture2D actor."));
			ON_SCOPE_EXIT { if (IsValid(CaptureActor)) CaptureActor->Destroy(); };

			USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
			if (!Capture)
				return FUnrealMcpToolResult::Error(TEXT("Transient SceneCapture2D had no capture component."));

			Capture->TextureTarget = RenderTarget;
			Capture->FOVAngle = FovDeg;
			Capture->CaptureSource = SCS_FinalColorLDR;
			Capture->bCaptureEveryFrame = false;
			Capture->bCaptureOnMovement = false;
			// Pin auto-exposure. A single-shot CaptureScene() never gives eye-adaptation a chance to
			// converge, so the default auto-exposure leaves one-off captures badly under/over-exposed.
			// Locking min == max brightness makes exposure a fixed factor (no adaptation transient), so the
			// capture is deterministic regardless of the scene's prior adaptation state.
			Capture->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
			Capture->PostProcessSettings.AutoExposureMinBrightness = 1.0f;
			Capture->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
			Capture->PostProcessSettings.AutoExposureMaxBrightness = 1.0f;
			// The capture component is the spawned actor's root (the actor was spawned at CaptureXform), so
			// its world transform already equals CaptureXform — no explicit SetWorldLocationAndRotation needed.
			Capture->CaptureScene();

			FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
			if (!Resource)
				return FUnrealMcpToolResult::Error(TEXT("Render target resource was not available after capture."));

			TArray<FColor> Pixels;
			FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
			ReadFlags.SetLinearToGamma(false);
			if (!Resource->ReadPixels(Pixels, ReadFlags))
				return FUnrealMcpToolResult::Error(TEXT("Failed to read pixels from the capture render target."));

			FString Base64; int32 Bytes = 0; FString Error;
			if (!RuntimeScreenshotEncodePngBase64(Pixels, Width, Height, Base64, Bytes, Error))
				return FUnrealMcpToolResult::Error(Error);

			const FString Message = FString::Printf(TEXT("Captured %s (%dx%d PNG, %d bytes)."), *Source, Width, Height, Bytes);
			UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] %s"), *Message);
			return FUnrealMcpToolResult::SuccessWithImage(Message, Base64, RuntimeScreenshotMakeStructured(Source, Width, Height, Bytes));
		}

		/** Shared description tail documenting the size caps (so each tool advertises them, §10). */
		const TCHAR* RuntimeScreenshotSizeCapNote()
		{
			return TEXT(" Returns a base64 PNG as MCP image content. Optional 'width'/'height' are clamped to "
			            "[1, 2048] per side; the default is 1024. Requires a GPU-backed (windowed) game/editor.");
		}

		/** The live game viewport (PIE or packaged game), or null when no game viewport client is up. */
		FViewport* RuntimeScreenshotGameViewport()
		{
			if (GEngine && GEngine->GameViewport)
				return GEngine->GameViewport->Viewport;
			return nullptr;
		}
	}

	// ---- Registration -----------------------------------------------------------------------------

	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		// screenshot-game-view — the live game viewport (PIE or packaged game).
		Registry.Tool(TEXT("screenshot-game-view"))
			.Title(TEXT("Screenshot Game View"))
			.Description(FString(TEXT("Capture the live game viewport — the Play-In-Editor game view in the editor, "
				"or the game window over a runtime connection. Errors when no game viewport is active (e.g. no PIE "
				"session and not a running game).")) + RuntimeScreenshotSizeCapNote())
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); when only 'height' is given the width is derived from the native aspect ratio, and the native game-view width is used when both are omitted."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); when only 'width' is given the height is derived from the native aspect ratio, and the native game-view height is used when both are omitted."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Validate the game-viewport precondition FIRST (GPU-free, headless-spec-covered).
				FViewport* GameViewport = RuntimeScreenshotGameViewport();
				if (!GameViewport)
					return FUnrealMcpToolResult::Error(TEXT("No game viewport is active. Start PIE (in the editor) or run the game before calling screenshot-game-view."));

				FString Error;
				if (!RuntimeScreenshotEnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				return RuntimeScreenshotCaptureFromViewport(Call, GameViewport, TEXT("game view"));
			});

		// screenshot-camera — render from a resolved camera actor via SceneCapture2D.
		Registry.Tool(TEXT("screenshot-camera"))
			.Title(TEXT("Screenshot Camera"))
			.Description(FString(TEXT("Render the scene from a chosen camera actor (resolved by label/name/path) and capture it.")) + RuntimeScreenshotSizeCapNote())
			.ParamString(TEXT("camera"), TEXT("Camera actor reference (label, object name, or path) to render from."), EUnrealMcpParamRequirement::Required)
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); default 1024."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); default 1024."))
			.ParamNumber(TEXT("fov"), TEXT("Optional horizontal field-of-view override in degrees (clamped to [5, 170]); defaults to the camera component's FOV (or 90)."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Validate arguments + resolve the camera FIRST (GPU-free, headless-spec-covered).
				if (!Call.Has(TEXT("camera")))
					return FUnrealMcpToolResult::Error(TEXT("'camera' is required."));

				UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No active world is available."));

				const FString CameraRef = Call.GetString(TEXT("camera"));
				AActor* CameraActor = FUnrealMcpObjectRef::ResolveActor(CameraRef, World);
				if (!CameraActor)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No actor matched camera reference '%s'."), *CameraRef));

				UCameraComponent* CameraComponent = CameraActor->FindComponentByClass<UCameraComponent>();
				// Clamp once at parse time to a sane perspective range; a degenerate FOV (<= 0 or >= 180)
				// yields a NaN/garbage projection matrix rather than a usable render.
				const float Fov = FMath::Clamp(Call.Has(TEXT("fov"))
					? (float)Call.GetNumber(TEXT("fov"))
					: (CameraComponent ? CameraComponent->FieldOfView : 90.0f), 5.0f, 170.0f);
				const FTransform CaptureXform = CameraComponent
					? CameraComponent->GetComponentTransform()
					: CameraActor->GetActorTransform();

				FString Error;
				if (!RuntimeScreenshotEnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				const int32 Width = ResolveCaptureDimension(Call.GetInt(TEXT("width"), 0));
				const int32 Height = ResolveCaptureDimension(Call.GetInt(TEXT("height"), 0));
				const FString Source = FString::Printf(TEXT("camera '%s'"), *CameraActor->GetActorNameOrLabel());
				return RuntimeScreenshotCaptureWithSceneCapture(World, CaptureXform, Fov, Width, Height, Source);
			});
	}
}
