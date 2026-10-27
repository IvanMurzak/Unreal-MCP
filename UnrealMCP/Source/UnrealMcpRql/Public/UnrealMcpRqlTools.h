// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FUnrealMcpToolRegistry;
class FJsonObject;
class IUnrealMcpRqlModule;

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
	/** Register (or re-register: same-named entries are replaced) the three tools into @p Registry (core tools). */
	UNREALMCPRQL_API void Register(FUnrealMcpToolRegistry& Registry);

	/** The registered RQL modules (IUnrealMcpRqlModule modular features), sorted by module name. */
	UNREALMCPRQL_API TArray<IUnrealMcpRqlModule*> Modules();

	/**
	 * The extensions whose RQL modules are enabled (the editor's extension manager runs this from its module pass, then
	 * re-registers the tools: their skill lists what the modules add). A module without an extension id always loads;
	 * until the first call every module does.
	 */
	UNREALMCPRQL_API void SetEnabledExtensions(const TSet<FString>& ExtensionIds);

	/** Whether @p Module takes part in RQL sessions (SetEnabledExtensions). */
	UNREALMCPRQL_API bool IsModuleEnabled(const IUnrealMcpRqlModule& Module);

	/**
	 * Execute a QueryPlan JSON on a fresh session; returns the QueryResult JSON. Game thread. (Specs drive this.)
	 * With @p bAllowMutation the plan must end in a set or call stage (RQL 0.2.0: otherwise invalid-plan).
	 */
	UNREALMCPRQL_API FString ExecutePlan(const FString& PlanJson, bool bAllowMutation, bool bDryRun, bool& bOutOk);

	/**
	 * ExecutePlan with rql-mutate's max_targets: @p MaxTargets > 0 is this request's target limit (default 200, capped at
	 * 1000); 0 keeps the default.
	 */
	UNREALMCPRQL_API FString ExecutePlan(const FString& PlanJson, bool bAllowMutation, bool bDryRun, int32 MaxTargets, bool& bOutOk);

	/** The rql-schema JSON for @p TypeName (empty = overview). Game thread. */
	UNREALMCPRQL_API FString Schema(const FString& TypeName, bool& bOutOk);

	/** The `unreal-rql` skill markdown (core + installed modules). Game thread. */
	UNREALMCPRQL_API FString SkillMarkdown();

	// ---- spec helpers (an extension's Automation spec)

	/** The registered RQL module of the extension @p ExtensionId; nullptr when none. */
	UNREALMCPRQL_API IUnrealMcpRqlModule* FindModule(const FString& ExtensionId);

	/**
	 * ExecutePlan without dry run: whether the QueryResult is ok; @p OutResult receives the parsed QueryResult JSON.
	 * Pass @p bAllowMutation only for plans that end in set / call (read plans with it fail with invalid-plan).
	 */
	UNREALMCPRQL_API bool RunPlan(const FString& PlanJson, bool bAllowMutation, TSharedPtr<FJsonObject>* OutResult = nullptr);
}
