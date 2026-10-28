// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlEditorApi.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpPinTypes.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EditorLevelUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "LevelUtils.h"
#include "Components/ActorComponent.h"
#include "Components/BrushComponent.h"
#include "Components/SceneComponent.h"
#include "DataTableEditorUtils.h"
#include "ActorFactories/ActorFactory.h"
#include "AssetSelection.h"
#include "Builders/CubeBuilder.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Factories/DataTableFactory.h"
#include "Factories/Factory.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "HAL/IConsoleManager.h"
#include "IAssetTools.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ITransaction.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"


namespace UnrealMcpRqlEditorApiLocal
{
	IAssetTools& AssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	}

	using UnrealMcpRql::Failed;

	/** The tail of every spawn: the final rotation, a root for root-less classes, the label and the parent. */
	AActor* FinishSpawnedActor(AActor* Actor, const FVector& Location, const FRotator& Rotation, const FString& Label, AActor* Parent)
	{
		UWorld* World = Actor->GetWorld();
		// SpawnActor composes the class's default root rotation (a SpotLight's -90 pitch) with Rotation. A rotation the
		// caller gave is the final one; none keeps the class default (a SpotLight points down).
		if (!Rotation.IsZero() && Actor->GetRootComponent())
		{
			Actor->SetActorRotation(Rotation, ETeleportType::TeleportPhysics);
			Actor->PostEditMove(/*bFinished*/ true); // construction scripts ran at the composed rotation
		}
		// A class without a root component (a plain Actor) has no transform: the location would be dropped and nothing
		// could attach to it. Give it the scene root the editor's Empty Actor gets (UActorFactoryEmptyActor).
		if (!Actor->GetRootComponent() && !UnrealMcpRql::AddComponent(Actor, USceneComponent::StaticClass(),
			USceneComponent::GetDefaultSceneRootVariableName().ToString(), [&Location, &Rotation](UActorComponent* Component)
			{
				USceneComponent* Root = CastChecked<USceneComponent>(Component);
				Root->Mobility = EComponentMobility::Movable;
				Root->bVisualizeComponent = true;
				Root->SetWorldLocationAndRotation(Location, Rotation);
			}))
		{
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
			return nullptr;
		}
		if (!Label.IsEmpty())
			FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
		if (Parent)
		{
			Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
			if (Actor->GetAttachParentActor() != Parent)
			{
				const FString Reason = UnrealMcpRqlAttachFailure(Actor, Parent);
				World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
				return Failed<AActor*>(FString::Printf(TEXT("could not attach to %s%s"), *Parent->GetActorLabel(), *Reason));
			}
		}
		return Actor;
	}

	/**
	 * The generated class of the one actor Blueprint asset named @p Name ("BP_Door"); null when there is none, or when
	 * several have the name (their paths in @p OutAmbiguous).
	 */
	UClass* BlueprintClassByName(const FString& Name, TArray<FString>& OutAmbiguous)
	{
		if (Name.IsEmpty() || Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")))
			return nullptr;
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Found;
		Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses*/ true);
		UClass* Match = nullptr;
		TArray<FString> Paths;
		for (const FAssetData& Data : Found)
		{
			if (!Data.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase))
				continue;
			const UBlueprint* Blueprint = Cast<UBlueprint>(Data.GetAsset());
			UClass* Class = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
			if (!Class || !Class->IsChildOf(AActor::StaticClass()))
				continue;
			Paths.Add(Data.PackageName.ToString());
			Match = Class;
		}
		if (Paths.Num() > 1)
		{
			OutAmbiguous = MoveTemp(Paths);
			return nullptr;
		}
		return Match;
	}

	/** Writes stay inside project / plugin content: /Engine, /Script and /Temp are refused. */
	bool IsWritableRoot(const FString& Path)
	{
		for (const TCHAR* Root : { TEXT("/Engine"), TEXT("/Script"), TEXT("/Temp") })
		{
			if (Path.Equals(Root) || Path.StartsWith(FString(Root) + TEXT("/")))
				return false;
		}
		return true;
	}

	/** Long package name of an asset path ("/Game/Maps/Arena" or its object path); false + reason for a malformed path. */
	bool ToPackageName(const FString& Path, FString& OutPackage, FString& OutError)
	{
		OutPackage = Path.TrimStartAndEnd();
		if (FPackageName::IsValidObjectPath(OutPackage))
			OutPackage = FPackageName::ObjectPathToPackageName(OutPackage);
		if (!FPackageName::IsValidLongPackageName(OutPackage))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid asset path (expected e.g. '/Game/Maps/Arena')"), *Path);
			return false;
		}
		return true;
	}

	/**
	 * The folder and name of a new asset at @p Path: a valid, writable (project / plugin) package path where no asset is
	 * yet. False after reporting why (UnrealMcpRql::Fail).
	 */
	bool ValidateNewAssetPath(const FString& Path, FString& OutFolder, FString& OutName)
	{
		FString Package, Error;
		if (!ToPackageName(Path, Package, Error) || !IsWritableRoot(Package))
			return Failed<bool>(FString::Printf(TEXT("'%s' is not a writable asset path (use e.g. /Game/Folder/Name)"), *Path));
		if (UEditorAssetLibrary::DoesAssetExist(Package))
			return Failed<bool>(FString::Printf(TEXT("'%s' already exists"), *Package));
		OutFolder = FPackageName::GetLongPackagePath(Package);
		OutName = FPackageName::GetShortName(Package);
		return true;
	}

	/** On-disk .umap of an existing level package; false + reason otherwise (a missing map would log an engine error). */
	bool LevelFile(const FString& Path, FString& OutFile, FString& OutError)
	{
		FString Package;
		if (!ToPackageName(Path, Package, OutError))
			return false;
		if (!FPackageName::DoesPackageExist(Package, &OutFile))
		{
			UPackage* Loaded = FindPackage(nullptr, *Package);
			OutError = Loaded && UWorld::FindWorldInPackage(Loaded)
				? FString::Printf(TEXT("the level '%s' is not saved yet: editor | call SaveAsset(\"%s\") first"), *Package, *Package)
				: FString::Printf(TEXT("no level at '%s'"), *Package);
			return false;
		}
		if (!OutFile.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("'%s' is not a level"), *Package);
			return false;
		}
		return true;
	}
}

using namespace UnrealMcpRqlEditorApiLocal;

AActor* UnrealMcpRql::SpawnActor(UClass* Class, const FVector& Location, const FRotator& Rotation, const FString& Label, AActor* Parent)
{
	UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
	if (!World)
		return Failed<AActor*>(TEXT("no editor world"));
	if (!Class || !Class->IsChildOf(AActor::StaticClass()))
		return Failed<AActor*>(TEXT("not an actor class"));
	if (Class->HasAnyClassFlags(CLASS_Abstract))
		return Failed<AActor*>(FString::Printf(TEXT("'%s' is abstract"), *Class->GetName()));
	// UWorld::SpawnActor rather than the editor actor subsystem: no viewport snapping (headless-safe). The open RQL
	// transaction records the transactional actor, so Ctrl+Z removes it.
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transactional;
	AActor* Actor = World->SpawnActor<AActor>(Class, Location, Rotation, Params);
	if (!Actor)
		return Failed<AActor*>(FString::Printf(TEXT("could not spawn %s"), *Class->GetName()));
	// A volume is its brush: spawned bare it has no shape, so a NavMeshBoundsVolume / TriggerVolume / PostProcessVolume
	// would bound nothing. Give it the editor's default box (200 cm, as the Place Actors panel does); Scale sizes it.
	if (AVolume* Volume = Cast<AVolume>(Actor); Volume && !Volume->Brush)
		UnrealMcpRqlBuildVolumeBox(Volume, FVector(100.0));
	return FinishSpawnedActor(Actor, Location, Rotation, Label, Parent);
}

void UnrealMcpRqlBuildVolumeBox(AVolume* Volume, const FVector& HalfExtent)
{
	// The editor's box brush (what the Brush Settings' Cube builder makes), centred on the actor.
	UCubeBuilder* Builder = NewObject<UCubeBuilder>();
	FGCObjectScopeGuard BuilderGuard(Builder);
	Builder->X = static_cast<float>(2.0 * HalfExtent.X);
	Builder->Y = static_cast<float>(2.0 * HalfExtent.Y);
	Builder->Z = static_cast<float>(2.0 * HalfExtent.Z);
	if (UBrushComponent* Component = Volume->GetBrushComponent())
		Component->Modify(); // the brush it points at is replaced: undo restores the old one
	UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
}

FString UnrealMcpRqlAttachFailure(const AActor* Actor, const AActor* Parent)
{
	const USceneComponent* Root = Actor ? Actor->GetRootComponent() : nullptr;
	const USceneComponent* ParentRoot = Parent ? Parent->GetRootComponent() : nullptr;
	if (!Root)
		return TEXT(": it has no root component (no transform)");
	if (!ParentRoot)
		return FString::Printf(TEXT(": %s has no root component (no transform); give it one with AddComponent(\"SceneComponent\", \"Root\")"),
			*Parent->GetActorLabel());
	// The engine refuses a Static component under a non-Static one (it could not follow its parent's movement).
	if (Root->Mobility == EComponentMobility::Static && ParentRoot->Mobility != EComponentMobility::Static)
		return FString::Printf(TEXT(": a Static %s cannot attach to a %s parent (set the parent's root Mobility to Static, or this actor's to Movable)"),
			*Actor->GetClass()->GetName(), ParentRoot->Mobility == EComponentMobility::Movable ? TEXT("Movable") : TEXT("Stationary"));
	return FString();
}

AActor* UUnrealMcpRqlEditorApi::SpawnActor(const FString& ClassName, const FString& Label, FVector Location, FRotator Rotation, AActor* Parent)
{
	if (UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassName))
	{
		if (Class->IsChildOf(AActor::StaticClass()))
			return UnrealMcpRql::SpawnActor(Class, Location, Rotation, Label, Parent);
	}
	else
	{
		TArray<FString> Ambiguous;
		if (UClass* Blueprint = BlueprintClassByName(ClassName, Ambiguous))
			return UnrealMcpRql::SpawnActor(Blueprint, Location, Rotation, Label, Parent);
		if (Ambiguous.Num() > 0)
			return Failed<AActor*>(FString::Printf(TEXT("several Blueprints are named '%s': use a path (%s)"), *ClassName,
				*FString::Join(Ambiguous, TEXT(", "))));
	}
	// An asset the editor can place (a mesh dragged into the viewport becomes a StaticMeshActor showing it).
	if (ClassName.StartsWith(TEXT("/")))
	{
		if (UObject* Asset = UnrealMcpRql::LoadObject(UObject::StaticClass(), ClassName); Asset && !Asset->IsA<UClass>() && !Asset->IsA<UBlueprint>())
		{
			UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
			UActorFactory* Factory = FActorFactoryAssetProxy::GetFactoryForAssetObject(Asset);
			if (!World || !Factory)
				return Failed<AActor*>(FString::Printf(TEXT("'%s' is a %s, which cannot be placed in a level"), *ClassName, *Asset->GetClass()->GetName()));
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			AActor* Actor = Factory->CreateActor(Asset, World->GetCurrentLevel(), FTransform(Rotation, Location), Params);
			if (!Actor)
				return Failed<AActor*>(FString::Printf(TEXT("could not place '%s'"), *ClassName));
			return FinishSpawnedActor(Actor, Location, Rotation, Label, Parent);
		}
	}
	return Failed<AActor*>(FString::Printf(TEXT("'%s' is not an actor class (try a class name such as \"PointLight\", a Blueprint "
		"name or path, or an asset to place such as a StaticMesh path)"), *ClassName));
}

UWorld* UUnrealMcpRqlEditorApi::OpenLevel(const FString& Path)
{
	if (!GEditor)
		return Failed<UWorld*>(TEXT("no editor"));
	FString File, Error;
	if (!LevelFile(Path, File, Error))
		return Failed<UWorld*>(Error);
	UWorld* Opened = UEditorLoadingAndSavingUtils::LoadMap(File);
	return Opened ? Opened : Failed<UWorld*>(FString::Printf(TEXT("could not open '%s'"), *Path));
}

UWorld* UUnrealMcpRqlEditorApi::NewLevel(const FString& Path, const FString& Template)
{
	if (!GEditor)
		return Failed<UWorld*>(TEXT("no editor"));
	// Validate everything before the current world is replaced.
	FString Package, Error;
	if (!Path.IsEmpty())
	{
		if (!ToPackageName(Path, Package, Error))
			return Failed<UWorld*>(Error);
		if (!IsWritableRoot(Package))
			return Failed<UWorld*>(FString::Printf(TEXT("'%s' is not a writable level path (use e.g. /Game/Maps/Name)"), *Package));
		FString Existing;
		if (FPackageName::DoesPackageExist(Package, &Existing) && !Existing.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase))
			return Failed<UWorld*>(FString::Printf(TEXT("'%s' exists and is not a level"), *Package));
	}
	UWorld* World = nullptr;
	if (!Template.IsEmpty())
	{
		FString TemplateFile;
		if (!LevelFile(Template, TemplateFile, Error))
			return Failed<UWorld*>(Error);
		World = UEditorLoadingAndSavingUtils::NewMapFromTemplate(TemplateFile, /*bSaveExistingMap*/ false);
	}
	else
	{
		World = GEditor->NewMap(); // the engine automation's headless-safe in-memory world
	}
	if (!World)
		return Failed<UWorld*>(TEXT("could not create the level"));
	if (!Package.IsEmpty() && !UEditorLoadingAndSavingUtils::SaveMap(World, Package))
		UnrealMcpRql::Fail(FString::Printf(TEXT("created the level but could not save it to '%s'"), *Package));
	return World;
}

void UUnrealMcpRqlEditorApi::SaveLevel(const FString& Path)
{
	UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
	if (!World)
		return Failed(TEXT("no editor world"));
	// As the editor scripting libraries do: in an -unattended editor PromptForCheckoutAndSave cancels every save unless a
	// script is marked as running, and in a windowed one the mark keeps the save free of prompts and failure dialogs.
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
	if (!Path.IsEmpty())
	{
		FString Package, Error;
		if (!ToPackageName(Path, Package, Error))
			return Failed(Error);
		if (!IsWritableRoot(Package))
			return Failed(FString::Printf(TEXT("'%s' is not a writable level path (use e.g. /Game/Maps/Name)"), *Package));
		if (!UEditorLoadingAndSavingUtils::SaveMap(World, Package))
			Failed(FString::Printf(TEXT("could not save the level to '%s'"), *Package));
		return;
	}
	// A never-saved level would open the interactive Save-As dialog (modal UI on the game thread): refuse instead.
	ULevel* Current = World->GetCurrentLevel();
	UPackage* LevelPackage = Current ? Current->GetPackage() : nullptr;
	if (!LevelPackage || !FPackageName::DoesPackageExist(LevelPackage->GetName()))
		return Failed(TEXT("the current level was never saved: call SaveLevel(\"/Game/Maps/Name\")"));
	// SaveCurrentLevel reports only a cancel as failure (a package that failed to save, e.g. a read-only file whose
	// checkout failed, still returns true), so a package left dirty is the failure signal.
	TArray<UPackage*> Dirty;
	if (LevelPackage->IsDirty())
		Dirty.Add(LevelPackage);
	for (UPackage* External : Current->GetLoadedExternalObjectPackages())
	{
		if (External->IsDirty())
			Dirty.Add(External);
	}
	const bool bSaved = UEditorLoadingAndSavingUtils::SaveCurrentLevel();
	TArray<FString> Unsaved;
	for (UPackage* Package : Dirty)
	{
		if (Package->IsDirty() && !UPackage::IsEmptyPackage(Package))
			Unsaved.Add(Package->GetName());
	}
	if (!bSaved || Unsaved.Num() > 0)
		Failed(FString::Printf(TEXT("the current level could not be saved (checkout or write failure)%s%s"),
			Unsaved.Num() > 0 ? TEXT(": ") : TEXT(""), *FString::Join(Unsaved, TEXT(", "))));
}

void UUnrealMcpRqlEditorApi::SaveAll()
{
	// Never-saved maps would prompt for a location: SaveDirtyPackages skips the prompt when it cannot ask.
	if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true))
		Failed(TEXT("some packages could not be saved (never-saved levels need SaveLevel(path))"));
}

ULevel* UUnrealMcpRqlEditorApi::AddSublevel(const FString& Path, const FString& Streaming)
{
	UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
	if (!World)
		return Failed<ULevel*>(TEXT("no editor world"));
	if (World->IsPartitionedWorld())
		return Failed<ULevel*>(TEXT("a World Partition world streams by cells, not sublevels (use data layers, or a level without World Partition)"));
	const FString Package = FPackageName::ObjectPathToPackageName(Path.TrimStartAndEnd());
	FString File;
	if (!FPackageName::IsValidLongPackageName(Package) || !FPackageName::DoesPackageExist(Package, &File)
		|| !FPaths::GetExtension(File, /*bIncludeDot*/ true).Equals(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase))
		return Failed<ULevel*>(FString::Printf(TEXT("no saved level (map) at '%s' (save it first: editor | call SaveLevel())"), *Path));
	if (Package == World->GetOutermost()->GetName())
		return Failed<ULevel*>(TEXT("a level cannot be its own sublevel"));
	// The engine asks with a modal dialog (not skipped when unattended) when the world already streams that package.
	if (FLevelUtils::FindStreamingLevel(World, *Package))
		return Failed<ULevel*>(FString::Printf(TEXT("'%s' is already a sublevel of this world"), *Package));
	const FString Kind = Streaming.TrimStartAndEnd();
	UClass* StreamingClass = Kind.IsEmpty() || Kind.Equals(TEXT("Dynamic"), ESearchCase::IgnoreCase) ? ULevelStreamingDynamic::StaticClass()
		: Kind.Equals(TEXT("AlwaysLoaded"), ESearchCase::IgnoreCase) ? ULevelStreamingAlwaysLoaded::StaticClass() : nullptr;
	if (!StreamingClass)
		return Failed<ULevel*>(FString::Printf(TEXT("unknown streaming '%s' (Dynamic or AlwaysLoaded)"), *Streaming));
	// AddLevelToWorld makes the new level current: keep the current level where it was (spawns keep their target).
	ULevel* Current = World->GetCurrentLevel();
	ULevelStreaming* Added = UEditorLevelUtils::AddLevelToWorld(World, *Package, StreamingClass);
	ULevel* Level = Added ? Added->GetLoadedLevel() : nullptr;
	if (!Level)
	{
		if (Added)
			UEditorLevelUtils::RemoveInvalidLevelFromWorld(Added); // no broken entry left behind
		return Failed<ULevel*>(FString::Printf(TEXT("could not load '%s' as a sublevel"), *Package));
	}
	if (Current && Current != World->GetCurrentLevel())
		UEditorLevelUtils::MakeLevelCurrent(Current, /*bEvenIfLocked*/ false);
	return Level;
}

void UUnrealMcpRqlEditorApi::UnloadSublevel(ULevel* Level)
{
	UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
	if (!World)
		return Failed(TEXT("no editor world"));
	if (Level == World->PersistentLevel)
		return Failed(TEXT("the persistent level cannot be unloaded"));
	if (!UEditorLevelUtils::RemoveLevelFromWorld(Level))
		Failed(TEXT("could not unload the sublevel"));
}

UObject* UUnrealMcpRqlEditorApi::CopyAsset(const FString& Source, const FString& Destination)
{
	FString Folder, Name;
	if (!UEditorAssetLibrary::DoesAssetExist(Source))
		return Failed<UObject*>(FString::Printf(TEXT("no asset at '%s'"), *Source));
	if (!ValidateNewAssetPath(Destination, Folder, Name))
		return nullptr;
	UObject* Copy = UEditorAssetLibrary::DuplicateAsset(Source, Destination);
	if (!Copy)
		return Failed<UObject*>(FString::Printf(TEXT("could not copy '%s'"), *Source));
	// A level opens from its file only (OpenLevel, AddSublevel): a copied level is saved at once, as a new map is.
	if (Copy->IsA<UWorld>() && !UEditorAssetLibrary::SaveLoadedAsset(Copy, /*bOnlyIfIsDirty*/ false))
		return Failed<UObject*>(FString::Printf(TEXT("copied the level to '%s' but could not save it"), *Destination));
	return Copy;
}

void UUnrealMcpRqlEditorApi::MoveAsset(const FString& Source, const FString& Destination)
{
	FString Folder, Name;
	if (!UEditorAssetLibrary::DoesAssetExist(Source))
		return Failed(FString::Printf(TEXT("no asset at '%s'"), *Source));
	if (!IsWritableRoot(Source))
		return Failed(FString::Printf(TEXT("'%s' is engine content"), *Source));
	if (ValidateNewAssetPath(Destination, Folder, Name) && !UEditorAssetLibrary::RenameAsset(Source, Destination))
		Failed(FString::Printf(TEXT("could not move '%s'"), *Source));
}

void UUnrealMcpRqlEditorApi::DeleteAsset(const FString& Path, bool bForce)
{
	if (!UEditorAssetLibrary::DoesAssetExist(Path))
		return Failed(FString::Printf(TEXT("no asset at '%s'"), *Path));
	if (!IsWritableRoot(Path))
		return Failed(FString::Printf(TEXT("'%s' is engine content"), *Path));
	if (!bForce)
	{
		const FAssetData Data = UEditorAssetLibrary::FindAssetData(Path);
		TArray<FString> Referencers = UEditorAssetLibrary::FindPackageReferencersForAsset(Path);
		Referencers.Remove(Data.PackageName.ToString());
		// The asset registry knows saved references only: an unsaved asset that uses this one (a new material
		// instance's parent) shows up in memory alone. The memory scan walks every object: only when needed.
		UObject* Loaded = Referencers.Num() == 0 ? Data.FastGetAsset(/*bLoad*/ false) : nullptr;
		if (Loaded)
		{
			bool bReferenced = false, bReferencedByUndo = false;
			FReferencerInformationList Memory;
			ObjectTools::GatherObjectReferencersForDeletion(Loaded, bReferenced, bReferencedByUndo, &Memory);
			// bReferenced: something reachable other than the undo buffer holds it (the list also has garbage).
			if (!bReferenced)
				Memory.ExternalReferences.Reset();
			for (const FReferencerInformation& Info : Memory.ExternalReferences)
			{
				const UPackage* Package = Info.Referencer ? Info.Referencer->GetPackage() : nullptr;
				if (Package && Package != Loaded->GetPackage() && Package != GetTransientPackage() && !Package->HasAnyFlags(RF_Transient))
					Referencers.AddUnique(Package->GetName());
			}
		}
		if (Referencers.Num() > 0)
		{
			const TArray<FString> Shown(Referencers.GetData(), FMath::Min(Referencers.Num(), 5));
			return Failed(FString::Printf(TEXT("referenced by %d package(s) (%s); pass bForce = true to delete anyway"),
				Referencers.Num(), *FString::Join(Shown, TEXT(", "))));
		}
	}
	if (!UEditorAssetLibrary::DeleteAsset(Path))
		Failed(FString::Printf(TEXT("could not delete '%s'"), *Path));
}

void UUnrealMcpRqlEditorApi::CreateFolder(const FString& Path)
{
	if (!IsWritableRoot(Path) || !Path.StartsWith(TEXT("/")))
		return Failed(FString::Printf(TEXT("'%s' is not a writable content path (e.g. /Game/Props)"), *Path));
	if (!UEditorAssetLibrary::DoesDirectoryExist(Path) && !UEditorAssetLibrary::MakeDirectory(Path))
		Failed(FString::Printf(TEXT("could not create '%s'"), *Path));
}

void UUnrealMcpRqlEditorApi::RefreshAssets(const FString& Path, bool bForce)
{
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
		.ScanPathsSynchronous({ Path.IsEmpty() ? FString(TEXT("/Game")) : Path }, bForce);
}

UObject* UnrealMcpRql::CreateAsset(UClass* Class, const FString& Path)
{
	FString Folder, Name;
	if (!Class || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)
		|| Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass()))
		return Failed<UObject*>(FString::Printf(TEXT("'%s' is not an asset class (actors are spawned: SpawnActor)"),
			Class ? *Class->GetName() : TEXT("None")));
	if (Class->HasAnyClassFlags(CLASS_Abstract))
		return Failed<UObject*>(FString::Printf(TEXT("'%s' is abstract: create one of its subclasses (unreal.classes(\"%s\") lists them)"),
			*Class->GetName(), *Class->GetName()));
	if (!ValidateNewAssetPath(Path, Folder, Name))
		return nullptr;
	// The factory of the Content Browser's "New" menu when one creates this class (materials, input actions, data
	// tables, ...): it sets the asset up as that menu does. Otherwise a plain object in a new package.
	UClass* FactoryClass = nullptr;
	for (const UFactory* Default : AssetTools().GetNewAssetFactories())
	{
		if (Default && Default->GetSupportedClass() == Class)
		{
			FactoryClass = Default->GetClass();
			break;
		}
	}
	UObject* Created = nullptr;
	if (FactoryClass)
	{
		UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
		FGCObjectScopeGuard FactoryGuard(Factory);
		Created = AssetTools().CreateAsset(Name, Folder, Class, Factory);
	}
	else if (UPackage* Package = CreatePackage(*(Folder / Name)))
	{
		Created = NewObject<UObject>(Package, Class, *Name, RF_Public | RF_Standalone | RF_Transactional);
		if (Created)
		{
			FAssetRegistryModule::AssetCreated(Created);
			Package->MarkPackageDirty();
		}
	}
	return Created ? Created : Failed<UObject*>(FString::Printf(TEXT("could not create a %s at '%s'"), *Class->GetName(), *Path));
}

UObject* UUnrealMcpRqlEditorApi::CreateAsset(const FString& Path, const FString& ClassName)
{
	UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassName);
	if (!Class)
		return Failed<UObject*>(FString::Printf(TEXT("'%s' is not an asset class (actors are spawned: SpawnActor)"), *ClassName));
	return UnrealMcpRql::CreateAsset(Class, Path);
}

UMaterialInstanceConstant* UUnrealMcpRqlEditorApi::CreateMaterialInstance(const FString& Path, UMaterialInterface* Parent)
{
	FString Folder, Name;
	if (!ValidateNewAssetPath(Path, Folder, Name))
		return nullptr;
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	FGCObjectScopeGuard FactoryGuard(Factory);
	Factory->InitialParent = Parent;
	UMaterialInstanceConstant* Created = Cast<UMaterialInstanceConstant>(
		AssetTools().CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
	return Created ? Created : Failed<UMaterialInstanceConstant*>(FString::Printf(TEXT("could not create '%s'"), *Path));
}

TArray<UObject*> UUnrealMcpRqlEditorApi::ImportAsset(const FString& File, const FString& Destination, const FString& Name, bool bReplaceExisting)
{
	if (!FPaths::FileExists(File))
		return Failed<TArray<UObject*>>(FString::Printf(TEXT("no file '%s'"), *File));
	// The registry's package paths have no trailing slash: "/Game/Props/" would find no created assets.
	FString Folder = Destination;
	while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/")))
		Folder.LeftChopInline(1);
	if (!IsWritableRoot(Folder) || !Folder.StartsWith(TEXT("/")))
		return Failed<TArray<UObject*>>(FString::Printf(TEXT("'%s' is not a writable content folder (e.g. /Game/Imported)"), *Destination));
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	FGCObjectScopeGuard TaskGuard(Task);
	Task->Filename = File;
	Task->DestinationPath = Folder;
	if (!Name.IsEmpty())
		Task->DestinationName = Name;
	Task->bAutomated = true;
	Task->bReplaceExisting = bReplaceExisting;
	Task->bSave = false;
	Task->bAsync = false;

	// Interchange imports are asynchronous under the hood; the legacy factories import synchronously (as the removed
	// asset-import tool did), so the result is known when the call returns.
	IConsoleVariable* const Interchange = IConsoleManager::Get().FindConsoleVariable(TEXT("Interchange.FeatureFlags.Import.Enable"));
	const bool bInterchange = Interchange && Interchange->GetBool();
	if (bInterchange)
		Interchange->Set(false, ECVF_SetByCode);
	ON_SCOPE_EXIT
	{
		if (bInterchange)
			Interchange->Set(true, ECVF_SetByCode);
	};
	// The task lists only the primary asset (the mesh of an FBX): the materials, textures and skeleton the importer creates
	// next to it are the assets the destination folder gains, found by diffing the registry.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const auto AssetsUnder = [&Registry, &Folder]()
	{
		TArray<FAssetData> Found;
		Registry.GetAssetsByPath(FName(*Folder), Found, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ false);
		TSet<FSoftObjectPath> Paths;
		for (const FAssetData& Data : Found)
			Paths.Add(Data.GetSoftObjectPath());
		return Paths;
	};
	const TSet<FSoftObjectPath> Before = AssetsUnder();
	AssetTools().ImportAssetTasks({ Task });
	TArray<UObject*> Imported;
	for (UObject* Object : Task->GetObjects())
	{
		if (Object)
			Imported.AddUnique(Object);
	}
	TArray<FSoftObjectPath> Created = AssetsUnder().Difference(Before).Array();
	Created.Sort([](const FSoftObjectPath& A, const FSoftObjectPath& B) { return A.LexicalLess(B); });
	for (const FSoftObjectPath& Path : Created)
	{
		if (UObject* Object = Path.TryLoad())
			Imported.AddUnique(Object);
	}
	if (Imported.Num() == 0)
	{
		// An existing asset of the same name is skipped without bReplaceExisting: name that, not an empty import.
		const FString Existing = Folder / (Name.IsEmpty() ? ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(File)) : Name);
		if (!bReplaceExisting && UEditorAssetLibrary::DoesAssetExist(Existing))
			UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' already exists; pass bReplaceExisting = true to reimport over it, or a Name for a new asset"), *Existing));
		else
			UnrealMcpRql::Fail(FString::Printf(TEXT("the import of '%s' produced no assets"), *File));
	}
	return Imported;
}

void UUnrealMcpRqlEditorApi::SaveAsset(UObject* Asset)
{
	if (!UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/ false))
		Failed(FString::Printf(TEXT("could not save %s"), *Asset->GetPathName()));
}

int32 UUnrealMcpRqlEditorApi::ClearConsole()
{
	return FUnrealMcpLogCollector::Get().Clear();
}

UDataTable* UUnrealMcpRqlEditorApi::CreateDataTable(const FString& Path, const FString& RowStruct)
{
	const FString Wanted = RowStruct.TrimStartAndEnd();
	UScriptStruct* Struct = UnrealMcpPinTypes::ResolveStruct(Wanted);
	if (!Struct || !FDataTableEditorUtils::IsValidTableStruct(Struct))
		return Failed<UDataTable*>(FString::Printf(TEXT("'%s' is not a struct DataTable rows can use (a struct deriving from FTableRowBase, or a user-defined struct)"), *Wanted));
	FString Folder, Name;
	if (!ValidateNewAssetPath(Path, Folder, Name))
		return nullptr;
	UDataTableFactory* Factory = NewObject<UDataTableFactory>();
	FGCObjectScopeGuard FactoryGuard(Factory);
	Factory->Struct = Struct;
	UDataTable* Table = nullptr;
	{
		// Not recorded in the mutation's transaction: the factory creates the table before it assigns the row struct, and a
		// transaction snapshot of a table without one logs an error (creating an asset is not an undoable edit anyway).
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		Table = Cast<UDataTable>(AssetTools().CreateAsset(Name, Folder, UDataTable::StaticClass(), Factory));
	}
	return Table ? Table : Failed<UDataTable*>(FString::Printf(TEXT("could not create a DataTable at '%s'"), *Path));
}
