// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Function.h"

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
	FString JsonType;        // "string" | "integer" | "number" | "boolean" | "object" | "array"
	FString Description;
	EUnrealMcpParamRequirement Requirement = EUnrealMcpParamRequirement::Optional;
	TSharedPtr<FJsonObject> ObjectSchema; // verbatim custom schema for object/array/exotic params (e.g. FVector → {x,y,z}, paths → array of string); null for plain scalars
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

/**
 * One binary image content block carried in a tool result (§1.2 — "binary payloads (screenshots)
 * travel as base64 strings inside JSON"; §1.3 — the MCP `content` array). The bridge serializes each
 * into a `{ "type": "image", "data": <base64>, "mimeType": <mime> }` block, which the sidecar maps
 * 1:1 onto an MCP image ContentBlock (ProxyResponseMapper). The screenshot family (§10) is the first
 * producer.
 */
struct UNREALMCPEDITOR_API FUnrealMcpImageContent
{
	FString Base64Data;               // base64-encoded image bytes (no data: URI prefix)
	FString MimeType = TEXT("image/png");
};

/** Terminal result of a tool invocation, mirroring the IPC tool-response shape (§1.3). */
struct UNREALMCPEDITOR_API FUnrealMcpToolResult
{
	bool bSuccess = true;
	FString Message;                       // human-readable text content block
	TSharedPtr<FJsonObject> Structured;    // structured content (may be null)
	TArray<FUnrealMcpImageContent> Images; // image content blocks, appended after the text block (§1.3)

	static FUnrealMcpToolResult Success(const FString& InMessage, const TSharedPtr<FJsonObject>& InStructured = nullptr)
	{
		return FUnrealMcpToolResult{ true, InMessage, InStructured };
	}
	static FUnrealMcpToolResult Error(const FString& InMessage)
	{
		return FUnrealMcpToolResult{ false, InMessage, nullptr };
	}
	/** Success carrying a single image content block (§10 screenshot family). */
	static FUnrealMcpToolResult SuccessWithImage(const FString& InMessage, const FString& InBase64Data,
		const TSharedPtr<FJsonObject>& InStructured = nullptr, const FString& InMimeType = TEXT("image/png"))
	{
		FUnrealMcpToolResult Result{ true, InMessage, InStructured };
		Result.Images.Add(FUnrealMcpImageContent{ InBase64Data, InMimeType });
		return Result;
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
	/**
	 * A dedicated SHORT skill description (the [AiSkillDescription] analog, docs/ARCHITECTURE.md §7) used for the
	 * generated SKILL.md YAML front-matter `description:` — distinct from the full Description, which becomes the
	 * SKILL.md body. Optional per tool; when empty the manifest falls back to the Title (a concise human label),
	 * and the sidecar generator further falls back to the capped full Description. Declared via the fluent
	 * FUnrealMcpToolBuilder::SkillDescription(); shipped to the sidecar in ToDescriptorJson() as `skillDescription`.
	 */
	FString SkillDescription;
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

/** Outcome of registering one extension provider's tools (docs/ARCHITECTURE.md §5). */
struct UNREALMCPEDITOR_API FUnrealMcpExtensionRegistrationResult
{
	/** Number of tools that passed validation + dedup and were committed under the extension's id. */
	int32 ToolsRegistered = 0;
	/** One human-readable line per dropped (invalid) or rejected (duplicate) entry; empty when healthy. */
	TArray<FString> Errors;
};

/** Fluent declaration builder (docs/ARCHITECTURE.md §3.3). One per tool; commits on destruction-free Handle(). */
class UNREALMCPEDITOR_API FUnrealMcpToolBuilder
{
public:
	FUnrealMcpToolBuilder(FUnrealMcpToolRegistry& InRegistry, const FString& InName);

	FUnrealMcpToolBuilder& Title(const FString& InTitle);
	FUnrealMcpToolBuilder& Description(const FString& InDescription);
	/** Set the SHORT skill description (the [AiSkillDescription] analog → SKILL.md front-matter). Optional. */
	FUnrealMcpToolBuilder& SkillDescription(const FString& InSkillDescription);
	FUnrealMcpToolBuilder& ParamString(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ParamInt(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ParamNumber(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ParamBool(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpToolBuilder& ParamVector(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	/**
	 * Generic escape hatch for params whose JSON Schema the typed helpers above do not cover (e.g.
	 * `{pitch,yaw,roll}` rotators, free-form `object` property bags, `array` lists). Pass the fully
	 * built schema in @p CustomSchema; it is used verbatim as the param's schema. Families build their
	 * own schemas locally so the shared builder surface stays small (one method, not one per math type).
	 */
	FUnrealMcpToolBuilder& Param(const FString& Name, const FString& JsonType, const FString& Desc, EUnrealMcpParamRequirement Req, TSharedPtr<FJsonObject> CustomSchema = nullptr);
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

	/**
	 * Commit a fully-built tool (called by the builder's Handle()).
	 * - Core path (default): replaces any same-named tool — core families are trusted.
	 * - Extension scope (inside RegisterExtension): the tool is stamped with the scope's ExtensionId,
	 *   validated (§5 — name pattern / schema well-formedness / handler bound), and deduped against
	 *   already-registered tools. An invalid entry is DROPPED and a duplicate is REJECTED (first-wins),
	 *   each recording a line in the scope's error list; neither affects other tools or extensions.
	 */
	void Commit(FUnrealMcpRegisteredTool&& InTool);

	/**
	 * Register an extension provider's tools (docs/ARCHITECTURE.md §5). Opens an "extension scope":
	 * every Commit during @p RegisterFn is stamped with @p ExtensionId, validated, and deduped, with
	 * invalid/duplicate entries dropped/rejected and recorded in the returned result. Scopes never nest.
	 */
	FUnrealMcpExtensionRegistrationResult RegisterExtension(const FString& ExtensionId, TFunctionRef<void(FUnrealMcpToolRegistry&)> RegisterFn);

	/** Remove every tool stamped with @p ExtensionId (hot-unload / rebuild, §5). Bumps revision if any removed. Returns count removed. */
	int32 RemoveToolsForExtension(const FString& ExtensionId);

	/** True iff @p Name is a valid kebab-case tool id (non-empty; lowercase letters/digits with single internal hyphens). */
	static bool IsValidToolName(const FString& Name);

	/** Validate a tool descriptor for the §5 isolation contract. Returns false + a reason on the first problem. */
	static bool ValidateTool(const FUnrealMcpRegisteredTool& InTool, FString& OutError);

	bool HasTool(const FString& Name) const { return Tools.Contains(Name); }
	int32 Num() const { return Tools.Num(); }
	const FUnrealMcpRegisteredTool* Find(const FString& Name) const { return Tools.Find(Name); }

	/** Every registered tool name, sorted (the §7 MCP Tools window enumerates the full set, enabled or not). */
	TArray<FString> GetToolNamesSorted() const;

	/** Count of tools whose bEnabled flag is set (backs the boot-time enable-map log line). */
	int32 NumEnabled() const;

	/**
	 * True iff @p Name passes the §8 EnabledTools whitelist gate (the whitelist is empty, or the name is listed) —
	 * the STATIC env-driven dimension of the served predicate, independent of the §7 runtime blocklist. The §7
	 * MCP Tools window reads this to surface whitelist-gated tools, which the per-tool UI toggle cannot re-enable.
	 */
	bool PassesEnabledToolsWhitelist(const FString& Name) const;

	/**
	 * Set one tool's enabled flag directly — a GRANULAR helper, NOT the production §7 toggle. The Tools-window
	 * path is view-model → OnToolEnablementChanged → ApplyDisabledTools (a whole-blocklist re-apply); this method
	 * is the single-name primitive used by tests and any future single-tool caller. It does NOT update the
	 * retained whitelist/blocklist, so a later SetEnabledToolsFilter / ApplyDisabledTools recompute can override
	 * the flag it set. Disabled tools are EXCLUDED from BuildManifestJson so the sidecar never exposes them in
	 * tools/list (§2.2). Bumps the revision on a real change. Unknown name / no-op change: returns false, no
	 * revision bump. Game-thread only (same contract as any §2.2 dynamic re-registration — see the class comment).
	 */
	bool SetToolEnabled(const FString& Name, bool bEnabled);

	/**
	 * Set the §8 env whitelist (UNREAL_MCP_TOOLS / Config.EnabledTools). A tool passes the whitelist gate iff the
	 * whitelist is EMPTY (no filter — the common case) or the tool's name is listed. RETAINED, so a later
	 * re-registration (extension hot-reload) and every ApplyDisabledTools re-apply re-evaluate it. Recomputes
	 * enablement now and bumps the revision once if any flag changed. Combined effective rule (§8): a tool is
	 * served iff (whitelist empty OR in whitelist) AND NOT in the blocklist. Game-thread only.
	 */
	void SetEnabledToolsFilter(const TArray<FString>& EnabledTools);

	/**
	 * Apply the persisted §7 per-tool enable-map (the MCP Tools window's `disabledTools` blocklist): every tool
	 * whose name is in @p DisabledNames is disabled, the rest follow the §8 whitelist gate. RETAINED, so an
	 * extension hot-reload that re-registers tools FRESH (RegisterExtension → Commit) re-applies the blocklist to
	 * the rebuilt tools rather than resurrecting a disabled one. Recomputes against BOTH the retained whitelist and
	 * this blocklist, so re-applying the blocklist never clobbers the whitelist. Idempotent; bumps the revision once
	 * if any flag changed. Called on boot (before the bridge accepts) and on every UI toggle. Game-thread only.
	 */
	void ApplyDisabledTools(const TArray<FString>& DisabledNames);

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

	// --- Retained §7/§8 enablement filters. Re-applied on every (re-)registration (Commit) so an extension
	//     hot-reload cannot resurrect a disabled tool, and a blocklist re-apply never clobbers the whitelist. ---
	TSet<FString> EnabledToolsWhitelist; // §8 UNREAL_MCP_TOOLS; empty = no filter (every tool passes the whitelist gate)
	TSet<FString> DisabledToolNames;     // §7 persisted blocklist (the MCP Tools window's `disabledTools`)

	/** The combined §8 effective-served predicate: enabled iff (whitelist empty OR member) AND NOT blocklisted. */
	bool ShouldToolBeEnabled(const FString& Name) const;

	/** Re-evaluate every registered tool's bEnabled against ShouldToolBeEnabled; bumps the revision once if any changed. */
	void RecomputeEnablement();

	// --- Extension-scope state (active only inside RegisterExtension; scopes never nest) ----------
	bool bExtensionScope = false;
	FString ScopeExtensionId;
	int32 ScopeToolsRegistered = 0;
	TArray<FString> ScopeErrors;
};
