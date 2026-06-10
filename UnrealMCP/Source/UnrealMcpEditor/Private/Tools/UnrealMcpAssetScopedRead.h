// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * §3.2 "scoped reads" — token-saving post-serialization JSON filtering for the asset family's
 * read-only `*-get-data` tools (docs/ARCHITECTURE.md §3.2). A caller passes an array of
 * dot-separated field paths (e.g. "scalars.Roughness", "parent"); the tool returns ONLY those
 * branches of the fully-serialized structured result instead of the whole object, so an LLM can
 * ask for exactly the slice it needs.
 *
 * Implemented as a pure JSON→JSON transform (no editor state) so it is exercised by fast unit
 * Automation specs without a live content browser. Declared UNREALMCPEDITOR_API + lives in the
 * editor module's Private/ so the sibling Tests module can reach it via PrivateIncludePaths and
 * link the symbol (mirrors the dispatcher/ndjson test-seam pattern).
 *
 * NOTE: this is a family-local helper deliberately scoped to the asset task; a shared §3.2
 * scoped-read utility can be consolidated post-merge once the concurrent actor/blueprint chains
 * land (the implement-task brief asks each family to keep its read filtering local for now).
 */
namespace UnrealMcpAssetScopedRead
{
	/**
	 * Return a deep copy of @p Source pruned to the requested dot-paths.
	 * - Empty @p Paths → a deep copy of the whole object (no filtering requested).
	 * - Each path walks nested objects; a path that resolves to a leaf or sub-object copies that
	 *   value into the result at the same nesting. A path that does not resolve is silently skipped.
	 * - Overlapping paths merge (e.g. "a.b" + "a.c" → { a: { b, c } }).
	 */
	UNREALMCPEDITOR_API TSharedPtr<FJsonObject> Apply(const TSharedPtr<FJsonObject>& Source, const TArray<FString>& Paths);
}
