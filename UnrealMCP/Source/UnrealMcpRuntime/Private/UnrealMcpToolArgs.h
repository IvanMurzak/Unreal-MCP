// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

struct FUnrealMcpToolCall;

/**
 * Shared argument readers for tool / prompt handlers (docs/ARCHITECTURE.md §3.2). The actor / level / editor /
 * asset families each carried their own `*GetStringArray` copy (family-prefixed to dodge the unity-build ODR
 * collision); this single UNREALMCPRUNTIME_API surface replaces them with one externally-linked definition.
 */
namespace FUnrealMcpToolArgs
{
	/**
	 * Read a string-array argument @p Key off @p Call — the §3.2 scoped-read `paths` / refs filter. Returns empty
	 * when the field is ABSENT. Two malformed shapes are hard errors (set @p OutError, return an EMPTY array):
	 * a non-string ENTRY inside the array, and a field that is PRESENT but is not an array (a bare string/number/
	 * object). Silently accepting either would flip the read to the OPPOSITE extreme of the requested scope
	 * ("identity only" / "full object" / "select nothing") with no signal to the caller, so every family
	 * converges on this single strict contract.
	 */
	UNREALMCPRUNTIME_API TArray<FString> GetStringArray(const FUnrealMcpToolCall& Call, const FString& Key, FString& OutError);
}
