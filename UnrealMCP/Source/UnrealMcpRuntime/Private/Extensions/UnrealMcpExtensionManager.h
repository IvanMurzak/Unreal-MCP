// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "Templates/Function.h"
#include "UObject/NameTypes.h"

class FUnrealMcpToolRegistry;
class IUnrealMcpToolProvider;
class IModularFeature;

/**
 * A single discovered extension's public-facing record (docs/ARCHITECTURE.md §5 / §7 registry API row).
 * The Slate UI (§7, later task) renders one row per record: id, display name, version, tool count,
 * an enable toggle, and an error badge when HasError().
 */
struct FUnrealMcpExtensionRecord
{
	FString Id;
	FText DisplayName;
	FString Version;
	int32 ToolCount = 0;
	bool bEnabled = true;
	/** Concatenated per-entry validation/dedup failures for this extension; empty when healthy. */
	FString Error;

	bool HasError() const { return !Error.IsEmpty(); }
};

/**
 * Discovers IUnrealMcpToolProvider modular features, merges their tools into the plugin's single
 * FUnrealMcpToolRegistry in deterministic order (sorted by ExtensionId), isolates invalid/duplicate
 * entries (§5), and persists per-extension enable state. On every change it rebuilds the registry and
 * fires OnChanged so the bridge re-pushes the manifest (§2.2 hot-reload).
 *
 * Threading: all rebuilds run on the game thread. Initial discovery happens during plugin StartupModule;
 * late register/unregister events arrive on the game thread (module load/unload), as do UI toggles (§7).
 *
 * Persistence: disabledExtensions[] is stored in a minimal standalone JSON file under the project's
 * Saved dir. TODO(connection-config §8): fold this into FUnrealMcpConfig once that store lands, so the
 * extension enable state shares one config surface with connection settings.
 */
class UNREALMCPRUNTIME_API FUnrealMcpExtensionManager
{
public:
	/**
	 * @param InRegistry   the plugin-owned registry to merge extension tools into (core tools must already be registered).
	 * @param InOnChanged  fired after every rebuild so the owner can re-push the manifest (no-op when disconnected).
	 * @param InConfigPath absolute path of the disabledExtensions[] JSON file; empty => default under the project Saved dir.
	 */
	FUnrealMcpExtensionManager(FUnrealMcpToolRegistry& InRegistry, TFunction<void()> InOnChanged, const FString& InConfigPath = FString());
	~FUnrealMcpExtensionManager();

	/** Load persisted disabled set, subscribe to modular-feature events, and run the initial rebuild. */
	void Startup();
	/** Unsubscribe from modular-feature events (idempotent). */
	void Shutdown();

	/** Discovered extensions, sorted by Id. */
	const TArray<FUnrealMcpExtensionRecord>& GetExtensions() const { return Records; }
	const FUnrealMcpExtensionRecord* FindExtension(const FString& Id) const;

	/** True when @p Id is NOT in the persisted disabled set. */
	bool IsExtensionEnabled(const FString& Id) const { return !DisabledExtensions.Contains(Id); }

	/**
	 * Enable/disable an extension: updates + persists the disabled set, then rebuilds and notifies.
	 * A disabled extension contributes NO tools to the registry/manifest (§5, §D).
	 */
	void SetExtensionEnabled(const FString& Id, bool bEnabled);

	/** Rebuild from an explicit provider list — deterministic, no global IModularFeatures (test seam). */
	void RebuildFromProviders(const TArray<IUnrealMcpToolProvider*>& Providers, bool bNotify = true);

	/** Override the provider source used by Startup/SetExtensionEnabled rebuilds (test seam; default = live discovery). */
	void SetProviderSourceForTesting(TFunction<TArray<IUnrealMcpToolProvider*>()> InSource) { ProviderSource = MoveTemp(InSource); }

private:
	/** Rebuild from the current ProviderSource (live IModularFeatures discovery by default). */
	void Rebuild(bool bNotify);

	TArray<IUnrealMcpToolProvider*> GatherProviders() const;

	void OnFeatureRegistered(const FName& Type, IModularFeature* Feature);
	void OnFeatureUnregistered(const FName& Type, IModularFeature* Feature);

	void LoadConfig();
	void SaveConfig() const;

	FUnrealMcpToolRegistry& Registry;
	TFunction<void()> OnChanged;
	FString ConfigPath;
	TFunction<TArray<IUnrealMcpToolProvider*>()> ProviderSource;

	TArray<FUnrealMcpExtensionRecord> Records;
	TSet<FString> DisabledExtensions;
	/** Extension ids that currently contributed tools, so a rebuild can remove exactly those before re-registering. */
	TArray<FString> RegisteredExtensionIds;

	FDelegateHandle RegisteredHandle;
	FDelegateHandle UnregisteredHandle;
	bool bSubscribed = false;

	// Re-entrancy guard (§5): a provider's RegisterTools may synchronously (un)register a modular feature,
	// which fires OnFeatureRegistered → Rebuild while we are still mid-rebuild. Rather than recurse (which
	// would re-enter the registry's extension scope, trip its no-nested-scope check, and could free a
	// provider the outer Sorted loop still iterates), we DEFER: the nested call records a pending rebuild
	// and returns immediately; the outer rebuild re-runs once after it completes against the fresh source.
	bool bRebuilding = false;
	bool bPendingRebuild = false;
	bool bPendingNotify = false;
};
