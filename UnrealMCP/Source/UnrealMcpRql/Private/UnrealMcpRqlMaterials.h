// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;

/** One compile error of a material for one feature level / shader platform the editor compiled it for. */
struct FUnrealMcpMaterialCompileError
{
	FString FeatureLevel; // "SM6", "ES3_1"
	FString Platform;     // "PCD3D_SM6"
	FString Message;
};

/**
 * The compile errors of @p Material's resources, per feature level the editor compiles for (UnrealMcpRqlMaterials.cpp).
 * @p bWait finishes a compile still in flight first (blocks the game thread); without it a compile in flight reads as
 * its previous result. @p OutCompiled (optional) gets "SM6 PCD3D_SM6" for every resource that has compiled.
 */
TArray<FUnrealMcpMaterialCompileError> UnrealMcpRqlMaterialCompileErrors(UMaterialInterface* Material, bool bWait,
	TArray<FString>* OutCompiled = nullptr);
