// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlEditorApi.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTypes.h"
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
	IAssetRegistry& AssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	IAssetTools& AssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
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

	/** "/Game/Foo/Bar" (or "/Game/Foo/Bar.Bar") into "/Game/Foo" + "Bar". */
	bool SplitAssetPath(const FString& InPath, FString& OutFolder, FString& OutName)
	{
		FString Work = InPath.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Work.FindChar(TEXT('.'), Dot))
			Work.LeftInline(Dot);
		while (Work.EndsWith(TEXT("/")))
			Work.LeftChopInline(1);
		int32 Slash = INDEX_NONE;
		if (!Work.FindLastChar(TEXT('/'), Slash) || Slash <= 0 || Slash == Work.Len() - 1)
			return false;
		OutFolder = Work.Left(Slash);
		OutName = Work.Mid(Slash + 1);
		return true;
	}

	/** Long package name of a level asset path; false + reason for a malformed path. */
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

	UWorld* EditorWorld()
	{
		return FUnrealMcpObjectRef::GetEditorWorld();
	}
}

using namespace UnrealMcpRqlEditorApiLocal;

AActor* UUnrealMcpRqlEditorApi::SpawnActor(const FString& ClassName, const FString& Label, FVector Location, FRotator Rotation, AActor* Parent)
{
	UWorld* World = EditorWorld();
	if (!World)
	{
		UnrealMcpRql::Fail(TEXT("no editor world"));
		return nullptr;
	}
	UClass* Class = UnrealMcpRqlResolveClass(ClassName);
	if (!Class || !Class->IsChildOf(AActor::StaticClass()))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not an actor class (try a class name such as \"PointLight\" or a Blueprint path)"), *ClassName));
		return nullptr;
	}
	if (Class->HasAnyClassFlags(CLASS_Abstract))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is abstract"), *Class->GetName()));
		return nullptr;
	}
	// UWorld::SpawnActor rather than the editor actor subsystem: no viewport snapping (headless-safe). The open RQL
	// transaction records the transactional actor, so Ctrl+Z removes it.
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transactional;
	AActor* Actor = World->SpawnActor<AActor>(Class, Location, Rotation, Params);
	if (!Actor)
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not spawn %s"), *Class->GetName()));
		return nullptr;
	}
	if (!Label.IsEmpty())
		FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
	if (Parent)
	{
		Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
		if (Actor->GetAttachParentActor() != Parent)
		{
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
			UnrealMcpRql::Fail(FString::Printf(TEXT("could not attach to %s (the new actor has no root component)"), *Parent->GetActorLabel()));
			return nullptr;
		}
	}
	return Actor;
}

UWorld* UUnrealMcpRqlEditorApi::OpenLevel(const FString& Path)
{
	if (!GEditor)
	{
		UnrealMcpRql::Fail(TEXT("no editor"));
		return nullptr;
	}
	FString File, Error;
	if (!LevelFile(Path, File, Error))
	{
		UnrealMcpRql::Fail(Error);
		return nullptr;
	}
	UWorld* Opened = UEditorLoadingAndSavingUtils::LoadMap(File);
	if (!Opened)
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not open '%s'"), *Path));
	return Opened;
}

UWorld* UUnrealMcpRqlEditorApi::NewLevel(const FString& Path, const FString& Template)
{
	if (!GEditor)
	{
		UnrealMcpRql::Fail(TEXT("no editor"));
		return nullptr;
	}
	// Validate everything before the current world is replaced.
	FString Package, Error;
	if (!Path.IsEmpty())
	{
		if (!ToPackageName(Path, Package, Error))
		{
			UnrealMcpRql::Fail(Error);
			return nullptr;
		}
		FString Existing;
		if (FPackageName::DoesPackageExist(Package, &Existing) && !Existing.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' exists and is not a level"), *Package));
			return nullptr;
		}
	}
	UWorld* World = nullptr;
	if (!Template.IsEmpty())
	{
		FString TemplateFile;
		if (!LevelFile(Template, TemplateFile, Error))
		{
			UnrealMcpRql::Fail(Error);
			return nullptr;
		}
		World = UEditorLoadingAndSavingUtils::NewMapFromTemplate(TemplateFile, /*bSaveExistingMap*/ false);
	}
	else
	{
		World = GEditor->NewMap(); // the engine automation's headless-safe in-memory world
	}
	if (!World)
	{
		UnrealMcpRql::Fail(TEXT("could not create the level"));
		return nullptr;
	}
	if (!Package.IsEmpty() && !UEditorLoadingAndSavingUtils::SaveMap(World, Package))
		UnrealMcpRql::Fail(FString::Printf(TEXT("created the level but could not save it to '%s'"), *Package));
	return World;
}

bool UUnrealMcpRqlEditorApi::SaveLevel(const FString& Path)
{
	UWorld* World = EditorWorld();
	if (!World)
	{
		UnrealMcpRql::Fail(TEXT("no editor world"));
		return false;
	}
	if (!Path.IsEmpty())
	{
		FString Package, Error;
		if (!ToPackageName(Path, Package, Error))
		{
			UnrealMcpRql::Fail(Error);
			return false;
		}
		if (!UEditorLoadingAndSavingUtils::SaveMap(World, Package))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("could not save the level to '%s'"), *Package));
			return false;
		}
		return true;
	}
	// A never-saved level would open the interactive Save-As dialog (modal UI on the game thread): refuse instead.
	const ULevel* Current = World->GetCurrentLevel();
	const UPackage* Package = Current ? Current->GetOutermost() : nullptr;
	if (!Package || !FPackageName::DoesPackageExist(Package->GetName()))
	{
		UnrealMcpRql::Fail(TEXT("the current level was never saved: call SaveLevel(\"/Game/Maps/Name\")"));
		return false;
	}
	if (!UEditorLoadingAndSavingUtils::SaveCurrentLevel())
	{
		UnrealMcpRql::Fail(TEXT("the current level could not be saved (checkout or write failure)"));
		return false;
	}
	return true;
}

bool UUnrealMcpRqlEditorApi::SaveAll()
{
	// Never-saved maps would prompt for a location: SaveDirtyPackages skips the prompt when it cannot ask.
	if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true))
	{
		UnrealMcpRql::Fail(TEXT("some packages could not be saved (never-saved levels need SaveLevel(path))"));
		return false;
	}
	return true;
}

bool UUnrealMcpRqlEditorApi::UnloadSublevel(ULevel* Level)
{
	UWorld* World = EditorWorld();
	if (!World || !Level)
	{
		UnrealMcpRql::Fail(TEXT("no level"));
		return false;
	}
	if (Level == World->PersistentLevel)
	{
		UnrealMcpRql::Fail(TEXT("the persistent level cannot be unloaded"));
		return false;
	}
	if (!UEditorLevelUtils::RemoveLevelFromWorld(Level))
	{
		UnrealMcpRql::Fail(TEXT("could not unload the sublevel"));
		return false;
	}
	return true;
}

UObject* UUnrealMcpRqlEditorApi::CopyAsset(const FString& Source, const FString& Destination)
{
	FString Folder, Name;
	if (!UEditorAssetLibrary::DoesAssetExist(Source))
		UnrealMcpRql::Fail(FString::Printf(TEXT("no asset at '%s'"), *Source));
	else if (!SplitAssetPath(Destination, Folder, Name) || !IsWritableRoot(Destination))
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a writable asset path (use e.g. /Game/...)"), *Destination));
	else if (UEditorAssetLibrary::DoesAssetExist(Destination))
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' already exists"), *Destination));
	else if (UObject* Copy = UEditorAssetLibrary::DuplicateAsset(Source, Destination))
		return Copy;
	else
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not copy '%s'"), *Source));
	return nullptr;
}

bool UUnrealMcpRqlEditorApi::MoveAsset(const FString& Source, const FString& Destination)
{
	FString Folder, Name;
	if (!UEditorAssetLibrary::DoesAssetExist(Source))
		UnrealMcpRql::Fail(FString::Printf(TEXT("no asset at '%s'"), *Source));
	else if (!IsWritableRoot(Source) || !SplitAssetPath(Destination, Folder, Name) || !IsWritableRoot(Destination))
		UnrealMcpRql::Fail(TEXT("both paths must be writable asset paths (e.g. /Game/...)"));
	else if (UEditorAssetLibrary::DoesAssetExist(Destination))
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' already exists"), *Destination));
	else if (UEditorAssetLibrary::RenameAsset(Source, Destination))
		return true;
	else
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not move '%s'"), *Source));
	return false;
}

bool UUnrealMcpRqlEditorApi::DeleteAsset(const FString& Path, bool bForce)
{
	if (!UEditorAssetLibrary::DoesAssetExist(Path))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("no asset at '%s'"), *Path));
		return false;
	}
	if (!IsWritableRoot(Path))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is engine content"), *Path));
		return false;
	}
	if (!bForce)
	{
		const FAssetData Data = UEditorAssetLibrary::FindAssetData(Path);
		TArray<FName> Referencers;
		if (!Data.PackageName.IsNone())
			AssetRegistry().GetReferencers(Data.PackageName, Referencers);
		Referencers.Remove(Data.PackageName);
		if (Referencers.Num() > 0)
		{
			TArray<FString> Names;
			for (int32 Index = 0; Index < FMath::Min(Referencers.Num(), 5); ++Index)
				Names.Add(Referencers[Index].ToString());
			UnrealMcpRql::Fail(FString::Printf(TEXT("referenced by %d package(s) (%s); pass bForce = true to delete anyway"),
				Referencers.Num(), *FString::Join(Names, TEXT(", "))));
			return false;
		}
	}
	if (!UEditorAssetLibrary::DeleteAsset(Path))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not delete '%s'"), *Path));
		return false;
	}
	return true;
}

bool UUnrealMcpRqlEditorApi::CreateFolder(const FString& Path)
{
	if (!IsWritableRoot(Path) || !Path.StartsWith(TEXT("/")))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a writable content path (e.g. /Game/Props)"), *Path));
		return false;
	}
	if (UEditorAssetLibrary::DoesDirectoryExist(Path))
		return true;
	if (!UEditorAssetLibrary::MakeDirectory(Path))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not create '%s'"), *Path));
		return false;
	}
	return true;
}

bool UUnrealMcpRqlEditorApi::RefreshAssets(const FString& Path, bool bForce)
{
	AssetRegistry().ScanPathsSynchronous({ Path.IsEmpty() ? FString(TEXT("/Game")) : Path }, bForce);
	return true;
}

UMaterialInstanceConstant* UUnrealMcpRqlEditorApi::CreateMaterialInstance(const FString& Path, UMaterialInterface* Parent)
{
	FString Folder, Name;
	if (!Parent)
	{
		UnrealMcpRql::Fail(TEXT("no parent material"));
		return nullptr;
	}
	if (!SplitAssetPath(Path, Folder, Name) || !IsWritableRoot(Path))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a writable asset path (e.g. /Game/Materials/MI_Red)"), *Path));
		return nullptr;
	}
	if (UEditorAssetLibrary::DoesAssetExist(Path))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' already exists"), *Path));
		return nullptr;
	}
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	FGCObjectScopeGuard FactoryGuard(Factory);
	Factory->InitialParent = Parent;
	UMaterialInstanceConstant* Created = Cast<UMaterialInstanceConstant>(
		AssetTools().CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
	if (!Created)
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not create '%s'"), *Path));
	return Created;
}

TArray<UObject*> UUnrealMcpRqlEditorApi::ImportAsset(const FString& File, const FString& Destination, const FString& Name, bool bReplaceExisting)
{
	TArray<UObject*> Imported;
	if (!FPaths::FileExists(File))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("no file '%s'"), *File));
		return Imported;
	}
	if (!IsWritableRoot(Destination) || !Destination.StartsWith(TEXT("/")))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a writable content folder (e.g. /Game/Imported)"), *Destination));
		return Imported;
	}
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
	for (UObject* Object : Task->GetObjects())
	{
		if (Object)
			Imported.Add(Object);
	}
	if (Imported.Num() == 0)
		UnrealMcpRql::Fail(FString::Printf(TEXT("the import of '%s' produced no assets"), *File));
	return Imported;
}

bool UUnrealMcpRqlEditorApi::SaveAsset(UObject* Asset)
{
	if (!Asset)
	{
		UnrealMcpRql::Fail(TEXT("no asset"));
		return false;
	}
	if (!UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/ false))
	{
		UnrealMcpRql::Fail(FString::Printf(TEXT("could not save %s"), *Asset->GetPathName()));
		return false;
	}
	return true;
}

int32 UUnrealMcpRqlEditorApi::ClearConsole()
{
	return FUnrealMcpLogCollector::Get().Clear();
}
