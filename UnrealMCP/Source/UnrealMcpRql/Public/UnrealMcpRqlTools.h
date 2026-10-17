// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FUnrealMcpToolRegistry;

/**
 * The RQL tool family (docs/rql.md): rql-query, rql-mutate, rql-schema — the data layer of the editor, in place of the
 * removed actor / object / asset / level / editor-state / reflection tools.
 *
 * The C++ runtime executes QueryPlans and has no text parser (RQL ADR-0003): the unreal-mcp-bridge sidecar parses the
 * agent's `query` text with Rql.Parser and adds the canonical plan JSON as the `plan` argument; the handlers execute it
 * and answer with the QueryResult JSON, which the sidecar renders as compact rows. rql-schema answers with JSON the
 * sidecar renders the same way. Handlers run on the game thread (the dispatcher guarantees it).
 */
namespace UnrealMcpRqlTools
{
	/** Register the three tools into @p Registry (core tools). */
	UNREALMCPRQL_API void Register(FUnrealMcpToolRegistry& Registry);

	/**
	 * Republish the tools into @p Registry whenever the set of RQL modules changes (an extension loads / unloads: the
	 * tool texts and the unreal-rql skill list what modules add), then call @p OnChanged (push the manifest).
	 */
	UNREALMCPRQL_API void Attach(FUnrealMcpToolRegistry& Registry, TFunction<void()> OnChanged);

	/** Stop republishing (the coordinator's shutdown, before the registry is destroyed). */
	UNREALMCPRQL_API void Detach();

	/** Execute a QueryPlan JSON on a fresh session; returns the QueryResult JSON. Game thread. (Specs drive this.) */
	UNREALMCPRQL_API FString ExecutePlan(const FString& PlanJson, bool bAllowMutation, bool bDryRun, bool& bOutOk);

	/** The rql-schema JSON for @p TypeName (empty = overview). Game thread. */
	UNREALMCPRQL_API FString Schema(const FString& TypeName, bool& bOutOk);

	/** The `unreal-rql` skill markdown (core + installed modules). Game thread. */
	UNREALMCPRQL_API FString SkillMarkdown();
}
