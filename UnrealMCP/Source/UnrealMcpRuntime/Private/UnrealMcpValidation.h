// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

struct FUnrealMcpParamSpec;

/**
 * Shared §5 descriptor-validation helpers for the tool / prompt registries (docs/ARCHITECTURE.md §3.3, §5).
 * The kebab-id rule and the param-spec well-formedness loop were verbatim copies in both registries; this single
 * UNREALMCPRUNTIME_API surface owns one definition of each, with the entry-kind noun injected for the messages.
 */
namespace FUnrealMcpValidation
{
	/**
	 * True iff @p Name is a valid kebab-case id: non-empty; lowercase letters / digits with single internal
	 * hyphens (no leading, trailing, or doubled hyphen). The shared rule behind IsValidToolName / IsValidPromptName.
	 */
	UNREALMCPRUNTIME_API bool IsValidKebabName(const FString& Name);

	/**
	 * Validate a declared parameter list for the §5 isolation contract: every param has a non-empty name, a known
	 * JSON type (string/integer/number/boolean/object/array), a schema present for object/array params, and no
	 * duplicate names. Returns false + a reason (naming the offending @p EntryName, prefixed by @p Noun, e.g.
	 * "tool" / "prompt") on the FIRST problem; true when the whole list is well-formed.
	 */
	UNREALMCPRUNTIME_API bool ValidateParamSpecs(
		const TArray<FUnrealMcpParamSpec>& Params, const TCHAR* Noun, const FString& EntryName, FString& OutError);
}
