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
 * Kind-specific policy for the prompt registry instantiation of TUnrealMcpRegistry. The exact prompt sibling
 * of FUnrealMcpToolRegistryTraits: key accessor (Name), validation (kebab name + bound handler + param specs),
 * manifest strings, and the nouns for collision-rejection messages + log lines.
 */
struct FUnrealMcpPromptRegistryTraits
{
	static const FString& KeyOf(const FUnrealMcpRegisteredPrompt& Entry) { return Entry.Name; }
	static const TCHAR* KindNoun() { return TEXT("prompt"); }
	static const TCHAR* KindNounCapitalized() { return TEXT("Prompt"); }
	static const TCHAR* KeyNoun() { return TEXT("name"); }
	static const TCHAR* ManifestType() { return TEXT("prompt-manifest"); }
	static const TCHAR* ManifestArrayKey() { return TEXT("prompts"); }
	/** True iff @p Name is a valid kebab-case prompt id (same rule as the tool registry). */
	static bool IsValidKey(const FString& Name);
	/** Validate a prompt descriptor for the §5 isolation contract. Returns false + a reason on the first problem. */
	static bool Validate(const FUnrealMcpRegisteredPrompt& InPrompt, FString& OutError);
};

/**
 * The plugin-owned prompt registry (docs/ARCHITECTURE.md §A.1) — one instantiation of the generic
 * TUnrealMcpRegistry, the exact prompt sibling of FUnrealMcpToolRegistry. The template owns the compiled
 * prompt set, the manifest snapshot, the §7/§8 enablement, the extension-scope registration, and the
 * schema-hash; this class adds only the prompt-specific surface (the fluent builder entry, the kind-named
 * forwarders that keep the original API stable, and the Execute dispatch). Same threading contract as the
 * tool registry: all mutation happens at startup before the bridge accepts; any DYNAMIC re-registration MUST
 * marshal both the mutation and the manifest read through the game-thread dispatcher (§4).
 *
 * Prompt args reuse FUnrealMcpParamSpec + the §3.2 schema generation verbatim (see BuildInputSchema).
 */
class UNREALMCPRUNTIME_API FUnrealMcpPromptRegistry
	: public TUnrealMcpRegistry<FUnrealMcpPromptRegistry, FUnrealMcpRegisteredPrompt, FUnrealMcpPromptRegistryTraits>
{
public:
	/** Begin declaring a prompt (the §3.3 fluent API, prompt analog). */
	FUnrealMcpPromptBuilder Prompt(const FString& Name);

	// --- Kind-named API forwarders (preserve the original public surface; the logic lives in the template).

	/** True iff @p Name is a valid kebab-case prompt id (same rule as the tool registry). */
	static bool IsValidPromptName(const FString& Name) { return FUnrealMcpPromptRegistryTraits::IsValidKey(Name); }
	/** Validate a prompt descriptor for the §5 isolation contract. Returns false + a reason on the first problem. */
	static bool ValidatePrompt(const FUnrealMcpRegisteredPrompt& InPrompt, FString& OutError) { return FUnrealMcpPromptRegistryTraits::Validate(InPrompt, OutError); }

	bool HasPrompt(const FString& Name) const { return Has(Name); }
	/** Remove every prompt stamped with @p ExtensionId (hot-unload / rebuild, §5). Returns count removed. */
	int32 RemovePromptsForExtension(const FString& ExtensionId) { return RemoveForExtension(ExtensionId); }
	/** Every registered prompt name, sorted. */
	TArray<FString> GetPromptNamesSorted() const { return GetKeysSorted(); }

	/** True iff @p Name passes the §8 EnabledPrompts whitelist gate (empty whitelist, or the name is listed). */
	bool PassesEnabledPromptsWhitelist(const FString& Name) const { return PassesWhitelist(Name); }

	/**
	 * Set one prompt's enabled flag directly — a GRANULAR helper, NOT a production §7 toggle. Does NOT update
	 * the retained whitelist/blocklist. Disabled prompts are EXCLUDED from BuildManifestJson. Game-thread only.
	 */
	bool SetPromptEnabled(const FString& Name, bool bEnabled) { return SetEnabled(Name, bEnabled); }

	/** Set the §8 env whitelist. Mirrors the tool registry's SetEnabledToolsFilter exactly. Game-thread only. */
	void SetEnabledPromptsFilter(const TArray<FString>& EnabledPrompts) { SetWhitelistFilter(EnabledPrompts); }

	/** Apply the persisted §7 per-prompt enable-map (blocklist). Mirrors the tool registry's ApplyDisabledTools. Game-thread only. */
	void ApplyDisabledPrompts(const TArray<FString>& DisabledNames) { ApplyBlocklist(DisabledNames); }

	/**
	 * Execute a prompt by name on the CURRENT thread (the dispatcher has already marshalled to the game
	 * thread). Returns an error result for an unknown / disabled prompt. The handler owns argument interpretation.
	 */
	FUnrealMcpPromptResult Execute(const FString& Name, const FUnrealMcpToolCall& Call) const;
};
