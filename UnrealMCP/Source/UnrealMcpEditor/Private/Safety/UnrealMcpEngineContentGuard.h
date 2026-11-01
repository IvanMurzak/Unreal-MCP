// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/UObjectGlobals.h"

class UPackage;
class FOutputDevice;
struct FCanDeleteAssetResult;

/**
 * Keeps an agent from changing the installed Unreal Engine. While a tool call runs
 * (FUnrealMcpToolRegistry::IsExecutingTool), saving or deleting a package whose file lies inside an INSTALLED
 * engine (e.g. /Engine/Maps/Entry, the default startup map) is refused and the call reports it. The user's own
 * editor actions and source-built engines are unaffected. Chains the previously bound save delegate.
 */
class UNREALMCPEDITOR_API FUnrealMcpEngineContentGuard
{
public:
	/** Bind after engine init: UEditorEngine::Init binds IsPackageOKToSaveDelegate itself and would replace an earlier binding. */
	void Install();
	void Uninstall();

	/** True when @p Filename lies inside the installed engine's directory. False for a source-built engine. */
	static bool IsInstalledEngineFile(const FString& Filename);

private:
	bool IsPackageOkToSave(UPackage* Package, const FString& Filename, FOutputDevice* Error);
	void CanDeleteAssets(const TArray<UObject*>& Objects, FCanDeleteAssetResult& CanDelete);

	FCoreUObjectDelegates::FIsPackageOKToSaveDelegate PreviousSaveCheck;
	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle CanDeleteHandle;
	bool bSaveCheckBound = false;
};
