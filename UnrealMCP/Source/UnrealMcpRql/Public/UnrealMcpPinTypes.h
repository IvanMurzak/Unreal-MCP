// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

/**
 * The friendly variable-type tokens of the tools (ARCHITECTURE §3.2), shared by blueprint-add-variable /
 * blueprint-modify-variable and the RQL user-defined struct fields: a primitive (bool, int, int64, byte, float, string,
 * name, text), a math struct (vector, vector2d, rotator, transform, color), or a class / struct name or path.
 */
namespace UnrealMcpPinTypes
{
	/** @p Type as a Blueprint pin type (an array when @p bIsArray); false + @p OutError when the token resolves to nothing. */
	UNREALMCPRQL_API bool Make(const FString& Type, bool bIsArray, FEdGraphPinType& OutPinType, FString& OutError);

	/** A struct by path ("/Game/Data/S_Enemy") or name ("GameplayTag", C++ spelling "FGameplayTag"); null when there is none. */
	UNREALMCPRQL_API UScriptStruct* ResolveStruct(const FString& Text);

	/** The token for a pin type ("int", "Vector", "StaticMesh", with "[]" / "{}" / "<map>" for containers). */
	UNREALMCPRQL_API FString ToString(const FEdGraphPinType& PinType);
}
