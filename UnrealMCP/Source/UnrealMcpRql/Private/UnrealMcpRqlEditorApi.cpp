// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlEditorApi.h"

#include "IUnrealMcpRqlModule.h"
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
#include "Engine/World.h"
#include "Components/ActorComponent.h"
#include "Factories/Factory.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "IAssetTools.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"


namespace UnrealMcpRqlEditorApiLocal
{
	IAssetTools& AssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	}

	using UnrealMcpRql::Failed;

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
			OutError = FString::Printf(TEXT("no level at '%s'"), *Package);
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
	if (!Label.IsEmpty())
		FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
	if (Parent)
	{
		Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
		if (Actor->GetAttachParentActor() != Parent)
		{
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
			return Failed<AActor*>(FString::Printf(TEXT("could not attach to %s (the new actor has no root component)"), *Parent->GetActorLabel()));
		}
	}
	return Actor;
}

AActor* UUnrealMcpRqlEditorApi::SpawnActor(const FString& ClassName, const FString& Label, FVector Location, FRotator Rotation, AActor* Parent)
{
	UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassName);
	if (!Class || !Class->IsChildOf(AActor::StaticClass()))
		return Failed<AActor*>(FString::Printf(TEXT("'%s' is not an actor class (try a class name such as \"PointLight\" or a Blueprint path)"), *ClassName));
	return UnrealMcpRql::SpawnActor(Class, Location, Rotation, Label, Parent);
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
	const ULevel* Current = World->GetCurrentLevel();
	const UPackage* Package = Current ? Current->GetOutermost() : nullptr;
	if (!Package || !FPackageName::DoesPackageExist(Package->GetName()))
		return Failed(TEXT("the current level was never saved: call SaveLevel(\"/Game/Maps/Name\")"));
	if (!UEditorLoadingAndSavingUtils::SaveCurrentLevel())
		Failed(TEXT("the current level could not be saved (checkout or write failure)"));
}

void UUnrealMcpRqlEditorApi::SaveAll()
{
	// Never-saved maps would prompt for a location: SaveDirtyPackages skips the prompt when it cannot ask.
	if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true))
		Failed(TEXT("some packages could not be saved (never-saved levels need SaveLevel(path))"));
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
	return Copy ? Copy : Failed<UObject*>(FString::Printf(TEXT("could not copy '%s'"), *Source));
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
		TArray<FString> Referencers = UEditorAssetLibrary::FindPackageReferencersForAsset(Path);
		Referencers.Remove(UEditorAssetLibrary::FindAssetData(Path).PackageName.ToString());
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
	if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		|| Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass()))
		return Failed<UObject*>(FString::Printf(TEXT("'%s' is not an asset class (actors are spawned: SpawnActor)"),
			Class ? *Class->GetName() : TEXT("None")));
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
	if (!IsWritableRoot(Destination) || !Destination.StartsWith(TEXT("/")))
		return Failed<TArray<UObject*>>(FString::Printf(TEXT("'%s' is not a writable content folder (e.g. /Game/Imported)"), *Destination));
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	FGCObjectScopeGuard TaskGuard(Task);
	Task->Filename = File;
	Task->DestinationPath = Destination;
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
	AssetTools().ImportAssetTasks({ Task });
	TArray<UObject*> Imported;
	for (UObject* Object : Task->GetObjects())
	{
		if (Object)
			Imported.Add(Object);
	}
	if (Imported.Num() == 0)
		UnrealMcpRql::Fail(FString::Printf(TEXT("the import of '%s' produced no assets"), *File));
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
