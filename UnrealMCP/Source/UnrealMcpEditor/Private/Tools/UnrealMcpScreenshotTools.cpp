// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/GCObjectScopeGuard.h"

#include "Editor.h"
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
 * The screenshot / viewport-capture tool family (docs/ARCHITECTURE.md §10 "screenshot family", issue
 * #17). Four kebab-case CORE tools that return a base64 PNG as MCP image content:
 *
 *  - `screenshot-viewport`   — active editor viewport via FViewport::ReadPixels.
 *  - `screenshot-game-view`  — PIE/game view via the PIE FViewport; structured error when no PIE session.
 *  - `screenshot-camera`     — render from a §3.2-resolved camera actor through a transient
 *                              USceneCaptureComponent2D into a render target, then read back.
 *  - `screenshot-isolated`   — isolated actor render: transient SceneCapture2D + show-only list +
 *                              neutral background (the Godot SubViewport pattern mapped to UE).
 *
 * Every handler runs ON the game thread (the dispatcher guarantees it, §4). Captures are dimension-
 * capped (default 1024, hard cap 2048 per side; width/height clamped). Actual pixel capture needs a
 * GPU — headless `-nullrhi` cannot render, so each capture path validates its arguments first (those
 * branches are GPU-free and headless-spec-covered) and only then attempts the GPU read-back, returning
 * a structured "rendering unavailable" error under `-nullrhi` instead of crashing. The capture paths
 * themselves are LIVE-VERIFIED WINDOWED (issue #17 verification model).
 */
namespace UnrealMcpScreenshotTools
{
	// ---- GPU-free dimension logic (exported in UnrealMcpCoreTools.h for headless specs) -------------

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

	// ---- Local helpers ----------------------------------------------------------------------------

	namespace
	{
		// Keep the encoded payload well under the §1.2 64 MiB IPC line cap (base64 expands ~4/3).
		static constexpr int64 MaxEncodedBytes = 40 * 1024 * 1024;

		/** True when the editor can render; false under headless `-nullrhi`. */
		bool EnsureRenderingAvailable(FString& OutError)
		{
			if (!FApp::CanEverRender())
			{
				OutError = TEXT("Rendering is unavailable (headless/-nullrhi). Screenshot capture requires a "
				                "GPU-backed editor; run the editor windowed and retry.");
				return false;
			}
			return true;
		}

		/** Force every pixel opaque so a viewport/render-target alpha of 0 does not yield a transparent PNG. */
		void ForceOpaque(TArray<FColor>& Pixels)
		{
			for (FColor& Pixel : Pixels)
				Pixel.A = 255;
		}

		/** The structured block every screenshot tool returns alongside the image content. */
		TSharedPtr<FJsonObject> MakeStructured(const FString& Source, int32 Width, int32 Height, int32 EncodedBytes)
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
		void ResizeIfNeeded(TArray<FColor>& Pixels, int32 SrcW, int32 SrcH, int32 DstW, int32 DstH)
		{
			if ((SrcW == DstW && SrcH == DstH) || DstW <= 0 || DstH <= 0)
				return;
			TArray<FColor> Resized;
			Resized.SetNumUninitialized(DstW * DstH);
			FImageUtils::ImageResize(SrcW, SrcH, Pixels, DstW, DstH, Resized, /*bResizeSRGBinLinearSpace*/ false);
			Pixels = MoveTemp(Resized);
		}

		/** Encode a BGRA8 FColor buffer to a base64 PNG (forcing opaque first). */
		bool EncodePngBase64(TArray<FColor>& Pixels, int32 Width, int32 Height,
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
			ForceOpaque(Pixels);

			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Width, Height,
				TArrayView64<const FColor>(Pixels.GetData(), (int64)Width * (int64)Height), Png);
			if (Png.Num() == 0)
			{
				OutError = TEXT("PNG encoding produced no bytes.");
				return false;
			}
			if (Png.Num() > MaxEncodedBytes)
			{
				OutError = FString::Printf(
					TEXT("Encoded PNG (%lld bytes) exceeds the %lld-byte cap; request a smaller width/height."),
					(int64)Png.Num(), MaxEncodedBytes);
				return false;
			}
			OutBase64 = FBase64::Encode(Png.GetData(), Png.Num());
			OutEncodedBytes = (int32)Png.Num();
			return true;
		}

		/**
		 * Effective output size for a viewport-style capture. When BOTH width and height are supplied they
		 * are each clamped to [1, 2048]; when only ONE is supplied the other is derived from the native
		 * aspect ratio (so a lone width=512 on a 1920x1080 viewport yields 512x288, not a stretched
		 * 512x1080); when NEITHER is supplied the native size is used. The result is then hard-capped.
		 */
		void EffectiveViewportSize(const FUnrealMcpToolCall& Call, int32 NativeW, int32 NativeH, int32& OutW, int32& OutH)
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

		/** Read + encode an FViewport (editor or PIE). Called only on the GPU-available path. */
		FUnrealMcpToolResult CaptureFromViewport(const FUnrealMcpToolCall& Call, FViewport* Viewport, const FString& Source)
		{
			const FIntPoint Size = Viewport->GetSizeXY();
			if (Size.X <= 0 || Size.Y <= 0)
				return FUnrealMcpToolResult::Error(FString::Printf(TEXT("The %s has a zero-sized render area."), *Source));

			Viewport->Draw(); // ensure a current frame is present before the read-back

			TArray<FColor> Pixels;
			if (!Viewport->ReadPixels(Pixels, FReadSurfaceDataFlags(), FIntRect(0, 0, Size.X, Size.Y)))
				return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to read pixels from the %s."), *Source));

			int32 OutW = 0, OutH = 0;
			EffectiveViewportSize(Call, Size.X, Size.Y, OutW, OutH);
			ResizeIfNeeded(Pixels, Size.X, Size.Y, OutW, OutH);

			FString Base64; int32 Bytes = 0; FString Error;
			if (!EncodePngBase64(Pixels, OutW, OutH, Base64, Bytes, Error))
				return FUnrealMcpToolResult::Error(Error);

			const FString Message = FString::Printf(TEXT("Captured %s (%dx%d PNG, %d bytes)."), *Source, OutW, OutH, Bytes);
			UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] %s"), *Message);
			return FUnrealMcpToolResult::SuccessWithImage(Message, Base64, MakeStructured(Source, OutW, OutH, Bytes));
		}

		/**
		 * Render through a transient SceneCapture2D into a transient render target, then read back. The
		 * capture actor is spawned RF_Transient + hidden from the outliner and destroyed on every path, so
		 * the editor world is never dirtied. Called only on the GPU-available path.
		 */
		FUnrealMcpToolResult CaptureWithSceneCapture(
			UWorld* World, const FTransform& CaptureXform, float FovDeg, int32 Width, int32 Height,
			const FString& Source, const FLinearColor* BackgroundColor, AActor* ShowOnlyActor)
		{
			// Isolated mode (a background was requested) composites the actor over a solid color. A plain
			// SCS_FinalColorLDR capture writes opaque pixels everywhere, overwriting the render target's
			// ClearColor — so an empty region renders as the scene's (black) background, NOT the requested
			// color (verified windowed: a #FF0000 background came back pure black). To honor the background
			// we capture coverage-carrying HDR scene color (SCS_SceneColorHDR, inverse opacity in alpha)
			// into a float target and composite scene-over-background in the read-back. screenshot-camera
			// passes no background and keeps the cheaper tonemapped LDR path.
			const bool bComposite = (BackgroundColor != nullptr);

			UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
			RenderTarget->RenderTargetFormat = bComposite ? RTF_RGBA16f : RTF_RGBA8;
			RenderTarget->ClearColor = BackgroundColor ? *BackgroundColor : FLinearColor::Black;
			RenderTarget->bAutoGenerateMips = false;
			RenderTarget->InitAutoFormat(Width, Height);
			RenderTarget->UpdateResourceImmediate(true);
			FGCObjectScopeGuard RenderTargetGuard(RenderTarget);

			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
			SpawnParams.bTemporaryEditorActor = true;
			SpawnParams.bHideFromSceneOutliner = true;
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
			Capture->CaptureSource = bComposite ? SCS_SceneColorHDR : SCS_FinalColorLDR;
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
			if (ShowOnlyActor)
			{
				Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
				Capture->ShowOnlyActors.Add(ShowOnlyActor);
				// Include attached child actors (child-actor components, attach hierarchies — common for
				// composed Blueprints) so visually-integral parts are not silently omitted from the render.
				TArray<AActor*> AttachedActors;
				ShowOnlyActor->GetAttachedActors(AttachedActors, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ true);
				Capture->ShowOnlyActors.Append(AttachedActors);
			}
			// The capture component is the spawned actor's root (the actor was spawned at CaptureXform), so
			// its world transform already equals CaptureXform — no explicit SetWorldLocationAndRotation needed.
			Capture->CaptureScene();

			FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
			if (!Resource)
				return FUnrealMcpToolResult::Error(TEXT("Render target resource was not available after capture."));

			TArray<FColor> Pixels;
			if (bComposite)
			{
				// SCS_SceneColorHDR stores inverse opacity in alpha (1 = empty/background visible, 0 = opaque
				// geometry), so compositing scene-over-background recovers the requested solid background:
				//   final = sceneColor + background * alpha   (premultiplied "over", done in linear space).
				TArray<FLinearColor> LinearPixels;
				if (!Resource->ReadLinearColorPixels(LinearPixels))
					return FUnrealMcpToolResult::Error(TEXT("Failed to read pixels from the capture render target."));
				const FLinearColor Bg = *BackgroundColor;
				Pixels.SetNumUninitialized(LinearPixels.Num());
				for (int32 Index = 0; Index < LinearPixels.Num(); ++Index)
				{
					const FLinearColor& Scene = LinearPixels[Index];
					const FLinearColor Composited(
						Scene.R + Bg.R * Scene.A,
						Scene.G + Bg.G * Scene.A,
						Scene.B + Bg.B * Scene.A,
						1.0f);
					Pixels[Index] = Composited.ToFColor(/*bSRGB*/ true);
				}
			}
			else
			{
				FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
				ReadFlags.SetLinearToGamma(false);
				if (!Resource->ReadPixels(Pixels, ReadFlags))
					return FUnrealMcpToolResult::Error(TEXT("Failed to read pixels from the capture render target."));
			}

			FString Base64; int32 Bytes = 0; FString Error;
			if (!EncodePngBase64(Pixels, Width, Height, Base64, Bytes, Error))
				return FUnrealMcpToolResult::Error(Error);

			const FString Message = FString::Printf(TEXT("Captured %s (%dx%d PNG, %d bytes)."), *Source, Width, Height, Bytes);
			UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] %s"), *Message);
			return FUnrealMcpToolResult::SuccessWithImage(Message, Base64, MakeStructured(Source, Width, Height, Bytes));
		}

		/**
		 * Parse a '#RRGGBB' / '#RRGGBBAA' (bare 6- or 8-digit also accepted) hex color. FColor::FromHex
		 * silently returns black for malformed input, so validate explicitly and surface a structured error
		 * instead — this also gives the headless specs another GPU-free branch. Only the two lengths the
		 * tool advertises are accepted (no 3-digit shorthand) so the contract matches the documentation.
		 */
		bool ParseHexColor(const FString& In, FLinearColor& OutColor, FString& OutError)
		{
			FString Hex = In;
			Hex.RemoveFromStart(TEXT("#"));
			if (Hex.Len() != 6 && Hex.Len() != 8)
			{
				OutError = FString::Printf(
					TEXT("Invalid 'background' hex color '%s'; expected '#RRGGBB' or '#RRGGBBAA'."), *In);
				return false;
			}
			for (const TCHAR Ch : Hex)
			{
				if (!FChar::IsHexDigit(Ch))
				{
					OutError = FString::Printf(
						TEXT("Invalid 'background' hex color '%s'; non-hex character found."), *In);
					return false;
				}
			}
			OutColor = FLinearColor(FColor::FromHex(Hex));
			return true;
		}

		/** Shared description tail documenting the size caps (so each tool advertises them, §10). */
		const TCHAR* SizeCapNote()
		{
			return TEXT(" Returns a base64 PNG as MCP image content. Optional 'width'/'height' are clamped to "
			            "[1, 2048] per side; the default is 1024. Requires a GPU-backed (windowed) editor.");
		}
	}

	// ---- Registration -----------------------------------------------------------------------------

	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		// screenshot-viewport — active editor viewport.
		Registry.Tool(TEXT("screenshot-viewport"))
			.Title(TEXT("Screenshot Viewport"))
			.Description(FString(TEXT("Capture the active editor viewport. Note: while a Play-In-Editor session "
				"has viewport focus the 'active viewport' is the PIE game view; use screenshot-game-view to "
				"capture the game view explicitly.")) + SizeCapNote())
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); when only 'height' is given the width is derived from the native aspect ratio, and the native viewport width is used when both are omitted."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); when only 'width' is given the height is derived from the native aspect ratio, and the native viewport height is used when both are omitted."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FString Error;
				if (!EnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				FViewport* Viewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
				if (!Viewport)
					return FUnrealMcpToolResult::Error(TEXT("No active editor viewport. Focus a level editor viewport and retry."));

				return CaptureFromViewport(Call, Viewport, TEXT("editor viewport"));
			});

		// screenshot-game-view — PIE/game view; structured error when no PIE session is active.
		Registry.Tool(TEXT("screenshot-game-view"))
			.Title(TEXT("Screenshot Game View"))
			.Description(FString(TEXT("Capture the Play-In-Editor game view. Errors when no PIE session is active.")) + SizeCapNote())
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); when only 'height' is given the width is derived from the native aspect ratio, and the native game-view width is used when both are omitted."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); when only 'width' is given the height is derived from the native aspect ratio, and the native game-view height is used when both are omitted."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Validate the PIE precondition FIRST (GPU-free, headless-spec-covered).
				const bool bPlaySessionActive = GEditor && GEditor->PlayWorld != nullptr;
				FViewport* PieViewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
				if (!bPlaySessionActive || !PieViewport)
					return FUnrealMcpToolResult::Error(TEXT("No Play-In-Editor session is active. Start PIE before calling screenshot-game-view."));

				FString Error;
				if (!EnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				return CaptureFromViewport(Call, PieViewport, TEXT("PIE game view"));
			});

		// screenshot-camera — render from a resolved camera actor via SceneCapture2D.
		Registry.Tool(TEXT("screenshot-camera"))
			.Title(TEXT("Screenshot Camera"))
			.Description(FString(TEXT("Render the scene from a chosen camera actor (resolved by label/name/path) and capture it.")) + SizeCapNote())
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
					return FUnrealMcpToolResult::Error(TEXT("No editor world is available."));

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
				if (!EnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				const int32 Width = ResolveCaptureDimension(Call.GetInt(TEXT("width"), 0));
				const int32 Height = ResolveCaptureDimension(Call.GetInt(TEXT("height"), 0));
				const FString Source = FString::Printf(TEXT("camera '%s'"), *CameraActor->GetActorNameOrLabel());
				return CaptureWithSceneCapture(World, CaptureXform, Fov, Width, Height, Source, nullptr, nullptr);
			});

		// screenshot-isolated — isolated actor render (transient SceneCapture2D + show-only list + background).
		Registry.Tool(TEXT("screenshot-isolated"))
			.Title(TEXT("Screenshot Isolated Actor"))
			.Description(FString(TEXT("Render a single actor in isolation against a neutral background, auto-framed by its bounds.")) + SizeCapNote())
			.ParamString(TEXT("actor"), TEXT("Target actor reference (label, object name, or path) to render in isolation."), EUnrealMcpParamRequirement::Required)
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); default 1024."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); default 1024."))
			.ParamString(TEXT("background"), TEXT("Optional background color as hex ('#RRGGBB' or '#RRGGBBAA'); the alpha of '#RRGGBBAA' is accepted but ignored (the PNG is always opaque). Defaults to dark grey."))
			.ParamNumber(TEXT("fov"), TEXT("Optional field-of-view override in degrees (clamped to [5, 170]); default 50."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Validate arguments + resolve the actor FIRST (GPU-free, headless-spec-covered).
				if (!Call.Has(TEXT("actor")))
					return FUnrealMcpToolResult::Error(TEXT("'actor' is required."));

				UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No editor world is available."));

				const FString ActorRef = Call.GetString(TEXT("actor"));
				AActor* TargetActor = FUnrealMcpObjectRef::ResolveActor(ActorRef, World);
				if (!TargetActor)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No actor matched reference '%s'."), *ActorRef));

				// Parse + validate the background BEFORE the GPU guard so malformed hex is a GPU-free error branch.
				FLinearColor Background(0.05f, 0.05f, 0.05f, 1.0f);
				FString Error;
				if (Call.Has(TEXT("background")) && !ParseHexColor(Call.GetString(TEXT("background")), Background, Error))
					return FUnrealMcpToolResult::Error(Error);

				if (!EnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				// Auto-frame: place the capture along an isometric-ish offset so the actor's bounding sphere fits the FOV.
				// Clamp once here so the same value drives both the auto-framing distance below and the
				// capture's FOVAngle (an unclamped FOV would frame and render at different angles).
				const float Fov = FMath::Clamp(Call.Has(TEXT("fov")) ? (float)Call.GetNumber(TEXT("fov")) : 50.0f, 5.0f, 170.0f);
				const int32 Width = ResolveCaptureDimension(Call.GetInt(TEXT("width"), 0));
				const int32 Height = ResolveCaptureDimension(Call.GetInt(TEXT("height"), 0));
				FBox Bounds = TargetActor->GetComponentsBoundingBox(/*bNonColliding*/ true);
				FVector Center = Bounds.IsValid ? Bounds.GetCenter() : TargetActor->GetActorLocation();
				const float Radius = Bounds.IsValid ? FMath::Max(Bounds.GetExtent().Size(), 1.0f) : 100.0f;
				const float HalfFovRad = FMath::DegreesToRadians(Fov * 0.5f);
				// FOVAngle is the HORIZONTAL field of view, so for a wider-than-tall output the vertical FOV
				// shrinks by Height/Width (UE's scene-capture projection zooms the minor axis in). Pull the
				// camera back by Max(1, Width/Height) so a wide aspect frames the bounding sphere against the
				// (smaller) vertical FOV instead of clipping the actor off the top and bottom.
				const float AspectPullback = Height > 0 ? FMath::Max(1.0f, (float)Width / (float)Height) : 1.0f;
				const float Distance = (Radius / FMath::Max(FMath::Tan(HalfFovRad), KINDA_SMALL_NUMBER)) * 1.5f * AspectPullback;
				const FVector Offset = FVector(-1.0f, -1.0f, 0.6f).GetSafeNormal() * Distance;
				const FVector CamLocation = Center + Offset;
				const FRotator CamRotation = (Center - CamLocation).Rotation();
				const FTransform CaptureXform(CamRotation, CamLocation);

				const FString Source = FString::Printf(TEXT("isolated actor '%s'"), *TargetActor->GetActorNameOrLabel());
				return CaptureWithSceneCapture(World, CaptureXform, Fov, Width, Height, Source, &Background, TargetActor);
			});
	}
}
