// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpRqlTypes.h"
#include "UObject/StrongObjectPtr.h"

#include <functional>
#include <memory>

/**
 * One RQL engine over the live editor state, built for ONE tool call and dropped after it (game thread only).
 *
 * Rebuilding per call is cheap (descriptors fill lazily: a query touches a few classes) and is what keeps the provider
 * safe: a Blueprint compile, a hot reload or a user-defined struct edit recreates FProperty / UFunction objects, so no
 * descriptor may outlive the call that built it. Stable refs ("PointLight#1234") survive: they are object ids.
 *
 * The core module (UnrealMcpRqlCore.cpp) and every IUnrealMcpRqlModule extension configure the session in its
 * constructor. Mutations run inside one editor transaction (one Ctrl+Z step) and mark the changed packages dirty.
 */
class FUnrealMcpRqlSession
{
public:
	/** A source or function as rql-schema and the skill list it. */
	struct FEntry
	{
		FString Name;
		/** A source's value type: the element type of a list source (bList), else the type of its one value. */
		const rql::TypeDescriptor* Type = nullptr;
		bool bList = false;
		FString Description;
	};

	FUnrealMcpRqlSession();
	~FUnrealMcpRqlSession();
	FUnrealMcpRqlSession(const FUnrealMcpRqlSession&) = delete;
	FUnrealMcpRqlSession& operator=(const FUnrealMcpRqlSession&) = delete;

	FUnrealMcpRqlTypes& Types() { return *TypesPtr; }

	/** rql-mutate's default target limit and the ceiling its max_targets argument can raise it to (RQL spec 8). */
	static constexpr int32 DefaultMaxTargets = 200;
	static constexpr int32 HardMaxTargets = 1000;

	/**
	 * Execute a canonical QueryPlan (JSON); returns the QueryResult JSON (spec form, no stats). @p MaxTargets, when set, is
	 * this request's target limit: outside 1 .. HardMaxTargets the engine refuses the request (invalid-value). Unset keeps
	 * DefaultMaxTargets.
	 */
	FString Execute(const FString& PlanJson, bool bAllowMutation, bool bDryRun, bool& bOutOk, TOptional<int32> MaxTargets = {});

	/**
	 * The query text a mutation names its undo step after ("RQL: actors | ... | set ..."), so the Undo History and
	 * TRANSACTION UNDO say which change they revert. Empty: "RQL mutation".
	 */
	void SetUndoTitle(const FString& QueryText);

	/** rql-schema data: sources, functions, present classes and type descriptors (JSON the sidecar renders). */
	FString Schema(const FString& TypeName, bool& bOutOk);

	/** The unreal-rql skill: language, operations, live sources / functions, module guides, installed modules. */
	FString SkillMarkdown() const;

	// ---- registration (core module + extension modules)

	/** A source whose value is @p Type (bList: a list of @p Type); the type labels it in rql-schema (never the getter). */
	void AddSource(const FString& Name, const rql::TypeDescriptor* Type, bool bList, const FString& Description,
		std::function<rql::Value()> Getter);
	void AddFunction(const FString& Name, const FString& Description, rql::FunctionHandler Handler);
	void AddGuide(const FString& Topic, const FString& Markdown);
	void AddAdapter(rql::TypeAdapter Adapter);

	/**
	 * Checks every planned change of a set / call stage at once (RQL MutationPolicy::validate_batch), before anything is
	 * written or called, dry runs included. A non-empty reason refuses the whole stage (invalid-value), nothing changed.
	 */
	using FBatchValidator = std::function<FString(const std::vector<rql::MutationRecord>& Records)>;
	void AddBatchValidator(FBatchValidator Validator);
	using FMutationHook = std::function<void(const std::vector<rql::MutationRecord>& Records)>;
	/** Runs after every write of a mutation succeeded (not on dry run or rollback): persistence, notifications. */
	void AddCommitted(FMutationHook Hook);
	/** Class default objects this predicate accepts are writable (settings objects); other templates stay read-only. */
	void AllowTemplateWrites(TFunction<bool(const UObject*)> Predicate);

private:
	TUniquePtr<FUnrealMcpRqlTypes> TypesPtr;
	std::unique_ptr<rql::Engine> EnginePtr;
	TArray<FEntry> SourceEntries;
	TArray<FEntry> FunctionEntries;
	TArray<TPair<FString, FString>> Guides;
	TArray<FString> ModuleReports;
	TArray<FBatchValidator> BatchValidators;
	TArray<FMutationHook> CommittedHooks;
	TArray<TFunction<bool(const UObject*)>> WritableTemplates;
	int32 TransactionIndex = INDEX_NONE;
	/** Subobjects create_for_literal built for the running query: kept alive until it ends. */
	TArray<TStrongObjectPtr<UObject>> LiteralObjects;
	FString UndoTitle = TEXT("RQL mutation");

	/** A host function as the query source (unreal.assets("Material") | ...): evaluated into a source. True = failed. */
	bool FunctionSource(rql::QueryPlan& Plan, rql::QueryResult& OutFailure);
	/** {"type": descriptor, "shared": its base, whose members the sidecar summarises once} for rql-schema. */
	rql::Json Describe(const rql::TypeDescriptor* Type);
	void ConfigureMutations();
	void ConfigureModules();
	void CloseTransaction();
};

/** The core module: sources, functions, members, methods and struct adapters of the editor (UnrealMcpRqlCore.cpp). */
void UnrealMcpRqlConfigureCore(FUnrealMcpRqlSession& Session);
