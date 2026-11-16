// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlWorldPartition.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlEditorApi.h"
#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionBlueprintLibrary.h"
#include "WorldPartition/WorldPartitionHelpers.h"

namespace UnrealMcpRqlWorldPartitionLocal
{
	/**
	 * Whether the actor of @p Instance was deleted in this session: its descriptor stays until the level is saved, while its
	 * package stays in memory, dirty (deleted on save), without the actor. An unloaded actor's package is not in memory, or
	 * not dirty (World Partition does not unload dirty actors).
	 */
	bool IsDeletedActor(const FWorldPartitionActorDescInstance& Instance)
	{
		const UPackage* Package = FindObject<UPackage>(nullptr, *Instance.GetActorPackage().ToString());
		return Package && Package->IsDirty();
	}

	FString ClassName(const FWorldPartitionActorDescInstance& Instance)
	{
		// The base class is the Blueprint class of a Blueprint actor ("BP_Door_C"), else invalid: the native class then.
		const FTopLevelAssetPath Base = Instance.GetBaseClass();
		return (Base.IsValid() ? Base : Instance.GetNativeClass()).GetAssetName().ToString();
	}

	FString LabelOf(const FWorldPartitionActorDescInstance& Instance)
	{
		const FName Label = Instance.GetActorLabel();
		return (Label.IsNone() ? Instance.GetActorName() : Label).ToString();
	}

	/** Each actor descriptor of @p World's World Partition whose actor is not loaded (see Unloaded). */
	void ForEachUnloaded(UWorld* World, TFunctionRef<void(const FWorldPartitionActorDescInstance&)> Func)
	{
		UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
		if (!Partition)
			return;
		FWorldPartitionHelpers::ForEachActorDescInstance(Partition, AActor::StaticClass(), [&](const FWorldPartitionActorDescInstance* Instance)
		{
			if (Instance && !Instance->IsLoaded() && !Instance->GetActorIsRuntimeOnly() && !IsDeletedActor(*Instance))
				Func(*Instance);
			return true;
		});
	}
}

bool UnrealMcpRqlWorldPartition::Matches(const FUnrealMcpUnloadedActor& Row, const FString& Key)
{
	return Row.Label.Equals(Key, ESearchCase::IgnoreCase) || Row.Name.Equals(Key, ESearchCase::IgnoreCase)
		|| Row.Guid.Equals(Key, ESearchCase::IgnoreCase) || Row.Package.Equals(Key, ESearchCase::IgnoreCase);
}

TArray<FUnrealMcpUnloadedActor> UnrealMcpRqlWorldPartition::Unloaded(UWorld* World, TArray<FGuid>* OutGuids)
{
	using namespace UnrealMcpRqlWorldPartitionLocal;
	TArray<FUnrealMcpUnloadedActor> Rows;
	ForEachUnloaded(World, [&](const FWorldPartitionActorDescInstance& Desc)
	{
		const FWorldPartitionActorDescInstance* Instance = &Desc;
		FUnrealMcpUnloadedActor& Row = Rows.AddDefaulted_GetRef();
		Row.Name = Instance->GetActorName().ToString();
		Row.Label = LabelOf(*Instance);
		Row.Class = ClassName(*Instance);
		Row.Guid = Instance->GetGuid().ToString();
		Row.Package = Instance->GetActorPackage().ToString();
		Row.Folder = Instance->GetFolderPath().IsNone() ? FString() : Instance->GetFolderPath().ToString();
		Row.Location = Instance->GetActorTransform().GetLocation();
		Row.Bounds = Instance->GetEditorBounds();
		Row.SpatiallyLoaded = Instance->GetIsSpatiallyLoaded();
		for (const FName& Layer : Instance->GetDataLayers())
			Row.DataLayers.Add(Layer.ToString());
		if (OutGuids)
			OutGuids->Add(Instance->GetGuid());
	});
	return Rows;
}

bool UnrealMcpRqlWorldPartition::Load(const TArray<FGuid>& Guids, FString& OutError)
{
	if (Guids.IsEmpty())
		return true;
	// UWorldPartitionBlueprintLibrary::LoadActors (what Python's load_actors calls) keeps them in its per-world actor-list
	// loader adapter, released with the world; the class is MinimalAPI (LoadActors is not exported), so it is called
	// through reflection. It loads into the editor world.
	UClass* Library = UWorldPartitionBlueprintLibrary::StaticClass();
	UFunction* LoadActors = Library->FindFunctionByName(TEXT("LoadActors"));
	if (!LoadActors)
	{
		OutError = TEXT("this engine has no WorldPartitionBlueprintLibrary.LoadActors: load the region in the World Partition window instead");
		return false;
	}
	struct { TArray<FGuid> InActorsToLoad; } Params{ Guids };
	Library->GetDefaultObject()->ProcessEvent(LoadActors, &Params);
	return true;
}

TArray<AActor*> UnrealMcpRqlWorldPartition::Actors(UWorld* World, const TArray<FGuid>& Guids)
{
	TArray<AActor*> Loaded;
	UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
	if (!Partition)
		return Loaded;
	for (const FGuid& Guid : Guids)
	{
		const FWorldPartitionActorDescInstance* Instance = Partition->GetActorDescInstance(Guid);
		AActor* Actor = Instance ? Instance->GetActor() : nullptr;
		if (IsValid(Actor))
			Loaded.Add(Actor);
	}
	return Loaded;
}

bool UnrealMcpRqlWorldPartition::UnloadedNote(UWorld* World, FString& OutMessage, FString& OutHint)
{
	// Runs after every query over the world's actors: count them all, describe only the few it names.
	using namespace UnrealMcpRqlWorldPartitionLocal;
	constexpr int32 Named = 5;
	int32 Count = 0;
	FString First;
	TArray<FString> Examples;
	ForEachUnloaded(World, [&](const FWorldPartitionActorDescInstance& Instance)
	{
		if (Count++ < Named)
		{
			Examples.Add(FString::Printf(TEXT("%s (%s)"), *LabelOf(Instance), *ClassName(Instance)));
			if (First.IsEmpty())
				First = LabelOf(Instance);
		}
	});
	if (Count == 0)
		return false;
	if (Count > Named)
		Examples.Add(FString::Printf(TEXT("+%d more"), Count - Named));
	OutMessage = FString::Printf(TEXT("%d World Partition actor(s) are not loaded (only loaded actors are listed): %s"),
		Count, *FString::Join(Examples, TEXT(", ")));
	OutHint = FString::Printf(TEXT("list them: rql-query unloadedActors | select Label Class Location Bounds; load them (changes nothing saved, "
		"rql-query): editor | call LoadActors([\"%s\"]) or editor | call LoadRegion([minX, minY], [maxX, maxY])"), *First);
	return true;
}

namespace UnrealMcpRqlWorldPartitionLocal
{
	/** The editor world's World Partition, or null after Fail (no editor world, or a level without World Partition). */
	UWorld* PartitionedEditorWorld()
	{
		UWorld* World = UnrealMcpRql::EditorWorld();
		if (!World)
			return UnrealMcpRql::Failed<UWorld*>(TEXT("no editor world"));
		if (!World->GetWorldPartition())
			return UnrealMcpRql::Failed<UWorld*>(FString::Printf(TEXT("the level open in the editor, \"%s\", has no World Partition: every actor "
				"is loaded (actors lists them all)"), *World->GetOutermost()->GetName()));
		return World;
	}

	/** Load @p Guids into @p World and return their actors, or an empty list after Fail. */
	TArray<AActor*> LoadAndCollect(UWorld* World, const TArray<FGuid>& Guids)
	{
		FString Error;
		if (!UnrealMcpRqlWorldPartition::Load(Guids, Error))
			return UnrealMcpRql::Failed<TArray<AActor*>>(Error);
		return UnrealMcpRqlWorldPartition::Actors(World, Guids);
	}
}

TArray<AActor*> UUnrealMcpRqlEditorApi::LoadActors(const TArray<FString>& Actors)
{
	using namespace UnrealMcpRqlWorldPartitionLocal;
	UWorld* World = PartitionedEditorWorld();
	if (!World)
		return {};
	TArray<FGuid> Guids;
	const TArray<FUnrealMcpUnloadedActor> Rows = UnrealMcpRqlWorldPartition::Unloaded(World, &Guids);
	// Every name must match before anything loads: a typo then loads nothing and says what there is.
	TArray<FGuid> ToLoad;
	TArray<AActor*> AlreadyLoaded;
	for (const FString& Key : Actors)
	{
		const FString Wanted = Key.TrimStartAndEnd();
		bool bMatched = false;
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (UnrealMcpRqlWorldPartition::Matches(Rows[Index], Wanted))
			{
				ToLoad.AddUnique(Guids[Index]);
				bMatched = true;
			}
		}
		if (bMatched)
			continue;
		// Already loaded: nothing to do, and the caller still gets the actor.
		if (AActor* Loaded = FUnrealMcpObjectRef::ResolveActor(Wanted))
		{
			AlreadyLoaded.AddUnique(Loaded);
			continue;
		}
		TArray<FString> Similar;
		for (const FUnrealMcpUnloadedActor& Row : Rows)
		{
			if (Similar.Num() < 5 && (Row.Label.Contains(Wanted) || Wanted.Contains(Row.Label)))
				Similar.Add(Row.Label);
		}
		return UnrealMcpRql::Failed<TArray<AActor*>>(FString::Printf(TEXT("no World Partition actor '%s' in \"%s\" (loaded or not)%s; nothing was "
			"loaded. List the unloaded ones: unloadedActors | select Label Class Location"), *Wanted, *World->GetOutermost()->GetName(),
			Similar.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (did you mean %s?)"), *FString::Join(Similar, TEXT(", ")))));
	}
	TArray<AActor*> Result = LoadAndCollect(World, ToLoad);
	for (AActor* Actor : AlreadyLoaded)
		Result.AddUnique(Actor);
	return Result;
}

TArray<AActor*> UUnrealMcpRqlEditorApi::LoadRegion(FVector2D Min, FVector2D Max)
{
	using namespace UnrealMcpRqlWorldPartitionLocal;
	if (Min.X > Max.X || Min.Y > Max.Y)
		return UnrealMcpRql::Failed<TArray<AActor*>>(FString::Printf(TEXT("Min (%g, %g) must not exceed Max (%g, %g) on either axis"), Min.X, Min.Y, Max.X, Max.Y));
	UWorld* World = PartitionedEditorWorld();
	if (!World)
		return {};
	TArray<FGuid> Guids;
	const TArray<FUnrealMcpUnloadedActor> Rows = UnrealMcpRqlWorldPartition::Unloaded(World, &Guids);
	const FBox2D Region(Min, Max);
	TArray<FGuid> ToLoad;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FBox& Bounds = Rows[Index].Bounds;
		// A descriptor without bounds (an actor with no primitive) is placed at its location.
		const FBox2D Footprint = Bounds.IsValid ? FBox2D(FVector2D(Bounds.Min), FVector2D(Bounds.Max))
			: FBox2D(FVector2D(Rows[Index].Location), FVector2D(Rows[Index].Location));
		if (Region.Intersect(Footprint))
			ToLoad.Add(Guids[Index]);
	}
	return LoadAndCollect(World, ToLoad);
}

void UnrealMcpRqlAddWorldPartition(FUnrealMcpRqlSession& Session)
{
	FUnrealMcpRqlTypes* Types = &Session.Types();
	UScriptStruct* RowStruct = FUnrealMcpUnloadedActor::StaticStruct();
	Session.AddSource(TEXT("unloadedActors"), Types->ForStruct(RowStruct), /*bList*/ true,
		TEXT("the World Partition actors of the open level that are not loaded, so actors does not list them (Label Name Class Guid "
			"Package Folder Location Bounds SpatiallyLoaded DataLayers); empty without World Partition. Load them with editor | call "
			"LoadActors([\"Lake\"]) or LoadRegion([minX, minY], [maxX, maxY]) (rql-query; changes nothing saved)"),
		[Types, RowStruct]()
	{
		std::vector<rql::Value> Rows;
		for (const FUnrealMcpUnloadedActor& Row : UnrealMcpRqlWorldPartition::Unloaded(UnrealMcpRql::EditorWorld()))
			Rows.push_back(Types->WrapStruct(RowStruct, &Row));
		return rql::Value::list_of(std::move(Rows));
	});
}
