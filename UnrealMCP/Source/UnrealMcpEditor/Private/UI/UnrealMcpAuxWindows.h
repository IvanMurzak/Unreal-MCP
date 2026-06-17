// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UI/SUnrealMcpToolsWindow.h"
#include "UI/SUnrealMcpFeatureListWindow.h"

class FUnrealMcpEditorViewModel;
class SDockTab;
class FSpawnTabArgs;

/**
 * Registers the §7 auxiliary windows (docs/ARCHITECTURE.md §7) as nomad dockable tabs under the same
 * "Window → Tools" group as the main window. Connection settings are NOT an aux window — they live in the
 * single "AI Game Developer" main window (issue #107, Unity-MCP parity); the former standalone
 * "AI Game Developer Settings" tab + its "Project → Plugins → AI Game Developer" ISettingsModule section
 * were removed.
 *
 *   - UnrealMcpToolsWindow     — per-tool enable/disable (drives the §8 enable-map + manifest exclusion).
 *   - UnrealMcpPromptsWindow   — MCP prompts (honest empty state until a prompts feed lands).
 *   - UnrealMcpResourcesWindow — MCP resources (honest empty state).
 *   - UnrealMcpSerializationCheckWindow — serialize a picked UObject/Actor to JSON (Unity-MCP parity); opened
 *     by the main window's footer "Check" button via the static TryInvokeSerializationCheckTab().
 *
 * Dedup/focus (the known Godot [low] fixed here): nomad tabs are owned by FGlobalTabmanager, which keeps a
 * single live tab per id and FOCUSES the existing one when the menu entry is invoked again — invoking a
 * window's menu entry twice never errors, it brings the open tab forward.
 *
 * Lifetime mirrors FUnrealMcpMainWindowTab: Register() runs in StartupModule (after the view-model + registry
 * exist), Unregister() in Shutdown — and Unregister CLOSES any live tab first so the hosted widget (which holds
 * a strong view-model ref) is torn down before the runtime frees the view-model/bridge.
 */
class FUnrealMcpAuxWindows
{
public:
	static const FName ToolsTabId;
	static const FName PromptsTabId;
	static const FName ResourcesTabId;
	static const FName SerializationCheckTabId;

	/**
	 * Open (or focus, if already open) the Serialization Check tab. Static so the footer "Check" button and the
	 * dev-control `check` action can both drive it without holding an FUnrealMcpAuxWindows instance. Guards on
	 * FSlateApplication::IsInitialized() so a headless / -nullrhi run is a safe no-op. Returns true when a live
	 * tab was invoked (false when Slate is unavailable). Game-thread only — TryInvokeTab pumps Slate.
	 */
	static bool TryInvokeSerializationCheckTab();

	/**
	 * Register the nomad tabs. @p InViewModel is the shared state model (must outlive the windows — the runtime
	 * owns it). @p InToolListProvider snapshots the registry's tool set for the Tools window;
	 * @p InPromptProvider / @p InResourceProvider feed the Prompts / Resources lists (may be unset → empty
	 * state). Idempotent: a second call is a no-op.
	 */
	void Register(
		const TSharedRef<FUnrealMcpEditorViewModel>& InViewModel,
		TFunction<TArray<FUnrealMcpToolListEntry>()> InToolListProvider,
		TFunction<TArray<FUnrealMcpFeatureEntry>()> InPromptProvider,
		TFunction<TArray<FUnrealMcpFeatureEntry>()> InResourceProvider);

	/** Unregister the tab spawners (Shutdown). Idempotent. Neutralizes the providers first. */
	void Unregister();

	/**
	 * Flip the shared alive-flag the Tools provider closes over, so any widget that outlives Unregister()
	 * (a deferred RequestCloseTab still queued when the runtime frees the registry/bridge) short-circuits its
	 * provider to a safe empty result instead of dereferencing freed memory. Called from Unregister(), which the
	 * runtime invokes BEFORE the BridgeServer/Registry resets. Idempotent; safe before Register(). Game-thread only.
	 */
	void NeutralizeProviders();

private:
	TSharedRef<SDockTab> SpawnToolsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnPromptsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnResourcesTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnSerializationCheckTab(const FSpawnTabArgs& Args);

	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	TFunction<TArray<FUnrealMcpToolListEntry>()> ToolListProvider;
	TFunction<TArray<FUnrealMcpFeatureEntry>()> PromptProvider;
	TFunction<TArray<FUnrealMcpFeatureEntry>()> ResourceProvider;
	// Shared with every widget-held copy of the registry-touching provider; NeutralizeProviders() flips it
	// false during teardown so a surviving widget's next paint returns empty rather than dereferencing freed memory.
	TSharedPtr<bool> ProvidersAlive;
	bool bRegistered = false;
};
