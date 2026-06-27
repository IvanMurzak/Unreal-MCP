// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

/**
 * The common identity surface every Unreal-MCP extension provider exposes (docs/ARCHITECTURE.md §5 / §A.2).
 *
 * The tool / prompt / resource provider contracts each carried an identical id/name/version trio; this base
 * factors it out so the three interfaces (IUnrealMcpToolProvider / IUnrealMcpPromptProvider /
 * IUnrealMcpResourceProvider) declare only their KIND-specific surface — the `GetModularFeatureName()` the
 * kind registers under and the `Register*` body — and inherit the identity trio from here. A provider that
 * implements more than one kind (tools + prompts) implements this trio once.
 *
 * It is itself an IModularFeature so the manager's KIND-agnostic rebuild can treat any provider uniformly when
 * it only needs the identity (sort key, manifest extensionId, UI row); the per-kind discovery still keys off the
 * kind interface's own GetModularFeatureName().
 */
class IUnrealMcpProviderBase : public IModularFeature
{
public:
	virtual ~IUnrealMcpProviderBase() = default;

	/**
	 * Stable, unique extension identifier — reverse-DNS recommended (e.g. "com.foo.niagara-ai"). Used as the
	 * deterministic sort key and as the manifest's per-entry extensionId. Two providers with the same id is an
	 * authoring error (the second's duplicate entries are rejected, first-wins by the ExtensionId sort).
	 */
	virtual FString GetExtensionId() const = 0;

	/** Human-readable name shown in the extensions UI row (§7). */
	virtual FText GetDisplayName() const = 0;

	/** Free-form version string of the extension (independent of the Unreal-MCP plugin version). */
	virtual FString GetExtensionVersion() const = 0;
};
