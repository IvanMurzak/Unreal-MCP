// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIWaterSafety.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeBlueprintBrushBase.h"
#include "LandscapeSettings.h"
#include "Materials/Material.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIWaterSafety, Log, All);

namespace UnrealAIWaterSafetyLocal
{
	/** The name the Water plugin's brush asks for (AWaterLandscapeBrush::SetTargetLandscape). */
	const FName WaterLayerName(TEXT("Water"));

	FDelegateHandle WorldInitHandle;
	FDelegateHandle WorldCleanupHandle;
	TMap<TWeakObjectPtr<UWorld>, FDelegateHandle> Watched;
	/** How many FWaterSpawnScopes are alive (they nest). */
	int32 SpawnScopes = 0;

	/** Whether spawning @p Actor can make the Water editor ask the landscape for a "Water" edit layer. */
	bool NeedsWaterLayer(const AActor* Actor)
	{
		if (const AWaterBody* Body = Cast<AWaterBody>(Actor))
		{
			const UWaterBodyComponent* Component = Body->GetWaterBodyComponent();
			return Component && Component->bAffectsLandscape;
		}
		return Actor && Actor->IsA<ALandscapeBlueprintBrushBase>();
	}

	void Watch(UWorld* World)
	{
		if (!World || World->WorldType != EWorldType::Editor || Watched.Contains(World))
			return;
		const TWeakObjectPtr<UWorld> Weak(World);
		Watched.Add(World, World->AddOnActorPreSpawnInitialization(FOnActorSpawned::FDelegate::CreateLambda([Weak](AActor* Actor)
		{
			if (SpawnScopes > 0)
			{
				if (const AWaterBody* Body = Cast<AWaterBody>(Actor))
				{
					if (UWaterBodyComponent* Component = Body->GetWaterBodyComponent())
						Component->bAffectsLandscape = false;
				}
				return;
			}
			if (UWorld* Owner = Weak.Get(); Owner && NeedsWaterLayer(Actor))
			{
				const int32 Added = UnrealAIWaterSafety::PrepareWaterEditLayers(Owner);
				if (Added > 0)
					UE_LOG(LogUnrealAIWaterSafety, Log, TEXT("added a \"Water\" edit layer to %d landscape(s) before %s, so the Water plugin does not open the modal \"Insert New Landscape Edit Layer\" dialog"), Added, *Actor->GetClass()->GetName());
			}
		})));
	}

	void Unwatch(UWorld* World)
	{
		FDelegateHandle Handle;
		if (World && Watched.RemoveAndCopyValue(World, Handle))
			World->RemoveOnActorPreSpawnInitialization(Handle);
	}
}

bool UnrealAIWaterSafety::IsWaterMaterial(const UMaterialInterface* Material)
{
	const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	return Base && Base->MaterialDomain == MD_Surface && Base->GetUsageByFlag(MATUSAGE_Water);
}

int32 UnrealAIWaterSafety::PrepareWaterEditLayers(UWorld* World)
{
	using namespace UnrealAIWaterSafetyLocal;
	int32 Added = 0;
	if (!World)
		return 0;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		ALandscape* Landscape = *It;
		if (!IsValid(Landscape) || !Landscape->GetLandscapeGuid().IsValid() || Landscape->GetLayerIndex(WaterLayerName) != INDEX_NONE)
			continue;
		Landscape->Modify();
		if (Landscape->CreateLayer(WaterLayerName) != INDEX_NONE)
			++Added;
	}
	return Added;
}

void UnrealAIWaterSafety::Start()
{
	using namespace UnrealAIWaterSafetyLocal;
	WorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddLambda([](UWorld* World, const UWorld::InitializationValues) { Watch(World); });
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* World, bool, bool) { Unwatch(World); });
	for (TObjectIterator<UWorld> It; It; ++It)
		Watch(*It);
}

void UnrealAIWaterSafety::Stop()
{
	using namespace UnrealAIWaterSafetyLocal;
	FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitHandle);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	for (const TPair<TWeakObjectPtr<UWorld>, FDelegateHandle>& Entry : Watched)
	{
		if (UWorld* World = Entry.Key.Get())
			World->RemoveOnActorPreSpawnInitialization(Entry.Value);
	}
	Watched.Reset();
}

UnrealAIWaterSafety::FWaterSpawnScope::FWaterSpawnScope(UWorld* World)
{
	UnrealAIWaterSafetyLocal::Watch(World);
	++UnrealAIWaterSafetyLocal::SpawnScopes;
	ULandscapeSettings* Settings = GetMutableDefault<ULandscapeSettings>();
	bPreviousDialog = Settings->bShowDialogForAutomaticLayerCreation;
	Settings->bShowDialogForAutomaticLayerCreation = false;
}

UnrealAIWaterSafety::FWaterSpawnScope::~FWaterSpawnScope()
{
	--UnrealAIWaterSafetyLocal::SpawnScopes;
	GetMutableDefault<ULandscapeSettings>()->bShowDialogForAutomaticLayerCreation = bPreviousDialog;
}
