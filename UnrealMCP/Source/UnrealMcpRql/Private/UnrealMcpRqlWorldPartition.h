// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpRqlEditorApi.h"

class UWorld;

/**
 * World Partition actors that are not loaded (docs/rql.md "World Partition"). In a World Partition level the editor holds
 * only the actors of its loaded regions; `actors` lists those, so the rest is invisible to a query. These helpers find the
 * rest through the level's actor descriptors and load them as the World Partition window does.
 */
namespace UnrealMcpRqlWorldPartition
{
	/**
	 * The actors of @p World's World Partition that are not loaded, in descriptor order: not runtime-only (HLODs), not
	 * deleted in this session (a deleted actor keeps its descriptor until the level is saved). Empty for a level without
	 * World Partition. @p OutGuids, when given, receives their GUIDs in the same order.
	 */
	TArray<FUnrealMcpUnloadedActor> Unloaded(UWorld* World, TArray<FGuid>* OutGuids = nullptr);

	/** @p Key names @p Row: its label, name, GUID or package, case-insensitively. */
	bool Matches(const FUnrealMcpUnloadedActor& Row, const FString& Key);

	/**
	 * Load the actors @p Guids of the editor world's World Partition through the editor's actor-list loader
	 * (UWorldPartitionBlueprintLibrary::LoadActors: no saved change, no undo step; they stay loaded until the world goes).
	 * False with @p OutError when this engine has no such loader.
	 */
	bool Load(const TArray<FGuid>& Guids, FString& OutError);

	/** The loaded actors of @p Guids in @p World (after Load), in order; GUIDs whose actor did not load are skipped. */
	TArray<AActor*> Actors(UWorld* World, const TArray<FGuid>& Guids);

	/**
	 * The `world-partition-unloaded` warning a query over the editor world's actors gets while some are not loaded: how
	 * many, a few of them, and how to list and load them. False when every actor is loaded (or no World Partition).
	 */
	bool UnloadedNote(UWorld* World, FString& OutMessage, FString& OutHint);
}
