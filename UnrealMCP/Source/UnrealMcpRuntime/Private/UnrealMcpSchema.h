// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Shared JSON-Schema builders for tool / prompt parameters (docs/ARCHITECTURE.md §3.2).
 *
 * These were ODR-cloned per family — the registries each carried `MakeTypedSchema` / `MakeVectorSchema`
 * (family-prefixed to dodge the unity-build collision), and the actor/level/editor tool families each carried
 * their own `Make*StringArraySchema` / property-bag / rotator copy. This single UNREALMCPRUNTIME_API surface
 * replaces all of them with externally-linked free functions (defined once, declared once), so there is exactly
 * one definition of each and the unity build cannot collide. The editor module reaches these via the runtime
 * module's public dependency + private-include path, exactly like the other Tools/ helpers.
 */
namespace FUnrealMcpSchema
{
	/** `{ type: <JsonType>[, description] }` — the scalar §3.2 schema (string/integer/number/boolean). */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> TypedSchema(const FString& JsonType, const FString& Description);

	/** `{ type:"object", properties:{x,y,z:number}[, description] }` — the §3.2 FVector mapping. */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> VectorSchema(const FString& Description);

	/** `{ type:"object", properties:{pitch,yaw,roll:number}[, description] }` — the §3.2 FRotator mapping (degrees). */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> Rotator(const FString& Description);

	/** `{ type:"array", items:{type:"string"}[, description] }` — the §3.2 scoped-read `paths` filter. */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> StringArray(const FString& Description);

	/** `{ type:"object", additionalProperties:true[, description] }` — a free-form `{ key: value }` property bag. */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> ObjectBag(const FString& Description);
}

/** Serialize a JSON object to a compact (whitespace-free, stable field order) string — the canonical condensed
 *  form used for the §2.2 stable schema hash and any condensed wire payload. One definition shared across the
 *  runtime module. */
UNREALMCPRUNTIME_API FString UnrealMcpSerializeCondensed(const TSharedPtr<FJsonObject>& Object);
