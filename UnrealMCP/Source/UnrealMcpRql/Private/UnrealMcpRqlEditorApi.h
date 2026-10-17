// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealMcpRqlEditorApi.generated.h"

class AActor;
class ULevel;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UWorld;

/**
 * The `editor` RQL source (type "Editor"): editor operations that have no owner object, e.g.
 * `editor | call SpawnActor("PointLight", "Lamp", "(0, 0, 300)")`. Every UFUNCTION here is callable from rql-mutate
 * (the comment above it is the description agents see). They are BlueprintCallable only because UHT records C++ default
 * arguments for BlueprintCallable functions alone; BlueprintInternalUseOnly keeps them out of Blueprint menus. The
 * members IsPlaying, IsPaused, Selection, World and
 * CurrentLevel are added by the core RQL module. Failures go through UnrealMcpRql::Fail, so the call reports them.
 */
UCLASS(Transient)
class UUnrealMcpRqlEditorApi : public UObject
{
	GENERATED_BODY()

public:
	/** spawn an actor in the current level from a class name ("PointLight"), class path or Blueprint asset path; Label, Location, Rotation and Parent are optional; returns the actor */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	AActor* SpawnActor(const FString& ClassName, const FString& Label = TEXT(""), FVector Location = FVector::ZeroVector,
		FRotator Rotation = FRotator::ZeroRotator, AActor* Parent = nullptr);

	/** open a level by asset path ("/Game/Maps/Arena"), replacing the editor world (unsaved edits of the current level are lost); returns the world */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	UWorld* OpenLevel(const FString& Path);

	/** create a new empty level (or a copy of Template) and make it the editor world; saved to Path when given; returns the world */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	UWorld* NewLevel(const FString& Path = TEXT(""), const FString& Template = TEXT(""));

	/** save the current level in place, or the persistent level to Path ("/Game/Maps/Copy") */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	bool SaveLevel(const FString& Path = TEXT(""));

	/** save every unsaved level and asset */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	bool SaveAll();

	/** remove a loaded streaming sublevel from the world (levels | where Map == "Sub" gives the level) */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	bool UnloadSublevel(ULevel* Level);

	/** copy an asset ("/Game/A" to "/Game/B"); returns the copy */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	UObject* CopyAsset(const FString& Source, const FString& Destination);

	/** move or rename an asset */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	bool MoveAsset(const FString& Source, const FString& Destination);

	/** delete an asset; refuses when other packages reference it unless bForce */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	bool DeleteAsset(const FString& Path, bool bForce = false);

	/** create a content folder ("/Game/Props") */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	bool CreateFolder(const FString& Path);

	/** rescan a content path so files added on disk appear (bForce: full rescan) */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	bool RefreshAssets(const FString& Path = TEXT("/Game"), bool bForce = false);

	/** create a material instance at Path from a parent material (unreal.asset(path) or its path); returns it */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	UMaterialInstanceConstant* CreateMaterialInstance(const FString& Path, UMaterialInterface* Parent);

	/** import a file (FBX, PNG, …) from disk into a content folder; Name renames the asset; returns the imported assets */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	TArray<UObject*> ImportAsset(const FString& File, const FString& Destination, const FString& Name = TEXT(""),
		bool bReplaceExisting = false);

	/** save one asset (or level) package to disk */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	bool SaveAsset(UObject* Asset);

	/** clear the captured editor log (the console source); returns how many entries were removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	int32 ClearConsole();
};
