// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealMcpRqlEditorApi.generated.h"

class AActor;
class ULevel;
class UDataTable;
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
	/** spawn an actor in the current level from a class name ("PointLight"), class path, Blueprint name or path, or an asset to place (a StaticMesh path gives a StaticMeshActor showing it); Label, Location, Rotation (final; omitted keeps the class default, a SpotLight points down) and Parent are optional; returns the actor */
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
	void SaveLevel(const FString& Path = TEXT(""));

	/** save every unsaved level and asset */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void SaveAll();

	/** add a saved level ("/Game/Maps/Sub_A") to the editor world as a streaming sublevel (Streaming: "Dynamic", or "AlwaysLoaded"); returns the loaded level (not undoable: UnloadSublevel removes it; the current level stays) */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	ULevel* AddSublevel(const FString& Path, const FString& Streaming = TEXT("Dynamic"));

	/** remove a loaded streaming sublevel from the world (its map name "Sub_A", or levels | where Map == "Sub_A") */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void UnloadSublevel(ULevel* Level);

	/** copy an asset ("/Game/A" to "/Game/B"); returns the copy */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	UObject* CopyAsset(const FString& Source, const FString& Destination);

	/** move or rename an asset */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void MoveAsset(const FString& Source, const FString& Destination);

	/** delete an asset; refuses when other packages reference it unless bForce */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void DeleteAsset(const FString& Path, bool bForce = false);

	/** create a content folder ("/Game/Props") */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void CreateFolder(const FString& Path);

	/** rescan a content path so files added on disk appear (bForce: full rescan) */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void RefreshAssets(const FString& Path = TEXT("/Game"), bool bForce = false);

	/** create an asset of a class ("InputAction", "Material", "DataAsset subclass", ...) at Path ("/Game/Input/IA_Jump") with the class's editor factory when it has one; returns it */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	UObject* CreateAsset(const FString& Path, const FString& ClassName);

	/** create a material instance at Path from a parent material (unreal.asset(path) or its path); returns it */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	UMaterialInstanceConstant* CreateMaterialInstance(const FString& Path, UMaterialInterface* Parent);

	/** import a file (FBX, PNG, …) from disk into a content folder; Name renames the asset; returns the imported assets */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	TArray<UObject*> ImportAsset(const FString& File, const FString& Destination, const FString& Name = TEXT(""),
		bool bReplaceExisting = false);

	/** save one asset (or level) package to disk */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void SaveAsset(UObject* Asset);

	/** clear the captured editor log (the console source); returns how many entries were removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	int32 ClearConsole();

	/** add a gameplay tag ("Ability.Fire") to the project's tag list (Config/DefaultGameplayTags.ini, or Source: another tag ini such as "Abilities.ini"); written to disk at once, not undoable */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void AddGameplayTag(const FString& Name, const FString& Comment = TEXT(""), const FString& Source = TEXT(""));

	/** remove a gameplay tag from its tag ini (refused while assets reference it, or when C++ / a DataTable declares it); written at once, not undoable */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void RemoveGameplayTag(const FString& Name);

	/** rename a gameplay tag (and its children) in its tag ini, leaving a redirector from the old name; written at once, not undoable */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	void RenameGameplayTag(const FString& Name, const FString& NewName);

	/** create a DataTable at Path whose rows are RowStruct (a struct name such as "GameplayTagTableRow", or a user-defined struct asset path); returns it, unsaved */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP|RQL", meta = (BlueprintInternalUseOnly = "true"))
	UDataTable* CreateDataTable(const FString& Path, const FString& RowStruct);
};

/** Why AttachToActor left @p Actor unattached from @p Parent, as an agent-facing ": reason" suffix ("" when unknown). */
FString UnrealMcpRqlAttachFailure(const AActor* Actor, const AActor* Parent);
