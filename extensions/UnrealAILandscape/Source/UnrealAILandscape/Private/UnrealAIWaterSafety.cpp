// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIWaterSafety.h"

#include "ActorFactories/ActorFactory.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "LandscapeSettings.h"
#include "Materials/Material.h"
#include "Misc/CoreDelegates.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIWaterSafety, Log, All);

namespace UnrealAIWaterSafetyLocal
{
	bool bPreviousDialog = true;
	FDelegateHandle ActorAddedHandle;
	FDelegateHandle EngineInitHandle;
	/** How many FWaterSpawnScopes are alive (they nest): their bodies get their operation's setup, not the repair. */
	int32 SpawnScopes = 0;
	/** Bodies placed without the Water defaults, repaired on the next tick (after their spawner had its turn). */
	TArray<TWeakObjectPtr<AWaterBody>> Pending;

	void RepairPending()
	{
		TArray<TWeakObjectPtr<AWaterBody>> Bodies = MoveTemp(Pending);
		for (const TWeakObjectPtr<AWaterBody>& Weak : Bodies)
		{
			AWaterBody* Body = Weak.Get();
			const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
			// A factory placement (Place Actors) has its defaults by now; only a bare spawn still has no material.
			if (Component && !Component->GetWaterMaterial() && !Component->GetWaterMeshOverride() && UnrealAIWaterSafety::ApplyWaterDefaults(Body))
				UE_LOG(LogUnrealAIWaterSafety, Log, TEXT("%s was placed without the Water defaults (no material, invisible): gave it the Place Actors defaults of its type"), *Body->GetActorLabel());
		}
	}

	void OnActorAdded(AActor* Actor)
	{
		AWaterBody* Body = Cast<AWaterBody>(Actor);
		const UWorld* World = Body ? Body->GetWorld() : nullptr;
		if (!World || World->WorldType != EWorldType::Editor || SpawnScopes > 0 || Body->bIsEditorPreviewActor || Body->HasAnyFlags(RF_Transient))
			return;
		if (Pending.IsEmpty())
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				RepairPending();
				return false; // once
			}));
		}
		Pending.AddUnique(Body);
	}

	/**
	 * UActorFactory::PostSpawnActor (what the Place Actors panel runs after spawning: the Water factories' defaults) is
	 * protected; a pointer to it taken in a derived class's scope calls it (virtually) on any factory. Never instantiated.
	 */
	struct FPostSpawnAccess : UActorFactory
	{
		static constexpr auto PostSpawn = &FPostSpawnAccess::PostSpawnActor;
	};

	void Register()
	{
		if (GEngine && !ActorAddedHandle.IsValid())
			ActorAddedHandle = GEngine->OnLevelActorAdded().AddStatic(&OnActorAdded);
	}
}

bool UnrealAIWaterSafety::IsWaterMaterial(const UMaterialInterface* Material)
{
	const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	return Base && Base->MaterialDomain == MD_Surface && Base->GetUsageByFlag(MATUSAGE_Water);
}

void UnrealAIWaterSafety::Rebuild(AWaterBody* Body)
{
	UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
	if (!Component)
		return;
	FOnWaterBodyChangedParams Params;
	Params.bShapeOrPositionChanged = true;
	Params.bUserTriggered = true;
	Component->OnWaterBodyChanged(Params); // shape, collision, zone assignment and (in a rendering editor) the water info mesh
}

bool UnrealAIWaterSafety::ApplyWaterDefaults(AWaterBody* Body)
{
	UActorFactory* Factory = GEditor && Body ? GEditor->FindActorFactoryForActorClass(Body->GetClass()) : nullptr;
	UWaterSplineComponent* Spline = Body ? Body->GetWaterSpline() : nullptr;
	if (!Factory || !Spline)
		return false;
	// The factory also resets the spline to its default shape: keep the body's own points (tangents, types) and their
	// per-point water data (width, depth, velocity, audio).
	TArray<FSplinePoint> Points;
	for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
		Points.Add(Spline->GetSplinePointAt(Index, ESplineCoordinateSpace::Local));
	UWaterSplineMetadata* Metadata = Body->GetWaterBodyComponent()->GetWaterSplineMetadata();
	UWaterSplineMetadata* Saved = Metadata ? DuplicateObject(Metadata, GetTransientPackage()) : nullptr;
	if (Metadata)
		Metadata->Modify();
	Body->Modify();
	Body->GetWaterBodyComponent()->Modify();
	Spline->Modify();
	(Factory->*UnrealAIWaterSafetyLocal::FPostSpawnAccess::PostSpawn)(Body->GetClass(), Body);
	if (Points.Num())
	{
		Spline->ClearSplinePoints(false);
		Spline->AddPoints(Points, false);
		Spline->UpdateSpline();
		Spline->SynchronizeWaterProperties();
		if (Metadata && Saved && Saved->RiverWidth.Points.Num() == Points.Num())
		{
			Metadata->Depth = Saved->Depth;
			Metadata->WaterVelocityScalar = Saved->WaterVelocityScalar;
			Metadata->RiverWidth = Saved->RiverWidth;
			Metadata->AudioIntensity = Saved->AudioIntensity;
		}
	}
	Rebuild(Body);
	return true;
}

void UnrealAIWaterSafety::Start()
{
	using namespace UnrealAIWaterSafetyLocal;
	ULandscapeSettings* Settings = GetMutableDefault<ULandscapeSettings>();
	bPreviousDialog = Settings->bShowDialogForAutomaticLayerCreation;
	Settings->bShowDialogForAutomaticLayerCreation = false;
	if (GEngine)
		Register();
	else
		EngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&Register);
}

void UnrealAIWaterSafety::Stop()
{
	using namespace UnrealAIWaterSafetyLocal;
	if (UObjectInitialized())
		GetMutableDefault<ULandscapeSettings>()->bShowDialogForAutomaticLayerCreation = bPreviousDialog;
	FCoreDelegates::OnPostEngineInit.Remove(EngineInitHandle);
	if (GEngine)
		GEngine->OnLevelActorAdded().Remove(ActorAddedHandle);
	ActorAddedHandle.Reset();
	Pending.Reset();
}

UnrealAIWaterSafety::FWaterSpawnScope::FWaterSpawnScope(UWorld* InWorld)
	: World(InWorld)
{
	++UnrealAIWaterSafetyLocal::SpawnScopes;
	if (InWorld)
	{
		Hook = InWorld->AddOnActorPreSpawnInitialization(FOnActorSpawned::FDelegate::CreateLambda([](AActor* Actor)
		{
			if (const AWaterBody* Body = Cast<AWaterBody>(Actor))
			{
				if (UWaterBodyComponent* Component = Body->GetWaterBodyComponent())
					Component->bAffectsLandscape = false;
			}
		}));
	}
}

UnrealAIWaterSafety::FWaterSpawnScope::~FWaterSpawnScope()
{
	--UnrealAIWaterSafetyLocal::SpawnScopes;
	if (UWorld* Owner = World.Get())
		Owner->RemoveOnActorPreSpawnInitialization(Hook);
}
