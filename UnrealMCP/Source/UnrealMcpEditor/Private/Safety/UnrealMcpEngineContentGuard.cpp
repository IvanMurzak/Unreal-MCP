// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Safety/UnrealMcpEngineContentGuard.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcpEngineContentGuardImpl
{
	FString EngineGuardRefusal(const FString& Action, const FString& PackageName)
	{
		return FString::Printf(TEXT("Refused to %s '%s': it belongs to the installed Unreal Engine, not this project. ")
			TEXT("Engine content is shared by every project and must not be changed by the agent. Create or copy the asset under /Game instead ")
			TEXT("(for a level: create a new one in /Game/Maps and save that)."), *Action, *PackageName);
	}

	void EngineGuardRefuse(const FString& Message)
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] %s"), *Message);
		FUnrealMcpToolRegistry::AddCallRefusal(Message);
	}
}

void FUnrealMcpEngineContentGuard::Install()
{
	CanDeleteHandle = FEditorDelegates::OnAssetsCanDelete.AddRaw(this, &FUnrealMcpEngineContentGuard::CanDeleteAssets);
	if (GEngine != nullptr && GEngine->IsInitialized())
		BindSaveCheck(); // loaded after engine init (enabled at runtime): OnPostEngineInit has already fired
	else
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FUnrealMcpEngineContentGuard::BindSaveCheck);
}

void FUnrealMcpEngineContentGuard::BindSaveCheck()
{
	// Keep the editor's own check (read-only and source-control rules) running after ours.
	PreviousSaveCheck = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
	FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindRaw(this, &FUnrealMcpEngineContentGuard::IsPackageOkToSave);
	bSaveCheckBound = true;
}

void FUnrealMcpEngineContentGuard::Uninstall()
{
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	FEditorDelegates::OnAssetsCanDelete.Remove(CanDeleteHandle);
	// Restore the previous check only while ours is still the bound one (another plugin may have chained after us).
	if (bSaveCheckBound && FCoreUObjectDelegates::IsPackageOKToSaveDelegate.IsBoundToObject(this))
	{
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate = PreviousSaveCheck;
	}
	PreviousSaveCheck.Unbind();
	bSaveCheckBound = false;
}

bool FUnrealMcpEngineContentGuard::IsInstalledEngineFile(const FString& Filename)
{
	if (!FApp::IsEngineInstalled() || Filename.IsEmpty())
		return false;
	static const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
	return FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Filename), EngineDir);
}

bool FUnrealMcpEngineContentGuard::IsPackageOkToSave(UPackage* Package, const FString& Filename, FOutputDevice* Error)
{
	if (FUnrealMcpToolRegistry::IsExecutingTool() && IsInstalledEngineFile(Filename))
	{
		UnrealMcpEngineContentGuardImpl::EngineGuardRefuse(UnrealMcpEngineContentGuardImpl::EngineGuardRefusal(
			TEXT("save"), Package ? Package->GetName() : Filename));
		return false;
	}
	return PreviousSaveCheck.IsBound() ? PreviousSaveCheck.Execute(Package, Filename, Error) : true;
}

void FUnrealMcpEngineContentGuard::CanDeleteAssets(const TArray<UObject*>& Objects, FCanDeleteAssetResult& CanDelete)
{
	if (!FUnrealMcpToolRegistry::IsExecutingTool())
		return;
	for (const UObject* Object : Objects)
	{
		const UPackage* Package = Object ? Object->GetPackage() : nullptr;
		FString Filename;
		if (Package && FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename) && IsInstalledEngineFile(Filename))
		{
			UnrealMcpEngineContentGuardImpl::EngineGuardRefuse(UnrealMcpEngineContentGuardImpl::EngineGuardRefusal(TEXT("delete"), Package->GetName()));
			CanDelete.Set(false);
		}
	}
}
