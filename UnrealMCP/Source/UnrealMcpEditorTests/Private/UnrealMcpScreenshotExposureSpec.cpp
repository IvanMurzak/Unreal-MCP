// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Containers/Ticker.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "ImageCore.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"

/**
 * DEV-269: screenshot-camera of a CineCameraActor in a lit Lumen scene must be exposed like screenshot-viewport of
 * the same view (it came back near-black: scene captures render without Lumen and without exposure history).
 *
 * Needs a rendering editor (windowed or -RenderOffscreen). Under -nullrhi nothing renders, so the spec only notes
 * that and passes; run it live as UnrealMcp.Tools.Screenshot.Exposure.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpScreenshotExposureSpec, "UnrealMcp.Tools.Screenshot.Exposure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	TArray<TWeakObjectPtr<AActor>> SceneActors;
	FTSTicker::FDelegateHandle TickerHandle;

	/** The level viewport the spec pilots, and its view before, restored in AfterEach (it is saved with the layout). */
	FLevelEditorViewportClient* PilotedClient = nullptr;
	FVector SavedViewLocation;
	FRotator SavedViewRotation;
	float SavedViewFov = 90.0f;

	static FText ExposureSpecRealtimeReason()
	{
		return FText::FromString(TEXT("UnrealMcp.Tools.Screenshot.Exposure spec"));
	}

	/** PilotedClient while it is still a live level viewport (one can close during the run), else nullptr. */
	FLevelEditorViewportClient* LivePilotedClient() const
	{
		return PilotedClient && GEditor && GEditor->GetLevelViewportClients().Contains(PilotedClient) && PilotedClient->Viewport
			? PilotedClient : nullptr;
	}

	void RestoreViewport()
	{
		if (FLevelEditorViewportClient* Client = LivePilotedClient())
		{
			Client->SetViewLocation(SavedViewLocation);
			Client->SetViewRotation(SavedViewRotation);
			Client->ViewFOV = SavedViewFov;
			Client->RemoveRealtimeOverride(ExposureSpecRealtimeReason(), /*bCheckMissingOverride*/ false);
			Client->Invalidate();
		}
		PilotedClient = nullptr;
	}

	template <typename T>
	T* SpawnSceneActor(UWorld* World, const FTransform& Transform)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		SpawnParams.bTemporaryEditorActor = true;
		SpawnParams.bHideFromSceneOutliner = true;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		T* Actor = World->SpawnActor<T>(T::StaticClass(), Transform, SpawnParams);
		if (Actor)
			SceneActors.Add(Actor);
		return Actor;
	}

	void SpawnMesh(UWorld* World, UStaticMesh* Mesh, const FVector& Location, const FVector& Scale)
	{
		AStaticMeshActor* Actor = SpawnSceneActor<AStaticMeshActor>(World, FTransform(FRotator::ZeroRotator, Location, Scale));
		if (Actor)
		{
			Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		}
	}

	void DestroyScene()
	{
		for (const TWeakObjectPtr<AActor>& Actor : SceneActors)
		{
			if (Actor.IsValid())
				Actor->Destroy();
		}
		SceneActors.Reset();
	}

	/** Mean Rec.709 luma of a tool's PNG image, in [0, 255]; -1 when the result carries no decodable image. */
	double ExposureSpecMeanLuma(const FUnrealMcpToolResult& Result)
	{
		if (!Result.bSuccess || Result.Images.Num() != 1)
			return -1.0;
		TArray<uint8> Png;
		FImage Image;
		if (!FBase64::Decode(Result.Images[0].Base64Data, Png) || !FImageUtils::DecompressImage(Png.GetData(), Png.Num(), Image))
			return -1.0;
		FImage Bgra;
		Image.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<FColor> Pixels = Bgra.AsBGRA8();
		if (Pixels.IsEmpty())
			return -1.0;
		double Sum = 0.0;
		for (const FColor& Pixel : Pixels)
			Sum += 0.2126 * Pixel.R + 0.7152 * Pixel.G + 0.0722 * Pixel.B;
		return Sum / (double)Pixels.Num();
	}
END_DEFINE_SPEC(FUnrealMcpScreenshotExposureSpec)

void FUnrealMcpScreenshotExposureSpec::Define()
{
	AfterEach([this]()
	{
		if (TickerHandle.IsValid())
			FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
		DestroyScene();
		RestoreViewport();
	});

	LatentIt("screenshot-camera of a golden-hour Lumen scene is exposed like screenshot-viewport of the same view",
		FTimespan::FromSeconds(180), [this](const FDoneDelegate& Done)
	{
		if (!UnrealMcpScreenshotTools::IsRenderingAvailable())
		{
			AddInfo(TEXT("Rendering is unavailable (-nullrhi): run this spec in a rendering editor (-RenderOffscreen)."));
			Done.Execute();
			return;
		}
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		// screenshot-viewport captures the active viewport: it must be the current level viewport, in perspective.
		FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
		if (Client && (!Client->Viewport || Client->Viewport != GEditor->GetActiveViewport() || !Client->IsPerspective()))
		{
			AddInfo(TEXT("The active viewport is not a perspective level viewport; nothing to compare against."));
			Done.Execute();
			return;
		}
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		if (!TestNotNull(TEXT("editor world"), World) || !TestNotNull(TEXT("current level viewport client"), Client)
			|| !TestNotNull(TEXT("cube mesh"), Cube) || !TestNotNull(TEXT("plane mesh"), Plane))
		{
			Done.Execute();
			return;
		}

		// Golden hour: a low sun driving the sky atmosphere, a real-time sky light, a floor and a few blocks.
		ADirectionalLight* Sun = SpawnSceneActor<ADirectionalLight>(World, FTransform(FRotator(-6.0f, 40.0f, 0.0f)));
		if (Sun)
		{
			UDirectionalLightComponent* SunLight = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
			SunLight->SetMobility(EComponentMobility::Movable);
			SunLight->SetAtmosphereSunLight(true);
			SunLight->SetIntensity(10.0f);
		}
		if (AActor* Sky = SpawnSceneActor<AActor>(World, FTransform::Identity))
		{
			USkyAtmosphereComponent* Atmosphere = NewObject<USkyAtmosphereComponent>(Sky);
			Sky->SetRootComponent(Atmosphere);
			Atmosphere->RegisterComponent();
		}
		if (ASkyLight* SkyLight = SpawnSceneActor<ASkyLight>(World, FTransform::Identity))
		{
			USkyLightComponent* SkyLightComponent = SkyLight->GetLightComponent();
			SkyLightComponent->SetMobility(EComponentMobility::Movable);
			SkyLightComponent->SetRealTimeCapture(true);
		}
		SpawnMesh(World, Plane, FVector::ZeroVector, FVector(200.0f, 200.0f, 1.0f));
		for (int32 Index = 0; Index < 5; ++Index)
			SpawnMesh(World, Cube, FVector(800.0f + Index * 250.0f, (Index - 2) * 300.0f, 100.0f), FVector(2.0f, 2.0f, 2.0f + Index));

		const FVector CameraLocation(-200.0f, 0.0f, 250.0f);
		const FRotator CameraRotation(-4.0f, 0.0f, 0.0f);
		ACineCameraActor* Camera = SpawnSceneActor<ACineCameraActor>(World, FTransform(CameraRotation, CameraLocation));
		if (!TestNotNull(TEXT("cine camera"), Camera))
		{
			Done.Execute();
			return;
		}
		const FMinimalViewInfo CameraView = [Camera]()
		{
			FMinimalViewInfo View;
			Camera->GetCineCameraComponent()->GetCameraView(0.0f, View);
			return View;
		}();

		// Pilot the level viewport to the camera (what an agent does with set_level_viewport_camera_info) and let it
		// converge (Lumen, eye adaptation) for a while before both captures.
		PilotedClient = Client;
		SavedViewLocation = Client->GetViewLocation();
		SavedViewRotation = Client->GetViewRotation();
		SavedViewFov = Client->ViewFOV;
		Client->AddRealtimeOverride(true, ExposureSpecRealtimeReason());
		Client->SetViewLocation(CameraView.Location);
		Client->SetViewRotation(CameraView.Rotation);
		Client->ViewFOV = CameraView.FOV;
		const FString CameraName = Camera->GetName();
		int32 FramesLeft = 120;
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[this, Done, CameraName, FramesLeft](float) mutable
		{
			FLevelEditorViewportClient* LiveClient = LivePilotedClient();
			if (!LiveClient || LiveClient->Viewport != GEditor->GetActiveViewport())
			{
				AddError(TEXT("The piloted level viewport closed or lost focus during the run."));
				TickerHandle.Reset();
				Done.Execute();
				return false;
			}
			FViewport* Viewport = LiveClient->Viewport;
			if (--FramesLeft > 0)
			{
				// An unfocused or offscreen editor may not redraw the level viewport on its own: draw it every frame,
				// so its Lumen and eye adaptation converge like an interactive viewport's.
				Viewport->Draw();
				return true;
			}
			TickerHandle.Reset();

			FUnrealMcpToolRegistry Registry;
			UnrealMcpScreenshotTools::Register(Registry);
			const FIntPoint Size = Viewport->GetSizeXY();
			const TSharedPtr<FJsonObject> CameraArgs = MakeShared<FJsonObject>();
			CameraArgs->SetStringField(TEXT("camera"), CameraName);
			CameraArgs->SetNumberField(TEXT("width"), 512);
			CameraArgs->SetNumberField(TEXT("height"), Size.X > 0 ? FMath::Max(1, FMath::RoundToInt(512.0 * Size.Y / Size.X)) : 288);
			const TSharedPtr<FJsonObject> ViewportArgs = MakeShared<FJsonObject>();
			ViewportArgs->SetNumberField(TEXT("width"), 512);

			const FUnrealMcpToolResult CameraShot = Registry.Execute(TEXT("screenshot-camera"), FUnrealMcpToolCall(CameraArgs));
			const FUnrealMcpToolResult ViewportShot = Registry.Execute(TEXT("screenshot-viewport"), FUnrealMcpToolCall(ViewportArgs));
			TestTrue(FString::Printf(TEXT("screenshot-camera succeeds: %s"), *CameraShot.Message), CameraShot.bSuccess);
			TestTrue(FString::Printf(TEXT("screenshot-viewport succeeds: %s"), *ViewportShot.Message), ViewportShot.bSuccess);
			const double CameraLuma = ExposureSpecMeanLuma(CameraShot);
			const double ViewportLuma = ExposureSpecMeanLuma(ViewportShot);
			AddInfo(FString::Printf(TEXT("DEV-269 mean luma (0-255): screenshot-camera %.1f, screenshot-viewport %.1f (viewport %dx%d)"),
				CameraLuma, ViewportLuma, Size.X, Size.Y));
			if (TestTrue(TEXT("both images decode"), CameraLuma >= 0.0 && ViewportLuma >= 0.0))
			{
				TestTrue(TEXT("the viewport frame is lit (a meaningful reference)"), ViewportLuma > 20.0);
				// Same view, same lighting: exposure may differ a little, never by a near-black factor.
				TestTrue(TEXT("the camera frame is within 0.6x..1.6x of the viewport's mean luma"),
					CameraLuma >= ViewportLuma * 0.6 && CameraLuma <= ViewportLuma * 1.6);
			}
			Done.Execute();
			return false;
		}));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
