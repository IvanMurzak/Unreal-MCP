// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/ThreadSafeBool.h"

/**
 * Core tool-registration types for the Unreal-MCP plugin (docs/ARCHITECTURE.md §2, §3.3).
 *
 * The registry is the plugin-owned source of truth for the tool set. Each core family declares its
 * tools through the fluent FUnrealMcpToolBuilder; the registry compiles each declaration into a JSON
 * descriptor (the §2.2 manifest shape) and dispatches incoming tool-calls to the declared handler.
 * The sidecar mirrors the manifest into MCP ProxyTools; the C++ side never knows about MCP.
 */

/** Whether a declared parameter is required or optional. */
enum class EUnrealMcpParamRequirement : uint8
{
	Optional,
	Required
};

/** A declared tool parameter (name + JSON-schema type + description). */
struct UNREALMCPEDITOR_API FUnrealMcpParamSpec
{
	FString Name;
	FString JsonType;        // "string" | "integer" | "number" | "boolean" | "object"
	FString Description;
	EUnrealMcpParamRequirement Requirement = EUnrealMcpParamRequirement::Optional;
	TSharedPtr<FJsonObject> ObjectSchema; // for "object" params (e.g. FVector → {x,y,z})
};

/**
 * The arguments + cancellation surface handed to a tool handler. The handler runs ON the game thread
 * (the dispatcher guarantees this, §4); it must never block on bridge state or pump modal UI.
 */
struct UNREALMCPEDITOR_API FUnrealMcpToolCall
{
	/** Raw JSON arguments object (never null; empty object when the call carried none). */
	TSharedPtr<FJsonObject> Arguments;

	/** Cooperative cancellation flag (set by a tool-cancel or a timeout, §4). May be null in tests. */
	// Held by shared ownership so the flag outlives the bridge's CancelFlags map entry: a copy of this
	// call object (the dispatcher captures one for the game-thread body) keeps the flag alive even after
	// the response is sent and the map entry is removed — IsCancelled() can never deref freed memory.
	TSharedPtr<const FThreadSafeBool, ESPMode::ThreadSafe> CancelRequested;

	FUnrealMcpToolCall() : Arguments(MakeShared<FJsonObject>()) {}
	explicit FUnrealMcpToolCall(const TSharedPtr<FJsonObject>& InArgs)
		: Arguments(InArgs.IsValid() ? InArgs : MakeShared<FJsonObject>()) {}

	bool Has(const FString& Key) const { return Arguments->HasField(Key); }
	bool IsCancelled() const { return CancelRequested.IsValid() && *CancelRequested; }

	FString GetString(const FString& Key, const FString& Default = FString()) const;
	int64 GetInt(const FString& Key, int64 Default = 0) const;
	double GetNumber(const FString& Key, double Default = 0.0) const;
	bool GetBool(const FString& Key, bool Default = false) const;
	FVector GetVector(const FString& Key, const FVector& Default = FVector::ZeroVector) const;
	FRotator GetRotator(const FString& Key, const FRotator& Default = FRotator::ZeroRotator) const;
};

/** Terminal result of a tool invocation, mirroring the IPC tool-response shape (§1.3). */
struct UNREALMCPEDITOR_API FUnrealMcpToolResult
{
	bool bSuccess = true;
	FString Message;                       // human-readable text content block
	TSharedPtr<FJsonObject> Structured;    // structured content (may be null)

	static FUnrealMcpToolResult Success(const FString& InMessage, const TSharedPtr<FJsonObject>& InStructured = nullptr)
	{
		return FUnrealMcpToolResult{ true, InMessage, InStructured };
	}
	static FUnrealMcpToolResult Error(const FString& InMessage)
	{
		return FUnrealMcpToolResult{ false, InMessage, nullptr };
	}
};

/** Signature of a tool handler: runs on the game thread, returns a terminal result synchronously. */
using FUnrealMcpToolHandler = TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)>;

/** A fully compiled tool: descriptor (manifest shape) + handler. */
struct UNREALMCPEDITOR_API FUnrealMcpRegisteredTool
{
	FString Name;
	FString Title;
	FString Description;
	TArray<FUnrealMcpParamSpec> Params;
	TSharedPtr<FJsonObject> OutputSchema;
	bool bReadOnlyHint = false;
	bool bDestructiveHint = false;
	bool bIdempotentHint = false;
	bool bOpenWorldHint = false;
	bool bEnabled = true;
	FString ExtensionId = TEXT("core");
	FString SchemaHash;
	FUnrealMcpToolHandler Handler;

	/** Build the JSON Schema for this tool's inputs from its declared params (§3.2 subset). */
	TSharedPtr<FJsonObject> BuildInputSchema() const;

	/** The §2.2 descriptor object (one entry in tool-manifest.tools[]). */
	TSharedPtr<FJsonObject> ToDescriptorJson() const;
};

class FUnrealMcpToolRegistry;

/** Fluent declaration builder (docs/ARCHITECTURE.md §3.3). One per tool; commits on destruction-free Handle(). */
class UNREALMCPEDITOR_API FUnrealMcpToolBuilder
{
public:
	FUnrealMcpToolBuilder(FUnrealMcpToolRegistry& InRegistry, const FString& InName);

	FUnrealMcpToolBuilder& Title(const FString& InTitle);
	FUnrealMcpToolBuilder& Description(const FString& InDescription);
	FUnrealMcpToolBuilder& ParamString(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ParamInt(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ParamNumber(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ParamBool(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ParamVector(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ReadOnlyHint(bool bValue);
	FUnrealMcpToolBuilder& DestructiveHint(bool bValue);
	FUnrealMcpToolBuilder& IdempotentHint(bool bValue);
	FUnrealMcpToolBuilder& OpenWorldHint(bool bValue);
	FUnrealMcpToolBuilder& ExtensionId(const FString& InExtensionId);

	/** Bind the handler and commit the tool into the registry. */
	void Handle(FUnrealMcpToolHandler InHandler);

private:
	FUnrealMcpToolRegistry& Registry;
	FUnrealMcpRegisteredTool Tool;
};

/**
 * The plugin-owned tool registry (docs/ARCHITECTURE.md §2.2). Holds the compiled tool set, produces
 * the JSON manifest snapshot for the bridge, and dispatches tool-calls by name. A monotonic Revision
 * bumps on every registry change so the sidecar's manifest diff can reason about ordering (§2.2 step 3).
 * Not thread-safe. All mutation happens at startup (core families Register before the bridge accepts), so
 * the bridge may read BuildManifestJson() directly from its IPC reader thread on handshake without a race.
 * Any DYNAMIC re-registration (the §2.2 hot-reload path) MUST marshal both the mutation and the manifest
 * read through the game-thread dispatcher (§4) — the reader thread must never observe a half-mutated set.
 */
class UNREALMCPEDITOR_API FUnrealMcpToolRegistry
{
public:
	/** Begin declaring a tool (docs/ARCHITECTURE.md §3.3 fluent API). */
	FUnrealMcpToolBuilder Tool(const FString& Name);

	/** Commit a fully-built tool (called by the builder's Handle()). Replaces any same-named tool. */
	void Commit(FUnrealMcpRegisteredTool&& InTool);

	bool HasTool(const FString& Name) const { return Tools.Contains(Name); }
	int32 Num() const { return Tools.Num(); }
	const FUnrealMcpRegisteredTool* Find(const FString& Name) const { return Tools.Find(Name); }

	/** Current manifest revision (bumps on every registry mutation). */
	int32 GetRevision() const { return Revision; }

	/** Build the full tool-manifest message body (revision + tools[]) for the bridge (§2.2). */
	TSharedPtr<FJsonObject> BuildManifestJson() const;

	/**
	 * Execute a tool by name on the CURRENT thread (the dispatcher has already marshalled to the game
	 * thread). Returns an error result for an unknown tool. The handler owns argument interpretation.
	 */
	FUnrealMcpToolResult Execute(const FString& Name, const FUnrealMcpToolCall& Call) const;

	/** Compute the stable schema hash for a tool descriptor (§2.2 — excludes the enabled flag). */
	static FString ComputeSchemaHash(const FUnrealMcpRegisteredTool& InTool);

private:
	TMap<FString, FUnrealMcpRegisteredTool> Tools;
	int32 Revision = 0;
};
