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

		/** Effective output size for a viewport-style capture: explicit width/height (clamped) else native, then hard cap. */
		void EffectiveViewportSize(const FUnrealMcpToolCall& Call, int32 NativeW, int32 NativeH, int32& OutW, int32& OutH)
		{
			const int64 ReqW = Call.GetInt(TEXT("width"), 0);
			const int64 ReqH = Call.GetInt(TEXT("height"), 0);
			const int32 W = ReqW > 0 ? ResolveCaptureDimension(ReqW) : NativeW;
			const int32 H = ReqH > 0 ? ResolveCaptureDimension(ReqH) : NativeH;
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
			UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
			RenderTarget->RenderTargetFormat = RTF_RGBA8;
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
			Capture->CaptureSource = SCS_FinalColorLDR;
			Capture->bCaptureEveryFrame = false;
			Capture->bCaptureOnMovement = false;
			if (ShowOnlyActor)
			{
				Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
				Capture->ShowOnlyActors.Add(ShowOnlyActor);
			}
			Capture->SetWorldLocationAndRotation(CaptureXform.GetLocation(), CaptureXform.GetRotation());
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
			if (!EncodePngBase64(Pixels, Width, Height, Base64, Bytes, Error))
				return FUnrealMcpToolResult::Error(Error);

			const FString Message = FString::Printf(TEXT("Captured %s (%dx%d PNG, %d bytes)."), *Source, Width, Height, Bytes);
			UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] %s"), *Message);
			return FUnrealMcpToolResult::SuccessWithImage(Message, Base64, MakeStructured(Source, Width, Height, Bytes));
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
			.Description(FString(TEXT("Capture the active editor viewport.")) + SizeCapNote())
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); native viewport width when omitted."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); native viewport height when omitted."))
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
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); native game-view width when omitted."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); native game-view height when omitted."))
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
			.ParamNumber(TEXT("fov"), TEXT("Optional horizontal field-of-view override in degrees; defaults to the camera component's FOV (or 90)."))
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
				const float Fov = Call.Has(TEXT("fov"))
					? (float)Call.GetNumber(TEXT("fov"))
					: (CameraComponent ? CameraComponent->FieldOfView : 90.0f);
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
			.ParamString(TEXT("background"), TEXT("Optional background color as hex ('#RRGGBB' or '#RRGGBBAA'); defaults to dark grey."))
			.ParamNumber(TEXT("fov"), TEXT("Optional field-of-view override in degrees; default 50."))
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

				FString Error;
				if (!EnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				// Auto-frame: place the capture along an isometric-ish offset so the actor's bounding sphere fits the FOV.
				const float Fov = Call.Has(TEXT("fov")) ? (float)Call.GetNumber(TEXT("fov")) : 50.0f;
				FBox Bounds = TargetActor->GetComponentsBoundingBox(/*bNonColliding*/ true);
				FVector Center = Bounds.IsValid ? Bounds.GetCenter() : TargetActor->GetActorLocation();
				const float Radius = Bounds.IsValid ? FMath::Max(Bounds.GetExtent().Size(), 1.0f) : 100.0f;
				const float HalfFovRad = FMath::DegreesToRadians(FMath::Clamp(Fov, 5.0f, 170.0f) * 0.5f);
				const float Distance = (Radius / FMath::Max(FMath::Tan(HalfFovRad), KINDA_SMALL_NUMBER)) * 1.5f;
				const FVector Offset = FVector(-1.0f, -1.0f, 0.6f).GetSafeNormal() * Distance;
				const FVector CamLocation = Center + Offset;
				const FRotator CamRotation = (Center - CamLocation).Rotation();
				const FTransform CaptureXform(CamRotation, CamLocation);

				FLinearColor Background(0.05f, 0.05f, 0.05f, 1.0f);
				if (Call.Has(TEXT("background")))
					Background = FLinearColor(FColor::FromHex(Call.GetString(TEXT("background"))));

				const int32 Width = ResolveCaptureDimension(Call.GetInt(TEXT("width"), 0));
				const int32 Height = ResolveCaptureDimension(Call.GetInt(TEXT("height"), 0));
				const FString Source = FString::Printf(TEXT("isolated actor '%s'"), *TargetActor->GetActorNameOrLabel());
				return CaptureWithSceneCapture(World, CaptureXform, Fov, Width, Height, Source, &Background, TargetActor);
			});
	}
}
