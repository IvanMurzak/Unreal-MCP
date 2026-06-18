// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

class FUnrealMcpPromptRegistry;

/**
 * The Unreal-MCP PROMPT extension contract (docs/ARCHITECTURE.md §5 / §A.2) — the prompt sibling of
 * IUnrealMcpToolProvider. To declare prompts you also include UnrealMcpPromptRegistry.h, and to register
 * the provider as a modular feature you include Features/IModularFeatures.h (mirrors the tool path).
 *
 * An extension is any UE plugin that implements this interface and registers an instance as a modular
 * feature under GetModularFeatureName():
 *
 *     IModularFeatures::Get().RegisterModularFeature(
 *         IUnrealMcpPromptProvider::GetModularFeatureName(), MyProviderInstance);
 *
 * Unreal-MCP discovers every registered provider on boot (and subscribes to register/unregister events
 * for late-loaded / hot-unloaded plugins), merges their prompts into the single prompt registry in
 * deterministic order (sorted by ExtensionId), and surfaces the merged manifest to the sidecar (§A.1).
 *
 * Isolation (§5): identical to the tool contract — descriptor-level, not body-level (UE builds without
 * C++ exceptions). Each prompt a provider declares is validated (name pattern, schema well-formedness,
 * handler bound). Invalid entries are dropped and a duplicate prompt name across providers rejects the
 * later registration (first-wins by the ExtensionId sort). Other extensions are never affected.
 */
class IUnrealMcpPromptProvider : public IModularFeature
{
public:
	virtual ~IUnrealMcpPromptProvider() = default;

	/** The modular-feature name every prompt provider registers under. Stable across the plugin's lifetime. */
	static FName GetModularFeatureName()
	{
		return FName(TEXT("UnrealMcpPromptProvider"));
	}

	/**
	 * Stable, unique extension identifier — reverse-DNS recommended (e.g. "com.foo.niagara-ai"). Used as
	 * the deterministic sort key and as the manifest's per-prompt extensionId. Two providers with the same
	 * id is an authoring error (the second's duplicate prompts are rejected).
	 */
	virtual FString GetExtensionId() const = 0;

	/** Human-readable name shown in the extensions UI row (§7). */
	virtual FText GetDisplayName() const = 0;

	/** Free-form version string of the extension (independent of the Unreal-MCP plugin version). */
	virtual FString GetExtensionVersion() const = 0;

	/**
	 * Declare the extension's prompts into @p Registry using the fluent FUnrealMcpPromptBuilder
	 * (Registry.Prompt("my-prompt").Title(...).Param*(...).Handle(...)). Called on the game thread.
	 * Prompts are automatically stamped with this provider's ExtensionId — do not call .ExtensionId().
	 * Invalid or duplicate entries are dropped/rejected and recorded; valid entries register normally.
	 */
	virtual void RegisterPrompts(FUnrealMcpPromptRegistry& Registry) = 0;
};
