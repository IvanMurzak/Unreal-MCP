// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FAiAgentConfigurator;

/**
 * Ordered registry of the available AI-agent configurators (docs/ARCHITECTURE.md §7) — the C++ analog of Unity's
 * AiAgentConfiguratorRegistry and Godot's GodotAgentConfiguratorRegistry. Phase A registers the two reference
 * agents (Claude Code, Cursor); Phase B fills in the rest and a Custom entry that is kept LAST (the Custom-last
 * invariant the registry guarantees regardless of insertion order, mirroring the siblings).
 *
 * Each call to MakeDefault() returns a fresh, independent set of configurator instances (they cache per-binding
 * config state, so the Slate panel and a spec each want their own). Lookups by id / index / name are provided for
 * the dropdown (index) and persistence (id).
 */
class FAiAgentConfiguratorRegistry
{
public:
	/** Build the default ordered configurator set (alphabetical by name; a Custom entry, when present, stays last). */
	static UNREALMCPEDITOR_API TArray<TSharedRef<FAiAgentConfigurator>> MakeDefault();

	/** Construct around a supplied set (the production set or a spec's set). The registry does not own creation. */
	UNREALMCPEDITOR_API explicit FAiAgentConfiguratorRegistry(TArray<TSharedRef<FAiAgentConfigurator>> InConfigurators);
	/** Convenience: construct around MakeDefault(). */
	UNREALMCPEDITOR_API FAiAgentConfiguratorRegistry();

	const TArray<TSharedRef<FAiAgentConfigurator>>& All() const { return Configurators; }
	UNREALMCPEDITOR_API int32 Num() const;

	/** All display names in registry order (for the dropdown). */
	UNREALMCPEDITOR_API TArray<FString> GetAgentNames() const;
	/** All agent ids in registry order. */
	UNREALMCPEDITOR_API TArray<FString> GetAgentIds() const;

	/** The configurator with @p AgentId, or null. */
	UNREALMCPEDITOR_API TSharedPtr<FAiAgentConfigurator> GetByAgentId(const FString& AgentId) const;
	/** The configurator at @p Index, or null when out of range. */
	UNREALMCPEDITOR_API TSharedPtr<FAiAgentConfigurator> GetByIndex(int32 Index) const;
	/** The registry index of @p AgentId, or INDEX_NONE. */
	UNREALMCPEDITOR_API int32 GetIndexByAgentId(const FString& AgentId) const;

private:
	TArray<TSharedRef<FAiAgentConfigurator>> Configurators;
};
