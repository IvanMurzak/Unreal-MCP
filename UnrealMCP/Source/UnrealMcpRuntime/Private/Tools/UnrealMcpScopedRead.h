// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * §3.2 "scoped reads" — the ONE shared token-saving post-serialization JSON-path filter (docs/ARCHITECTURE.md
 * §3.2). A caller passes an array of dot-separated field paths (e.g. "scalars.Roughness", "parent",
 * "rootComponent.relativeLocation"); the filter returns ONLY those branches of a fully-serialized JSON object
 * instead of the whole thing, so an LLM can ask for exactly the slice it needs.
 *
 * This unifies the two historically-separate implementations that each re-derived segment-split, per-segment
 * object descent, and overlapping-path merge:
 *   - UnrealMcpAssetScopedRead::Apply (the asset family's read-only *-get-data tools), and
 *   - FUnrealMcpPropertyJson::FilterByPaths (the actor/level families' get-data tools).
 * Both now delegate here; each forwards the FScopedReadOptions that reproduce its EXACT prior behavior. The two
 * call sites had three independent behavioral differences (all captured as options, none dropped):
 *   1. SEGMENT MATCHING — actor/level matched case-INSENSITIVELY (paths may be loosely cased like property
 *      writes); asset matched case-SENSITIVELY (exact TryGetField). → bCaseInsensitive.
 *   2. VALUE HANDLING — actor/level ALIASED the matched source value into the result (shared pointer); asset
 *      DEEP-CLONED it so the caller can mutate the result without touching the source. → bDeepClone.
 *   3. UNRESOLVED-PATH RESIDUE — actor/level built the result DURING the walk, so a path that resolved its
 *      first N segments but failed deeper LEFT the partial (empty) intermediate branch in the result; asset
 *      resolved the leaf first and only inserted on FULL resolution, leaving nothing for an unresolved path.
 *      → bLeavePartialBranches.
 *
 * Pure JSON→JSON (no editor state) so it is exercised by fast unit Automation specs. Declared
 * UNREALMCPRUNTIME_API and lives in the runtime module's Private/ so the sibling Tests module reaches it via
 * PrivateIncludePaths and links the symbol (mirrors the dispatcher/ndjson test-seam pattern).
 */
struct FScopedReadOptions
{
	/** Match each path segment against source keys case-INSENSITIVELY (actor/level) vs exact (asset). */
	bool bCaseInsensitive = false;
	/** DEEP-CLONE the matched value into the result (asset) vs ALIAS the source pointer (actor/level). */
	bool bDeepClone = false;
	/**
	 * Leave the partial (empty) intermediate branch produced while walking a path that ultimately fails to
	 * resolve a deeper segment (actor/level — result built during the walk) vs leave nothing for any
	 * unresolved path (asset — leaf resolved first, inserted only on full resolution).
	 */
	bool bLeavePartialBranches = false;

	/** Asset-family parity: exact-case match, deep-clone values, no partial residue. */
	static FScopedReadOptions AssetDefaults()
	{
		FScopedReadOptions Options;
		Options.bCaseInsensitive = false;
		Options.bDeepClone = true;
		Options.bLeavePartialBranches = false;
		return Options;
	}

	/** Actor/level-family parity: case-insensitive match, alias values, leave partial residue. */
	static FScopedReadOptions ActorLevelDefaults()
	{
		FScopedReadOptions Options;
		Options.bCaseInsensitive = true;
		Options.bDeepClone = false;
		Options.bLeavePartialBranches = true;
		return Options;
	}
};

namespace FUnrealMcpScopedRead
{
	/**
	 * Return @p Source pruned to the requested dot-@p Paths, per @p Options.
	 * - Empty @p Paths → the whole object: a DEEP COPY when Options.bDeepClone, else @p Source returned as-is
	 *   (aliased — the caller must not mutate it). (Matches each prior impl's no-paths branch.)
	 * - Each path walks nested objects; a path that resolves to a leaf or sub-object places that value into the
	 *   result at the same nesting (cloned or aliased per Options.bDeepClone). Overlapping paths merge
	 *   (e.g. "a.b" + "a.c" → { a: { b, c } }).
	 * - An unresolved path contributes nothing — except its already-walked partial intermediate branch when
	 *   Options.bLeavePartialBranches (the actor/level historical behavior).
	 */
	UNREALMCPRUNTIME_API TSharedPtr<FJsonObject> Filter(
		const TSharedPtr<FJsonObject>& Source, const TArray<FString>& Paths, const FScopedReadOptions& Options);
}
