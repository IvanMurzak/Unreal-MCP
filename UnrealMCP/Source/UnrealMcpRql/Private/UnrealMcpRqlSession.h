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

	/**
	 * rql-schema data (JSON the sidecar renders). @p Guide non-empty: that installed module's guide and sources (matched by
	 * module name, extension id, source name or source type). Else @p TypeName: that type, or without one the overview
	 * (sources, functions, installed modules with their guide call, present classes).
	 */
	FString Schema(const FString& TypeName, const FString& Guide, bool& bOutOk);

	/** The installed modules that add sources or guides, by name (the rql-* descriptions name them). */
	TArray<FString> ModuleNames() const;

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
	/** An extension module and the sources / guides its ConfigureRql added (index ranges into SourceEntries / Guides). */
	struct FModuleInfo
	{
		FString Name;
		FString ExtensionId;
		FString Version;
		int32 SourceBegin = 0;
		int32 SourceEnd = 0;
		int32 GuideBegin = 0;
		int32 GuideEnd = 0;
		/** Its operations classes (AddApiClass / AddApiSource). */
		TArray<UClass*> ApiClasses;
	};
	TArray<FModuleInfo> ModuleInfos;
	TArray<FBatchValidator> BatchValidators;
	TArray<FMutationHook> CommittedHooks;
	TArray<TFunction<bool(const UObject*)>> WritableTemplates;
	int32 TransactionIndex = INDEX_NONE;
	/** rql-query running a `call` of a read-only operation: the engine allows the call, the batch check refuses any other. */
	bool bReadOnlyCallRequest = false;
	/** The batch check refused a call in a read-only request: Execute answers with rql-query's permission-denied. */
	bool bRefusedReadOnlyCall = false;
	/** The call names a read-only operation and the caller gave no max_targets: the request runs at the ceiling. */
	bool bReadOnlyLimit = false;
	/** Every planned call of the request is a read-only operation's: its answer is a read, not a receipt. */
	bool bReadOnlyCalls = false;
	/** A call record of an operation that declares itself read-only (meta = (RqlReadOnly)). */
	bool IsReadOnlyCall(const rql::MutationRecord& Record) const;
	/** @p Records is a non-empty batch of read-only calls only. */
	bool AllReadOnlyCalls(const std::vector<rql::MutationRecord>& Records) const;
	/** @p Records is a non-empty batch of calls of operations that work the undo buffer themselves (Undo / Redo). */
	bool AllOwnUndoCalls(const std::vector<rql::MutationRecord>& Records) const;
	/** Subobjects create_for_literal built for the running query: kept alive until it ends. */
	TArray<TStrongObjectPtr<UObject>> LiteralObjects;
	FString UndoTitle = TEXT("RQL mutation");

	/** A host function as the query source (unreal.assets("Material") | ...): evaluated into a source. True = failed. */
	bool FunctionSource(rql::QueryPlan& Plan, rql::QueryResult& OutFailure);
	/** {"type": descriptor, "shared": its base, whose members the sidecar summarises once} for rql-schema. */
	rql::Json Describe(const rql::TypeDescriptor* Type);
	/**
	 * The module @p Name names, or null: its module name, extension id, the id's last segment with or without "unreal-ai-",
	 * one of its sources or its operations type; case, hyphens, underscores and spaces ignored (GuideKey).
	 */
	const FModuleInfo* FindModuleInfo(const FString& Name) const;
	/** @p Name lower-cased without hyphens, underscores and whitespace: how rql-schema's guide argument is matched. */
	static FString GuideKey(const FString& Name);
	/** The module with a guide whose operations class @p Type is, or null. */
	const FModuleInfo* ModuleOfType(const rql::TypeDescriptor* Type) const;
	bool IsApiType(const FModuleInfo& Info, const rql::TypeDescriptor* Type) const;
	/** {"name", "extension", "version"} of a module, for the overview and its guide. */
	static rql::Json ModuleJson(const FModuleInfo& Info);
	FString GuideSchema(const FString& Key, bool& bOutOk) const;
	void ConfigureMutations();
	void ConfigureModules();
	void CloseTransaction();
};

/** The core module: sources, functions, members, methods and struct adapters of the editor (UnrealMcpRqlCore.cpp). */
void UnrealMcpRqlConfigureCore(FUnrealMcpRqlSession& Session);
