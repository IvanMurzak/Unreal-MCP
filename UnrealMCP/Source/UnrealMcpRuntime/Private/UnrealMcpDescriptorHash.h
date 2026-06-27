// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Shared stable schema-hash helper for the tool / prompt / resource registries (docs/ARCHITECTURE.md §2.2).
 *
 * All three registries compute an identical "stable descriptor hash": serialize the descriptor JSON with the
 * condensed printer (no whitespace, stable field order) MINUS the mutable `enabled` + `schemaHash` fields, then
 * SHA-1 the UTF-8 bytes and return `sha1:<lowercase-hex>`. They previously each carried a verbatim copy guarded
 * only by a family-unique `*SerializeStable` name to dodge the unity-build ODR collision (conventions.md). This
 * single UNREALMCPRUNTIME_API free function replaces those three copies.
 *
 * Pass the descriptor JSON the caller already built via `Entry.ToDescriptorJson()` — the function strips
 * `enabled` + `schemaHash` from it (mutating the passed object, which is a throwaway built for the hash) before
 * canonicalizing, so the caller no longer needs the old deep-copy-the-struct-and-clear-SchemaHash dance.
 */
UNREALMCPRUNTIME_API FString UnrealMcpComputeDescriptorHash(const TSharedPtr<FJsonObject>& Descriptor);
