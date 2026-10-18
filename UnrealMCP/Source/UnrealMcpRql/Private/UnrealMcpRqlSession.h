// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpRqlTypes.h"

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

	/** Execute a canonical QueryPlan (JSON); returns the QueryResult JSON (spec form, no stats). */
	FString Execute(const FString& PlanJson, bool bAllowMutation, bool bDryRun, bool& bOutOk);

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

	/** Runs before the first write of a mutation (after the read-only checks); throwing vetoes the whole mutation. */
	using FMutationHook = std::function<void(const std::vector<rql::MutationRecord>& Records)>;
	void AddBeforeWrite(FMutationHook Hook);
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
	TArray<FMutationHook> BeforeWriteHooks;
	TArray<FMutationHook> CommittedHooks;
	TArray<TFunction<bool(const UObject*)>> WritableTemplates;
	int32 TransactionIndex = INDEX_NONE;

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
