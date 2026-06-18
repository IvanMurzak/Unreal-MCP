// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"   // TSharedPtr<FJsonObject> appears by-value in the UNREALMCPRUNTIME_API signatures below

class UObject;

/**
 * FProperty <-> JSON helpers for tool handlers (docs/ARCHITECTURE.md §3.2). Wraps FJsonObjectConverter
 * with two Unity-MCP-parity behaviours:
 *   - **Scoped reads**: a `paths` filter applied AFTER serialization (post-serialization JSON-path
 *     filtering, §3.2) so `*-get-data` tools can return only the requested dotted keys and save tokens.
 *   - **Transform writes**: `actor-modify`/`actor-component-modify` accept `location`/`rotation`/`scale`
 *     (and the component-relative variants) and route them through the SetActor / SetRelative APIs,
 *     because an actor/component transform is not a single writable FProperty.
 *
 * FJsonObjectConverter standardizes the first character of every key to lowercase, so serialized output
 * matches the §3.2 wire convention (`location`, `relativeLocation`, `{x,y,z}`) and the same lowercase
 * keys are accepted on write (property lookup is case-insensitive).
 *
 * All functions run ON the game thread — call only from a dispatched tool body (§4).
 */
namespace FUnrealMcpPropertyJson
{
	/**
	 * Serialize @p Object's reflected properties to a JSON object (§3.2 mapping). When @p Paths is
	 * non-empty, only those dotted paths (e.g. `rootComponent.relativeLocation`, `staticMesh`) are
	 * retained in the result (scoped read). Returns an empty object when @p Object is null.
	 */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> SerializeObject(const UObject* Object, const TArray<FString>& Paths);

	/** Filter @p Source down to the requested dotted @p Paths, preserving nesting. Empty Paths → @p Source
	 *  returned as-is (no copy — callers must not mutate the result in that case). */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> FilterByPaths(const TSharedPtr<FJsonObject>& Source, const TArray<FString>& Paths);

	/**
	 * Apply each field of @p Properties to @p Object via reflection (§3.2). Actor transform keys
	 * (`location`/`rotation`/`scale`) and scene-component relative-transform keys
	 * (`relativeLocation`/`relativeRotation`/`relativeScale3D`) are special-cased to the transform APIs;
	 * everything else is set through FJsonObjectConverter::JsonValueToUProperty. Returns the number of
	 * fields successfully applied; appends a one-line reason per failed field to @p OutErrors. Marks the
	 * object modified / dirty and fires PostEditChange when at least one field changed.
	 */
	UNREALMCPRUNTIME_API int32 ApplyProperties(UObject* Object, const TSharedPtr<FJsonObject>& Properties, TArray<FString>& OutErrors);
}
