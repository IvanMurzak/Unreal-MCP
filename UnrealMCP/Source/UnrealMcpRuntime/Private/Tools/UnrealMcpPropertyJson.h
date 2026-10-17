// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"   // TSharedPtr<FJsonObject> appears by-value in the UNREALMCPRUNTIME_API signature below

class UObject;

/**
 * FProperty -> JSON for the editor's Serialization Check window (docs/ARCHITECTURE.md §3.2): FJsonObjectConverter over an
 * object's reflected properties (keys lower-first-cased by the converter). Agent-facing object data is RQL (docs/rql.md).
 * Runs ON the game thread.
 */
namespace FUnrealMcpPropertyJson
{
	/** Serialize @p Object's reflected properties (transient / deprecated skipped). An empty object when @p Object is null. */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> SerializeObject(const UObject* Object);
}
