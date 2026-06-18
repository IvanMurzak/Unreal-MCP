// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

class FUnrealMcpResourceRegistry;

/**
 * The Unreal-MCP RESOURCE extension contract (docs/ARCHITECTURE.md §5 / §A.2) — the resource sibling of
 * IUnrealMcpToolProvider / IUnrealMcpPromptProvider. To declare resources you also include
 * UnrealMcpResourceRegistry.h, and to register the provider as a modular feature you include
 * Features/IModularFeatures.h (mirrors the tool/prompt paths).
 *
 * An extension is any UE plugin that implements this interface and registers an instance as a modular
 * feature under GetModularFeatureName():
 *
 *     IModularFeatures::Get().RegisterModularFeature(
 *         IUnrealMcpResourceProvider::GetModularFeatureName(), MyProviderInstance);
 *
 * Unreal-MCP discovers every registered provider on boot (and subscribes to register/unregister events
 * for late-loaded / hot-unloaded plugins), merges their resources into the single resource registry in
 * deterministic order (sorted by ExtensionId), and surfaces the merged manifest to the sidecar (§A.1).
 *
 * MVP scope: static fixed-URI resources only (templated/parameterized URIs are deferred, §A.1). A
 * resource's URI is its identity (the registry dedup/registration key).
 *
 * Isolation (§5): identical to the tool/prompt contract — descriptor-level, not body-level (UE builds
 * without C++ exceptions). Each resource a provider declares is validated (uri non-empty, mime/handler
 * present). Invalid entries are dropped and a duplicate resource uri across providers rejects the later
 * registration (first-wins by the ExtensionId sort). Other extensions are never affected.
 */
class IUnrealMcpResourceProvider : public IModularFeature
{
public:
	virtual ~IUnrealMcpResourceProvider() = default;

	/** The modular-feature name every resource provider registers under. Stable across the plugin's lifetime. */
	static FName GetModularFeatureName()
	{
		return FName(TEXT("UnrealMcpResourceProvider"));
	}

	/**
	 * Stable, unique extension identifier — reverse-DNS recommended (e.g. "com.foo.niagara-ai"). Used as
	 * the deterministic sort key and as the manifest's per-resource extensionId. Two providers with the
	 * same id is an authoring error (the second's duplicate resources are rejected).
	 */
	virtual FString GetExtensionId() const = 0;

	/** Human-readable name shown in the extensions UI row (§7). */
	virtual FText GetDisplayName() const = 0;

	/** Free-form version string of the extension (independent of the Unreal-MCP plugin version). */
	virtual FString GetExtensionVersion() const = 0;

	/**
	 * Declare the extension's resources into @p Registry using the fluent FUnrealMcpResourceBuilder
	 * (Registry.Resource("unreal://my/uri").Name(...).MimeType(...).Read(...)). Called on the game thread.
	 * Resources are automatically stamped with this provider's ExtensionId — do not call .ExtensionId().
	 * Invalid or duplicate entries are dropped/rejected and recorded; valid entries register normally.
	 */
	virtual void RegisterResources(FUnrealMcpResourceRegistry& Registry) = 0;
};
