// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"
// Prompt args reuse FUnrealMcpParamSpec / EUnrealMcpParamRequirement / FUnrealMcpToolCall verbatim, and the
// §3.2 schema generation is identical to the tool path — include the tool registry header rather than
// re-declaring those types (mirror, don't fork).
#include "UnrealMcpToolRegistry.h"

/**
 * Core prompt-registration types for the Unreal-MCP plugin (docs/ARCHITECTURE.md §A.1 / §A.2). The exact
 * prompt sibling of UnrealMcpToolRegistry.h: the registry is the plugin-owned source of truth for the
 * prompt set; each family declares its prompts through the fluent FUnrealMcpPromptBuilder; the registry
 * compiles each declaration into a JSON descriptor (the prompt-manifest shape) and dispatches incoming
 * prompt-get requests to the declared handler. The sidecar mirrors the manifest into MCP ProxyPrompts.
 *
 * Prompt arguments reuse FUnrealMcpParamSpec + EUnrealMcpParamRequirement and the §3.2 schema generation
 * VERBATIM (the same MakeTypedSchema / MakeVectorSchema / object/properties/required shape), and a prompt
 * handler receives the same FUnrealMcpToolCall args+cancel surface as a tool handler — the prompt args are
 * the same JSON-object shape. Only the prompt-specific result (role-tagged messages) differs.
 */

/** The message author role of a rendered prompt message (mirrors the .NET Role enum: User | Assistant). */
enum class EUnrealMcpPromptRole : uint8
{
	User,
	Assistant
};

/** One rendered prompt message: an author role + the message text. */
struct UNREALMCPRUNTIME_API FUnrealMcpPromptMessage
{
	EUnrealMcpPromptRole Role = EUnrealMcpPromptRole::User;
	FString Text;
};

/** Terminal result of a prompt-get, mirroring the IPC prompt-response shape (§A.1). */
struct UNREALMCPRUNTIME_API FUnrealMcpPromptResult
{
	bool bSuccess = true;
	FString Description;                          // optional human-readable prompt description
	TArray<FUnrealMcpPromptMessage> Messages;     // the rendered messages (role + text)

	static FUnrealMcpPromptResult Success(const TArray<FUnrealMcpPromptMessage>& InMessages, const FString& InDescription = FString())
	{
		FUnrealMcpPromptResult Result;
		Result.bSuccess = true;
		Result.Description = InDescription;
		Result.Messages = InMessages;
		return Result;
	}

	/** Convenience: a single-message success (the common one-shot prompt). */
	static FUnrealMcpPromptResult Success(const FString& Text, EUnrealMcpPromptRole Role, const FString& Description = FString())
	{
		FUnrealMcpPromptResult Result;
		Result.bSuccess = true;
		Result.Description = Description;
		Result.Messages.Add(FUnrealMcpPromptMessage{ Role, Text });
		return Result;
	}

	static FUnrealMcpPromptResult Error(const FString& InDescription)
	{
		FUnrealMcpPromptResult Result;
		Result.bSuccess = false;
		Result.Description = InDescription;
		return Result;
	}
};

/**
 * Signature of a prompt handler: runs on the game thread (the dispatcher guarantees it, §4), returns a
 * terminal result synchronously. Reuses FUnrealMcpToolCall as the args+cancel surface — the prompt args
 * are the same JSON-object shape as a tool call's.
 */
using FUnrealMcpPromptHandler = TFunction<FUnrealMcpPromptResult(const FUnrealMcpToolCall&)>;

/** A fully compiled prompt: descriptor (manifest shape) + handler. */
struct UNREALMCPRUNTIME_API FUnrealMcpRegisteredPrompt
{
	FString Name;
	FString Title;
	FString Description;
	EUnrealMcpPromptRole Role = EUnrealMcpPromptRole::User;
	TArray<FUnrealMcpParamSpec> Params;
	bool bEnabled = true;
	FString ExtensionId = TEXT("core");
	FString SchemaHash;
	FUnrealMcpPromptHandler Handler;

	/** Build the JSON Schema for this prompt's inputs from its declared params (§3.2 subset — identical to a tool's). */
	TSharedPtr<FJsonObject> BuildInputSchema() const;

	/** The prompt-manifest descriptor object (one entry in prompt-manifest.prompts[]). */
	TSharedPtr<FJsonObject> ToDescriptorJson() const;
};

class FUnrealMcpPromptRegistry;

/** Fluent declaration builder (§3.3 prompt analog). One per prompt; commits on Handle(). */
class UNREALMCPRUNTIME_API FUnrealMcpPromptBuilder
{
public:
	FUnrealMcpPromptBuilder(FUnrealMcpPromptRegistry& InRegistry, const FString& InName);

	FUnrealMcpPromptBuilder& Title(const FString& InTitle);
	FUnrealMcpPromptBuilder& Description(const FString& InDescription);
	FUnrealMcpPromptBuilder& Role(EUnrealMcpPromptRole InRole);
	FUnrealMcpPromptBuilder& ParamString(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpPromptBuilder& ParamInt(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpPromptBuilder& ParamNumber(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpPromptBuilder& ParamBool(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	FUnrealMcpPromptBuilder& ParamVector(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req = EUnrealMcpParamRequirement::Optional);
	/** Generic escape hatch for params whose JSON Schema the typed helpers do not cover (mirror the tool builder). */
	FUnrealMcpPromptBuilder& Param(const FString& Name, const FString& JsonType, const FString& Desc, EUnrealMcpParamRequirement Req, TSharedPtr<FJsonObject> CustomSchema = nullptr);
	FUnrealMcpPromptBuilder& ExtensionId(const FString& InExtensionId);

	/** Bind the handler and commit the prompt into the registry. */
	void Handle(FUnrealMcpPromptHandler InHandler);

private:
	FUnrealMcpPromptRegistry& Registry;
	FUnrealMcpRegisteredPrompt Prompt;
};

/**
 * The plugin-owned prompt registry (docs/ARCHITECTURE.md §A.1) — the exact prompt sibling of
 * FUnrealMcpToolRegistry. Holds the compiled prompt set, produces the JSON manifest snapshot for the
 * bridge, and dispatches prompt-get requests by name. A monotonic Revision bumps on every change so the
 * sidecar's manifest diff can reason about ordering. Not thread-safe (same contract as the tool registry):
 * all mutation happens at startup before the bridge accepts; any DYNAMIC re-registration MUST marshal both
 * the mutation and the manifest read through the game-thread dispatcher (§4).
 *
 * Prompt args reuse FUnrealMcpParamSpec + the §3.2 schema generation verbatim (see BuildInputSchema).
 */
class UNREALMCPRUNTIME_API FUnrealMcpPromptRegistry
{
public:
	/** Begin declaring a prompt (the §3.3 fluent API, prompt analog). */
	FUnrealMcpPromptBuilder Prompt(const FString& Name);

	/**
	 * Commit a fully-built prompt (called by the builder's Handle()).
	 * - Core path (default): replaces any same-named prompt — core families are trusted.
	 * - Extension scope (inside RegisterExtension): the prompt is stamped with the scope's ExtensionId,
	 *   validated (§5), and deduped. An invalid entry is DROPPED and a duplicate is REJECTED (first-wins).
	 */
	void Commit(FUnrealMcpRegisteredPrompt&& InPrompt);

	/**
	 * Register an extension provider's prompts (§5). Opens an "extension scope": every Commit during
	 * @p RegisterFn is stamped with @p ExtensionId, validated, and deduped. Reuses the tool registry's
	 * FUnrealMcpExtensionRegistrationResult (its `ToolsRegistered` count here means "entries registered").
	 * Scopes never nest.
	 */
	FUnrealMcpExtensionRegistrationResult RegisterExtension(const FString& ExtensionId, TFunctionRef<void(FUnrealMcpPromptRegistry&)> RegisterFn);

	/** Remove every prompt stamped with @p ExtensionId (hot-unload / rebuild, §5). Bumps revision if any removed. Returns count removed. */
	int32 RemovePromptsForExtension(const FString& ExtensionId);

	/** True iff @p Name is a valid kebab-case prompt id (same rule as the tool registry). */
	static bool IsValidPromptName(const FString& Name);

	/** Validate a prompt descriptor for the §5 isolation contract. Returns false + a reason on the first problem. */
	static bool ValidatePrompt(const FUnrealMcpRegisteredPrompt& InPrompt, FString& OutError);

	bool HasPrompt(const FString& Name) const { return Prompts.Contains(Name); }
	int32 Num() const { return Prompts.Num(); }
	const FUnrealMcpRegisteredPrompt* Find(const FString& Name) const { return Prompts.Find(Name); }

	/** Every registered prompt name, sorted. */
	TArray<FString> GetPromptNamesSorted() const;

	/** Count of prompts whose bEnabled flag is set. */
	int32 NumEnabled() const;

	/** True iff @p Name passes the §8 EnabledPrompts whitelist gate (empty whitelist, or the name is listed). */
	bool PassesEnabledPromptsWhitelist(const FString& Name) const;

	/**
	 * Set one prompt's enabled flag directly — a GRANULAR helper, NOT a production §7 toggle. Does NOT
	 * update the retained whitelist/blocklist. Disabled prompts are EXCLUDED from BuildManifestJson.
	 * Bumps the revision on a real change. Unknown name / no-op change: returns false, no bump. Game-thread only.
	 */
	bool SetPromptEnabled(const FString& Name, bool bEnabled);

	/** Set the §8 env whitelist. Mirrors the tool registry's SetEnabledToolsFilter exactly. Game-thread only. */
	void SetEnabledPromptsFilter(const TArray<FString>& EnabledPrompts);

	/** Apply the persisted §7 per-prompt enable-map (blocklist). Mirrors the tool registry's ApplyDisabledTools. Game-thread only. */
	void ApplyDisabledPrompts(const TArray<FString>& DisabledNames);

	/** Current manifest revision (bumps on every registry mutation). */
	int32 GetRevision() const { return Revision; }

	/** Build the full prompt-manifest message body (revision + prompts[]) for the bridge (§A.1). Disabled prompts excluded, sorted by name. */
	TSharedPtr<FJsonObject> BuildManifestJson() const;

	/**
	 * Execute a prompt by name on the CURRENT thread (the dispatcher has already marshalled to the game
	 * thread). Returns an error result for an unknown / disabled prompt. The handler owns argument interpretation.
	 */
	FUnrealMcpPromptResult Execute(const FString& Name, const FUnrealMcpToolCall& Call) const;

	/** Compute the stable schema hash for a prompt descriptor (excludes the enabled flag). */
	static FString ComputeSchemaHash(const FUnrealMcpRegisteredPrompt& InPrompt);

private:
	TMap<FString, FUnrealMcpRegisteredPrompt> Prompts;
	int32 Revision = 0;

	// --- Retained §7/§8 enablement filter (mirror the tool registry exactly).
	//     Whitelist = §8 whitelist (empty = no filter); Blocklist = §7 persisted blocklist. ---
	FUnrealMcpEnablementFilter Enablement;

	/** The combined §8 effective-served predicate: enabled iff (whitelist empty OR member) AND NOT blocklisted. */
	bool ShouldPromptBeEnabled(const FString& Name) const;

	/** Re-evaluate every registered prompt's bEnabled against ShouldPromptBeEnabled; bumps the revision once if any changed. */
	void RecomputeEnablement();

	// --- Extension-scope state (active only inside RegisterExtension; scopes never nest) ----------
	bool bExtensionScope = false;
	FString ScopeExtensionId;
	int32 ScopePromptsRegistered = 0;
	TArray<FString> ScopeErrors;
};
