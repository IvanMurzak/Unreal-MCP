// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"
#include "Tools/UnrealMcpObjectRef.h"
#include "Tools/UnrealMcpSourceTools.h"

#include "Algo/MaxElement.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/Interface_PostProcessVolume.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "RHIGlobals.h"
#include "UObject/GCObjectScopeGuard.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "UnrealClient.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SWindow.h"
#include "ImageUtils.h"
#include "TextureResource.h"

#include "GameFramework/Actor.h"
#include "Engine/Scene.h"
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
 * rendering editor: windowed, or headless with `-RenderOffscreen` (what unreal-mcp-cli starts). Under `-nullrhi`
 * nothing renders at all, so each capture path validates its arguments first (those branches are GPU-free and
 * headless-spec-covered) and only then attempts the GPU read-back, returning a structured error that names
 * `-nullrhi` and the fix (NullRhiScreenshotError) instead of crashing or returning a black image. The capture
 * paths themselves are LIVE-VERIFIED with a rendering editor (issue #17 verification model).
 *
 * Every capture renders a new frame at call time and states its freshness (docs/ARCHITECTURE.md §10 "Background
 * rendering", FormatFreshness).
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

	FUnrealMcpToolResult MakeImageResult(const FString& Source, int32 Width, int32 Height, const FString& Base64Png, int32 EncodedBytes,
		const FString& Freshness, const FString& SavedPath)
	{
		// The metadata travels in the text block: SuccessWithImage carries no structured content (see there).
		FString Message = FString::Printf(TEXT("Captured %s (%dx%d, image/png, %d bytes)."),
			*Source, Width, Height, EncodedBytes);
		if (!SavedPath.IsEmpty())
			Message += FString::Printf(TEXT(" Saved to %s."), *SavedPath);
		if (!Freshness.IsEmpty())
			Message += TEXT(" ") + Freshness;
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] %s"), *Message);
		return FUnrealMcpToolResult::SuccessWithImage(Message, Base64Png);
	}

	bool IsRenderingAvailable()
	{
		return FApp::CanEverRender() && !GUsingNullRHI;
	}

	FString NullRhiScreenshotError()
	{
		return TEXT("Screenshot tools are unavailable: this editor runs with the null RHI (-nullrhi), which renders nothing. "
		            "Restart it without -nullrhi: windowed, or headless with -RenderOffscreen, which renders screenshots "
		            "(unreal-mcp-cli starts headless editors that way; omit --nullrhi).");
	}

	bool ResolveCaptureSavePath(const FString& Requested, const FString& ProjectDir, FString& OutFullPath, FString& OutError)
	{
		OutFullPath.Reset();
		FString Path = Requested.TrimStartAndEnd();
		if (Path.IsEmpty())
			return true;
		const auto Refuse = [&OutError, &Requested](const FString& Why)
		{
			OutError = FString::Printf(TEXT("Cannot save to '%s': %s"), *Requested, *Why);
			return false;
		};
		FPaths::NormalizeFilename(Path);
		FPaths::RemoveDuplicateSlashes(Path);
		// Judged on the request, before any path collapsing: engines disagree on it. UE 5.5/5.6 keep a trailing "/." ("." then
		// becomes a file named "..png"), UE 5.7+ drop it ("Screenshots/." then becomes "Screenshots.png").
		const FString FileName = FPaths::GetCleanFilename(Path);
		if (FileName.IsEmpty() || FileName == TEXT(".") || FileName == TEXT(".."))
			return Refuse(TEXT("it names a folder; add a file name, such as Screenshots/shot.png."));
		if (FPaths::GetBaseFilename(FileName).Replace(TEXT("."), TEXT("")).IsEmpty())
			return Refuse(TEXT("it has no file name, only an extension; use a name such as Screenshots/shot.png."));
		const auto RefuseOutside = [&Refuse, &ProjectDir]()
		{
			return Refuse(FString::Printf(TEXT("it must stay inside the project folder %s (a path relative to it, or an absolute "
				"path under it; no ':' after the drive, no name ending in a dot or a space, no link leading out)."), *FPaths::ConvertRelativePathToFull(ProjectDir)));
		};
		const UnrealMcpSourceTools::FJailedPath Jailed = UnrealMcpSourceTools::ResolveJailedPath(ProjectDir, Path);
		if (!Jailed.bOk)
			return RefuseOutside();
		FString Full = Jailed.FullPath;
		const FString Extension = FPaths::GetExtension(Full);
		if (Extension.IsEmpty())
			Full += TEXT(".png");
		else if (!Extension.Equals(TEXT("png"), ESearchCase::IgnoreCase))
			return Refuse(TEXT("the capture is a PNG, so the file name must end in .png."));
		// The jail accepts its own root (the project's absolute path), and adding .png to that names a file beside the project.
		if (!FPaths::IsUnderDirectory(Full, FPaths::ConvertRelativePathToFull(ProjectDir)))
			return RefuseOutside();
		if (IFileManager::Get().FileExists(*Full))
			return Refuse(TEXT("the file already exists and a capture never overwrites one; pick another name."));
		OutFullPath = Full;
		return true;
	}

	bool WriteCapturePng(const FString& FullPath, TArrayView64<const uint8> Png, FString& OutError)
	{
		// Checked again here: the file can appear while the capture renders. A writer's FILEWRITE_NoReplaceExisting does not
		// help, the file manager ignores it and replaces the file.
		if (IFileManager::Get().FileExists(*FullPath))
		{
			OutError = FString::Printf(TEXT("%s already exists and a capture never overwrites one; nothing was saved."), *FullPath);
			return false;
		}
		if (FFileHelper::SaveArrayToFile(Png, *FullPath))
			return true;
		// A failed write can leave a partial file (a full disk); the check above says this call created it.
		IFileManager::Get().Delete(*FullPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
		OutError = FString::Printf(TEXT("Could not write the capture to %s (the disk is full, the folder is read-only, or a file has "
			"the name of one of its folders); nothing was saved."), *FullPath);
		return false;
	}

	FString FormatFreshness(bool bFrameFresh, const FString& FrameReason, bool bSimulationLive, const FString& SimulationReason)
	{
		const FString Frame = bFrameFresh
			? FString(TEXT("Frame: fresh (rendered at capture time)."))
			: FString::Printf(TEXT("Frame: possibly stale (%s)."), *FrameReason);
		const FString Simulation = bSimulationLive
			? FString(TEXT("Simulation: live."))
			: FString::Printf(TEXT("Simulation: paused (%s); effects and animation may not show the latest edits yet."), *SimulationReason);
		return Frame + TEXT(" ") + Simulation;
	}

	FFrameUniformity AnalyzeFrameUniformity(const TArray<FColor>& Pixels)
	{
		FFrameUniformity Result;
		if (Pixels.IsEmpty())
			return Result;

		// The most common value per channel, so a few overlay pixels (sprites, debug text) cannot move it.
		int32 CountR[256] = {}, CountG[256] = {}, CountB[256] = {};
		for (const FColor& Pixel : Pixels)
		{
			++CountR[Pixel.R];
			++CountG[Pixel.G];
			++CountB[Pixel.B];
		}
		Result.Color = FColor((uint8)(Algo::MaxElement(CountR) - CountR), (uint8)(Algo::MaxElement(CountG) - CountG),
			(uint8)(Algo::MaxElement(CountB) - CountB));

		constexpr int32 Tolerance = 4;
		int32 Close = 0;
		for (const FColor& Pixel : Pixels)
		{
			if (FMath::Abs(Pixel.R - Result.Color.R) <= Tolerance && FMath::Abs(Pixel.G - Result.Color.G) <= Tolerance
				&& FMath::Abs(Pixel.B - Result.Color.B) <= Tolerance)
			{
				++Close;
			}
		}
		Result.Share = (double)Close / (double)Pixels.Num();
		return Result;
	}

	TArray<IInterface_PostProcessVolume*> GetWorldPostProcessVolumes(const UWorld& World)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		return World.PostProcessVolumes;
#else
		TArray<IInterface_PostProcessVolume*> Volumes;
		for (IInterface_PostProcessVolume& Volume : World.GetPostProcessVolumeIterator())
			Volumes.Add(&Volume);
		return Volumes;
#endif
	}

	bool HasOverexposingPostProcessVolume(const TArray<IInterface_PostProcessVolume*>& Volumes, bool bExtendedLuminanceRange)
	{
		if (bExtendedLuminanceRange)
			return false;
		// Measured on 5.7 with the CVar off: 0 and 0.001 render white, 0.03 is washed out but not uniform, 1 is normal.
		constexpr float MaxBrightnessThatBlowsOut = 0.01f;
		bool bPinnedNearZero = false;
		bool bManualExposure = false;
		for (const IInterface_PostProcessVolume* Volume : Volumes)
		{
			const FPostProcessVolumeProperties Properties = Volume->GetProperties();
			if (!Properties.bIsEnabled || !Properties.bIsUnbound || Properties.BlendWeight <= 0.0f || !Properties.Settings)
				continue;
			const FPostProcessSettings& Settings = *Properties.Settings;
			if (Settings.bOverride_AutoExposureMethod)
				bManualExposure = Settings.AutoExposureMethod == AEM_Manual;
			if (Settings.bOverride_AutoExposureMaxBrightness)
				bPinnedNearZero = Settings.AutoExposureMaxBrightness <= MaxBrightnessThatBlowsOut && Properties.BlendWeight >= 0.99f;
		}
		return bPinnedNearZero && !bManualExposure;
	}

	FString FormatUniformFrameNote(const FFrameUniformity& Uniformity, bool bOverexposingVolume)
	{
		FString Note = FString::Printf(
			TEXT("Frame content: uniform (#%02X%02X%02X across %.1f%% of the pixels), so this image shows almost nothing of "
			     "the scene. That is fine for a really plain scene; otherwise the view is over-exposed or unlit. "
			     "screenshot-camera renders the scene with its own fixed exposure, which can differ from what the player sees."),
			Uniformity.Color.R, Uniformity.Color.G, Uniformity.Color.B, Uniformity.Share * 100.0);
		if (bOverexposingVolume)
		{
			Note += TEXT(" Likely cause, in the project and not in this capture: an unbound PostProcessVolume overrides Auto Exposure "
			             "Max Brightness to about 0 while r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange is off, so "
			             "the player sees this too. Fix: set r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange=True "
			             "under [/Script/Engine.RendererSettings] in Config/DefaultEngine.ini (console-run-command applies it "
			             "to this session), or clear the volume's override.");
		}
		return Note;
	}

	// ---- Local helpers ----------------------------------------------------------------------------

	namespace
	{
		// Keep the encoded payload well under the §1.2 64 MiB IPC line cap (base64 expands ~4/3).
		static constexpr int64 MaxEncodedBytes = 40 * 1024 * 1024;

		/** IsRenderingAvailable(), with NullRhiScreenshotError() in @p OutError when it is not. */
		bool EnsureRenderingAvailable(FString& OutError)
		{
			if (!IsRenderingAvailable())
			{
				OutError = NullRhiScreenshotError();
				return false;
			}
			return true;
		}

		/**
		 * Whether the world behind a capture is ticking at full rate, so its effects and animation are current. The editor
		 * world advances ticking content only while one of its viewports is realtime, and a throttled background editor
		 * runs a few frames per second. Background rendering (§10) lifts both while an MCP client works with the editor;
		 * this reports what is true at capture time.
		 */
		bool ScreenshotIsSimulationLive(UWorld* World, FString& OutReason)
		{
			if (!World || !GEditor)
				return true;
			if (GEditor->ShouldThrottleCPUUsage())
			{
				OutReason = TEXT("the editor is in the background and throttled by its 'Use Less CPU when in Background' "
				                 "preference, so it runs only a few frames per second");
				return false;
			}
			if (World->IsGameWorld())
			{
				// A play (or simulate) session ticks its world fully unless it is paused (Pause button, breakpoint, SetGamePaused).
				if (!World->IsPaused())
					return true;
				OutReason = TEXT("the play session is paused");
				return false;
			}
			FString OverrideMessage;
			for (const FEditorViewportClient* Client : GEditor->GetAllViewportClients())
			{
				if (!Client || Client->GetWorld() != World)
					continue;
				if (Client->IsRealtime())
					return true;
				if (OverrideMessage.IsEmpty() && Client->IsRealtimeOverrideSet())
					OverrideMessage = Client->GetRealtimeOverrideMessage().ToString();
			}
			// A realtime override (e.g. the editor's own "Background Process" one, removed on the next editor tick) is not
			// something Ctrl+R fixes: name it instead.
			OutReason = OverrideMessage.IsEmpty()
				? FString(TEXT("no viewport of this level is in realtime mode, so the editor world only advances time; "
				               "turn on Realtime in the level viewport (Ctrl+R)"))
				: FString::Printf(TEXT("realtime is overridden for this level's viewports (%s), so the editor world only "
				                       "advances time"), *OverrideMessage);
			return false;
		}

		/**
		 * Whether redrawing @p Viewport yields a new image. A game viewport draws nothing while game rendering is off, and a
		 * viewport that renders straight to its window's back buffer (no separate render target) has nothing current to
		 * read while that window is minimized. Editor and PIE viewports render to their own target, so minimized is fine.
		 */
		bool ScreenshotCanRedrawViewport(FViewport* Viewport, FString& OutReason)
		{
			const UWorld* World = Viewport->GetClient() ? Viewport->GetClient()->GetWorld() : nullptr;
			if (World && World->IsGameWorld() && !FViewport::IsGameRenderingEnabled())
			{
				OutReason = TEXT("game rendering is disabled, so the game view was not redrawn");
				return false;
			}
			if (Viewport->IsSlateViewport())
			{
				FSceneViewport* SceneViewport = static_cast<FSceneViewport*>(Viewport);
				const TSharedPtr<SWindow> Window = SceneViewport->FindWindow();
				if (Window.IsValid() && Window->IsWindowMinimized()
					&& !static_cast<const ISlateViewport*>(SceneViewport)->UseSeparateRenderTarget())
				{
					OutReason = TEXT("this viewport renders straight to its window, which is minimized; restore the window "
					                 "or use screenshot-camera");
					return false;
				}
			}
			return true;
		}

		/** The freshness line for a capture of @p World (see FormatFreshness). */
		FString ScreenshotFreshness(UWorld* World, bool bFrameFresh, const FString& FrameReason)
		{
			FString SimulationReason;
			const bool bLive = ScreenshotIsSimulationLive(World, SimulationReason);
			return FormatFreshness(bFrameFresh, FrameReason, bLive, SimulationReason);
		}

		/** Force every pixel opaque so a viewport/render-target alpha of 0 does not yield a transparent PNG. */
		void ForceOpaque(TArray<FColor>& Pixels)
		{
			for (FColor& Pixel : Pixels)
				Pixel.A = 255;
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

		/** Encode a BGRA8 FColor buffer to a base64 PNG (forcing opaque first) and write it to @p SavePath when one is given. */
		bool EncodeAndSavePng(TArray<FColor>& Pixels, int32 Width, int32 Height, const FString& SavePath,
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
			if (!SavePath.IsEmpty() && !WriteCapturePng(SavePath, Png, OutError))
				return false;
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
		FUnrealMcpToolResult CaptureFromViewport(const FUnrealMcpToolCall& Call, FViewport* Viewport, const FString& Source, const FString& SavePath)
		{
			const FIntPoint Size = Viewport->GetSizeXY();
			if (Size.X <= 0 || Size.Y <= 0)
				return FUnrealMcpToolResult::Error(FString::Printf(TEXT("The %s has a zero-sized render area."), *Source));

			// Render a new frame now, focused or not: the editor does not redraw a minimized window or a throttled
			// background viewport on its own. Pending scene updates (an edit made earlier this frame) go to the renderer
			// first; ReadPixels then flushes the render thread before it reads.
			UWorld* World = Viewport->GetClient() ? Viewport->GetClient()->GetWorld() : nullptr;
			if (World)
				World->SendAllEndOfFrameUpdates();
			FString FrameReason;
			const bool bFrameFresh = ScreenshotCanRedrawViewport(Viewport, FrameReason);
			// Draw a PIE game view inside its world context, as UEditorEngine::Tick does (GWorld / PIE instance id).
			const bool bPieContext = GEditor && World && World == GEditor->PlayWorld && !GEditor->bIsSimulatingInEditor
				&& !GIsPlayInEditorWorld;
			UWorld* SavedWorld = bPieContext ? SetPlayInEditorWorld(World) : nullptr;
			Viewport->Draw();
			if (bPieContext)
				RestoreEditorWorld(SavedWorld);

			TArray<FColor> Pixels;
			if (!Viewport->ReadPixels(Pixels, FReadSurfaceDataFlags(), FIntRect(0, 0, Size.X, Size.Y)))
				return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to read pixels from the %s."), *Source));

			// Measured before the resize, which would blur overlay pixels into the blank frame around them.
			const FFrameUniformity Uniformity = AnalyzeFrameUniformity(Pixels);

			int32 OutW = 0, OutH = 0;
			EffectiveViewportSize(Call, Size.X, Size.Y, OutW, OutH);
			ResizeIfNeeded(Pixels, Size.X, Size.Y, OutW, OutH);

			FString Base64; int32 Bytes = 0; FString Error;
			if (!EncodeAndSavePng(Pixels, OutW, OutH, SavePath, Base64, Bytes, Error))
				return FUnrealMcpToolResult::Error(Error);

			FString Notes = ScreenshotFreshness(World, bFrameFresh, FrameReason);
			if (Uniformity.IsUniform())
			{
				const IConsoleVariable* ExtendedRange = IConsoleManager::Get().FindConsoleVariable(
					TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"));
				// This cause only blows a frame out, so it cannot explain a dark or colored one.
				const bool bWhite = FMath::Min3(Uniformity.Color.R, Uniformity.Color.G, Uniformity.Color.B) >= 250;
				const bool bOverexposingVolume = bWhite && World && ExtendedRange
					&& HasOverexposingPostProcessVolume(GetWorldPostProcessVolumes(*World), ExtendedRange->GetBool());
				Notes += TEXT(" ") + FormatUniformFrameNote(Uniformity, bOverexposingVolume);
			}
			return MakeImageResult(Source, OutW, OutH, Base64, Bytes, Notes, SavePath);
		}

		/**
		 * Render through a transient SceneCapture2D into a transient render target, then read back. The
		 * capture actor is spawned RF_Transient + hidden from the outliner and destroyed on every path, so
		 * the editor world is never dirtied. Called only on the GPU-available path.
		 */
		FUnrealMcpToolResult CaptureWithSceneCapture(
			UWorld* World, const FTransform& CaptureXform, float FovDeg, int32 Width, int32 Height,
			const FString& Source, const FLinearColor* BackgroundColor, AActor* ShowOnlyActor, const FString& SavePath = FString())
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
			// CaptureScene renders the scene now (and the read-back below flushes the render thread), so the frame is
			// fresh whatever the editor window's state; send pending scene updates first so the latest edit is in it.
			World->SendAllEndOfFrameUpdates();
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
			if (!EncodeAndSavePng(Pixels, Width, Height, SavePath, Base64, Bytes, Error))
				return FUnrealMcpToolResult::Error(Error);

			return MakeImageResult(Source, Width, Height, Base64, Bytes, ScreenshotFreshness(World, /*bFrameFresh*/ true, FString()), SavePath);
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
			return TEXT(" Renders a new frame at call time, also while the editor window is in the background or "
			            "minimized. Returns a base64 PNG as MCP image content; the text states 'Frame: fresh' or 'Frame: "
			            "possibly stale (reason)', and 'Simulation: live' or 'Simulation: paused (reason)' when effects and "
			            "animation were not ticking. Optional 'width'/'height' are clamped to [1, 2048] per side (defaults: "
			            "see each parameter). Needs an editor that renders: windowed, or headless with -RenderOffscreen; under "
			            "-nullrhi nothing renders and the tool returns an error.");
		}

		/** Description tail of the tools that can save their capture. */
		const TCHAR* SavePathNote()
		{
			return TEXT(" To give the user a file (they asked to see the result), pass 'savePath': the PNG is also written "
			            "inside the project and the text states 'Saved to <absolute path>'.");
		}

		const TCHAR* SavePathParamDoc()
		{
			return TEXT("Optional file to also write the PNG to, inside the project folder: relative to it (Screenshots/shot.png), "
			            "or absolute under it; missing folders are created and .png is added when there is no extension. "
			            "Refused when outside the project, when not a .png, or when the file exists (it never overwrites).");
		}

		/** The checked 'savePath' of @p Call (empty when not given); false with @p OutError when it is refused. */
		bool ScreenshotSavePath(const FUnrealMcpToolCall& Call, FString& OutPath, FString& OutError)
		{
			return ResolveCaptureSavePath(Call.GetString(TEXT("savePath")), FPaths::ProjectDir(), OutPath, OutError);
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
				"capture the game view explicitly.")) + SizeCapNote() + SavePathNote())
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); when only 'height' is given the width is derived from the native aspect ratio, and the native viewport width is used when both are omitted."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); when only 'width' is given the height is derived from the native aspect ratio, and the native viewport height is used when both are omitted."))
			.ParamString(TEXT("savePath"), SavePathParamDoc())
			.ReadOnlyHint(false).DestructiveHint(false).IdempotentHint(false).OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FString SavePath, Error;
				if (!ScreenshotSavePath(Call, SavePath, Error))
					return FUnrealMcpToolResult::Error(Error);
				if (!EnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				FViewport* Viewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
				if (!Viewport)
					return FUnrealMcpToolResult::Error(TEXT("No active editor viewport. Focus a level editor viewport and retry."));

				return CaptureFromViewport(Call, Viewport, TEXT("editor viewport"), SavePath);
			});

		// screenshot-game-view — PIE/game view; structured error when no PIE session is active.
		Registry.Tool(TEXT("screenshot-game-view"))
			.Title(TEXT("Screenshot Game View"))
			.Description(FString(TEXT("Capture the Play-In-Editor game view. Errors when no PIE session is active.")) + SizeCapNote() + SavePathNote())
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); when only 'height' is given the width is derived from the native aspect ratio, and the native game-view width is used when both are omitted."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); when only 'width' is given the height is derived from the native aspect ratio, and the native game-view height is used when both are omitted."))
			.ParamString(TEXT("savePath"), SavePathParamDoc())
			.ReadOnlyHint(false).DestructiveHint(false).IdempotentHint(false).OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FString SavePath, Error;
				if (!ScreenshotSavePath(Call, SavePath, Error))
					return FUnrealMcpToolResult::Error(Error);

				// Validate the PIE precondition before the GPU guard (GPU-free, headless-spec-covered).
				const bool bPlaySessionActive = GEditor && GEditor->PlayWorld != nullptr;
				FViewport* PieViewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
				if (!bPlaySessionActive || !PieViewport)
					return FUnrealMcpToolResult::Error(TEXT("No Play-In-Editor session is active. Start PIE before calling screenshot-game-view."));

				if (!EnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				return CaptureFromViewport(Call, PieViewport, TEXT("PIE game view"), SavePath);
			});

		// screenshot-camera — render from a resolved camera actor via SceneCapture2D.
		Registry.Tool(TEXT("screenshot-camera"))
			.Title(TEXT("Screenshot Camera"))
			.Description(FString(TEXT("Render the scene from a chosen camera actor (resolved by label/name/path) and capture it.")) + SizeCapNote() + SavePathNote())
			.ParamString(TEXT("camera"), TEXT("Camera actor reference (label, object name, or path) to render from."), EUnrealMcpParamRequirement::Required)
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); default 1024."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); default 1024."))
			.ParamNumber(TEXT("fov"), TEXT("Optional horizontal field-of-view override in degrees (clamped to [5, 170]); defaults to the camera component's FOV (or 90)."))
			.ParamString(TEXT("savePath"), SavePathParamDoc())
			.ReadOnlyHint(false).DestructiveHint(false).IdempotentHint(false).OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Validate arguments + resolve the camera FIRST (GPU-free, headless-spec-covered).
				FString SavePath, Error;
				if (!ScreenshotSavePath(Call, SavePath, Error))
					return FUnrealMcpToolResult::Error(Error);
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

				if (!EnsureRenderingAvailable(Error))
					return FUnrealMcpToolResult::Error(Error);

				const int32 Width = ResolveCaptureDimension(Call.GetInt(TEXT("width"), 0));
				const int32 Height = ResolveCaptureDimension(Call.GetInt(TEXT("height"), 0));
				const FString Source = FString::Printf(TEXT("camera '%s'"), *CameraActor->GetActorNameOrLabel());
				return CaptureWithSceneCapture(World, CaptureXform, Fov, Width, Height, Source, nullptr, nullptr, SavePath);
			});

		// screenshot-isolated — isolated actor render (transient SceneCapture2D + show-only list + background).
		Registry.Tool(TEXT("screenshot-isolated"))
			.Title(TEXT("Screenshot Isolated Actor"))
			.Description(FString(TEXT("Render a single actor in isolation against a neutral background, auto-framed by its bounds.")) + SizeCapNote() + SavePathNote())
			.ParamString(TEXT("actor"), TEXT("Target actor reference (label, object name, or path) to render in isolation."), EUnrealMcpParamRequirement::Required)
			.ParamInt(TEXT("width"), TEXT("Optional output width in pixels (clamped to [1, 2048]); default 1024."))
			.ParamInt(TEXT("height"), TEXT("Optional output height in pixels (clamped to [1, 2048]); default 1024."))
			.ParamString(TEXT("background"), TEXT("Optional background color as hex ('#RRGGBB' or '#RRGGBBAA'); the alpha of '#RRGGBBAA' is accepted but ignored (the PNG is always opaque). Defaults to dark grey."))
			.ParamNumber(TEXT("fov"), TEXT("Optional field-of-view override in degrees (clamped to [5, 170]); default 50."))
			.ParamString(TEXT("savePath"), SavePathParamDoc())
			.ReadOnlyHint(false).DestructiveHint(false).IdempotentHint(false).OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Validate arguments + resolve the actor FIRST (GPU-free, headless-spec-covered).
				FString SavePath, Error;
				if (!ScreenshotSavePath(Call, SavePath, Error))
					return FUnrealMcpToolResult::Error(Error);
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
				return CaptureWithSceneCapture(World, CaptureXform, Fov, Width, Height, Source, &Background, TargetActor, SavePath);
			});
	}
}
