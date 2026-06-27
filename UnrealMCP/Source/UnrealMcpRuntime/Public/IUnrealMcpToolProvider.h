// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "IUnrealMcpProviderBase.h"

class FUnrealMcpToolRegistry;

/**
 * The Unreal-MCP extension contract (docs/ARCHITECTURE.md §5) — the only Unreal-MCP-specific contract
 * header. To declare tools you also include UnrealMcpToolRegistry.h, and to register the provider as a
 * modular feature you include Features/IModularFeatures.h (see Step 2/3 in docs/EXTENSIONS.md).
 *
 * An extension is any UE plugin that implements this interface and registers an instance as a
 * modular feature under GetModularFeatureName():
 *
 *     IModularFeatures::Get().RegisterModularFeature(
 *         IUnrealMcpToolProvider::GetModularFeatureName(), MyProviderInstance);
 *
 * Unreal-MCP discovers every registered provider on boot (and subscribes to register/unregister
 * events for late-loaded / hot-unloaded plugins), merges their tools into the single tool registry
 * in deterministic order (sorted by ExtensionId), and surfaces the merged manifest to the sidecar
 * (§2.2). See docs/EXTENSIONS.md for the full author guide and samples/UnrealAITemplate for a
 * working example.
 *
 * Isolation (§5): UE builds without C++ exceptions, so isolation is descriptor-level, not
 * body-level. Each tool a provider declares is validated (name pattern, schema well-formedness,
 * handler bound). Invalid entries are dropped and the failure is recorded on the extension's
 * registry record; a duplicate tool name across providers rejects the later registration
 * (first-wins by the ExtensionId sort). Other extensions are never affected. Crash-level faults
 * inside a tool body are editor crashes regardless of family and are out of isolation scope.
 */
class IUnrealMcpToolProvider : public IUnrealMcpProviderBase
{
public:
	virtual ~IUnrealMcpToolProvider() = default;

	/** The modular-feature name every provider registers under. Stable across the plugin's lifetime. */
	static FName GetModularFeatureName()
	{
		return FName(TEXT("UnrealMcpToolProvider"));
	}

	// GetExtensionId / GetDisplayName / GetExtensionVersion are inherited from IUnrealMcpProviderBase.

	/**
	 * Declare the extension's tools into @p Registry using the fluent FUnrealMcpToolBuilder
	 * (Registry.Tool("my-tool").Title(...).Param*(...).Handle(...)). Called on the game thread.
	 * Tools are automatically stamped with this provider's ExtensionId — do not call .ExtensionId().
	 * Invalid or duplicate entries are dropped/rejected and recorded; valid entries register normally.
	 */
	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) = 0;
};
