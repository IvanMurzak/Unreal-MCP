// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Function.h"
#include "UnrealMcpEnablementFilter.h"
// The generic registry template (TUnrealMcpRegistry) + the shared FUnrealMcpExtensionRegistrationResult.
// FUnrealMcpToolRegistry is one instantiation of it (the prompt + resource registries are the other two).
#include "UnrealMcpRegistry.h"

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
struct UNREALMCPRUNTIME_API FUnrealMcpParamSpec
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
struct UNREALMCPRUNTIME_API FUnrealMcpToolCall
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
struct UNREALMCPRUNTIME_API FUnrealMcpImageContent
{
	FString Base64Data;               // base64-encoded image bytes (no data: URI prefix)
	FString MimeType = TEXT("image/png");
};

/** Terminal result of a tool invocation, mirroring the IPC tool-response shape (§1.3). */
struct UNREALMCPRUNTIME_API FUnrealMcpToolResult
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
struct UNREALMCPRUNTIME_API FUnrealMcpRegisteredTool
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

// FUnrealMcpExtensionRegistrationResult moved to the shared UnrealMcpRegistry.h (included above) so all three
// registries see one definition; it is still visible to every consumer that includes this header.

/** Fluent declaration builder (docs/ARCHITECTURE.md §3.3). One per tool; commits on destruction-free Handle(). */
class UNREALMCPRUNTIME_API FUnrealMcpToolBuilder
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
 * Kind-specific policy for the tool registry instantiation of TUnrealMcpRegistry. Supplies the key
 * accessor (Name), validation (kebab name + bound handler + param specs), the manifest strings, and the
 * nouns used in collision-rejection messages + log lines.
 */
struct FUnrealMcpToolRegistryTraits
{
	static const FString& KeyOf(const FUnrealMcpRegisteredTool& Entry) { return Entry.Name; }
	static const TCHAR* KindNoun() { return TEXT("tool"); }
	static const TCHAR* KindNounCapitalized() { return TEXT("Tool"); }
	static const TCHAR* KeyNoun() { return TEXT("name"); }
	static const TCHAR* ManifestType() { return TEXT("tool-manifest"); }
	static const TCHAR* ManifestArrayKey() { return TEXT("tools"); }
	/** True iff @p Name is a valid kebab-case tool id (non-empty; lowercase letters/digits with single internal hyphens). */
	static bool IsValidKey(const FString& Name);
	/** Validate a tool descriptor for the §5 isolation contract. Returns false + a reason on the first problem. */
	static bool Validate(const FUnrealMcpRegisteredTool& InTool, FString& OutError);
};

/**
 * The plugin-owned tool registry (docs/ARCHITECTURE.md §2.2). One instantiation of the generic
 * TUnrealMcpRegistry (which owns the compiled tool set, the manifest snapshot, the §7/§8 enablement, the
 * extension-scope registration, and the schema-hash). This class adds only the genuinely tool-specific
 * surface: the fluent builder entry, the kind-named public forwarders that keep the original API stable,
 * and the Execute dispatch (whose argument + result types are tool-specific).
 *
 * A monotonic Revision bumps on every registry change so the sidecar's manifest diff can reason about
 * ordering (§2.2 step 3). Not thread-safe. All mutation happens at startup (core families Register before
 * the bridge accepts), so the bridge may read BuildManifestJson() directly from its IPC reader thread on
 * handshake without a race. Any DYNAMIC re-registration (the §2.2 hot-reload path) MUST marshal both the
 * mutation and the manifest read through the game-thread dispatcher (§4) — the reader thread must never
 * observe a half-mutated set.
 */
class UNREALMCPRUNTIME_API FUnrealMcpToolRegistry
	: public TUnrealMcpRegistry<FUnrealMcpToolRegistry, FUnrealMcpRegisteredTool, FUnrealMcpToolRegistryTraits>
{
public:
	/** Begin declaring a tool (docs/ARCHITECTURE.md §3.3 fluent API). */
	FUnrealMcpToolBuilder Tool(const FString& Name);

	// --- Kind-named API forwarders (preserve the original public surface; the logic lives in the template).

	/** True iff @p Name is a valid kebab-case tool id. */
	static bool IsValidToolName(const FString& Name) { return FUnrealMcpToolRegistryTraits::IsValidKey(Name); }
	/** Validate a tool descriptor for the §5 isolation contract. Returns false + a reason on the first problem. */
	static bool ValidateTool(const FUnrealMcpRegisteredTool& InTool, FString& OutError) { return FUnrealMcpToolRegistryTraits::Validate(InTool, OutError); }

	bool HasTool(const FString& Name) const { return Has(Name); }
	/** Remove every tool stamped with @p ExtensionId (hot-unload / rebuild, §5). Returns count removed. */
	int32 RemoveToolsForExtension(const FString& ExtensionId) { return RemoveForExtension(ExtensionId); }
	/** Every registered tool name, sorted (the §7 MCP Tools window enumerates the full set, enabled or not). */
	TArray<FString> GetToolNamesSorted() const { return GetKeysSorted(); }

	/**
	 * True iff @p Name passes the §8 EnabledTools whitelist gate — the STATIC env-driven dimension of the
	 * served predicate, independent of the §7 runtime blocklist. The §7 MCP Tools window reads this to surface
	 * whitelist-gated tools, which the per-tool UI toggle cannot re-enable.
	 */
	bool PassesEnabledToolsWhitelist(const FString& Name) const { return PassesWhitelist(Name); }

	/**
	 * Set one tool's enabled flag directly — a GRANULAR helper, NOT the production §7 toggle (that path is
	 * view-model → OnToolEnablementChanged → ApplyDisabledTools, a whole-blocklist re-apply). Used by tests +
	 * any future single-tool caller. Does NOT update the retained whitelist/blocklist. Game-thread only.
	 */
	bool SetToolEnabled(const FString& Name, bool bEnabled) { return SetEnabled(Name, bEnabled); }

	/**
	 * Set the §8 env whitelist (UNREAL_MCP_TOOLS / Config.EnabledTools). RETAINED, re-evaluated on every
	 * re-registration + ApplyDisabledTools re-apply. Combined effective rule (§8): served iff (whitelist empty
	 * OR in whitelist) AND NOT in the blocklist. Game-thread only.
	 */
	void SetEnabledToolsFilter(const TArray<FString>& EnabledTools) { SetWhitelistFilter(EnabledTools); }

	/**
	 * Apply the persisted §7 per-tool enable-map (the MCP Tools window's `disabledTools` blocklist). RETAINED,
	 * so an extension hot-reload that re-registers tools FRESH re-applies the blocklist rather than resurrecting
	 * a disabled one; re-applying never clobbers the whitelist. Idempotent. Game-thread only.
	 */
	void ApplyDisabledTools(const TArray<FString>& DisabledNames) { ApplyBlocklist(DisabledNames); }

	/**
	 * Execute a tool by name on the CURRENT thread (the dispatcher has already marshalled to the game
	 * thread). Returns an error result for an unknown / disabled tool. The handler owns argument interpretation.
	 */
	FUnrealMcpToolResult Execute(const FString& Name, const FUnrealMcpToolCall& Call) const;
};
