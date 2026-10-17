// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpRqlTypes.h"

class FUnrealMcpRqlSession;
class UScriptStruct;

/** Most assets one unreal.assets(...) / asset source lists (they load lazily, as the query reads them). */
constexpr int32 UnrealMcpRqlMaxAssets = 200;

/** Assets of @p Class (and subclasses) under @p Path ("/Game" when empty), sorted by path, loaded when read. */
rql::Value UnrealMcpRqlAssetList(FUnrealMcpRqlTypes& Types, UClass* Class, const FString& Path);

/** Text and list literals of the math structs: "(1, 2, 3)", "X=1 Y=2 Z=3", [1, 2, 3], "#ff8800" (colors). */
bool UnrealMcpRqlParseMathStruct(UScriptStruct* Struct, const rql::Value& Value, void* Out, FString& OutError);

/** Whether @p Struct is one of the math structs with a text form (FVector, FRotator, FLinearColor, ...). */
bool UnrealMcpRqlIsMathStruct(const UScriptStruct* Struct);

/** RQL type adapters for the math structs: "(x, y, z)" output, text / list literals on writes. */
void UnrealMcpRqlAddStructAdapters(FUnrealMcpRqlSession& Session);
