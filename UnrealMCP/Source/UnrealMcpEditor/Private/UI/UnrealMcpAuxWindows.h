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
 * Registers the four §7 auxiliary windows (docs/ARCHITECTURE.md §7) as nomad dockable tabs under the same
 * "Window → Tools" group as the main window, plus the Settings page ALSO as a "Project → Plugins → AI Game
 * Developer" ISettingsModule section (UE users expect Project Settings discoverability):
 *
 *   - UnrealMcpToolsWindow     — per-tool enable/disable (drives the §8 enable-map + manifest exclusion).
 *   - UnrealMcpPromptsWindow   — MCP prompts (honest empty state until a prompts feed lands).
 *   - UnrealMcpResourcesWindow — MCP resources (honest empty state).
 *   - UnrealMcpSettingsWindow  — §8 connection settings (also the ISettingsModule section).
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
	static const FName SettingsTabId;

	/**
	 * Register the four nomad tabs + the ISettingsModule settings section. @p InViewModel is the shared state
	 * model (must outlive the windows — the runtime owns it). @p InToolListProvider snapshots the registry's
	 * tool set for the Tools window; @p InPortStatusProvider yields the read-only IPC-port line for Settings;
	 * @p InPromptProvider / @p InResourceProvider feed the Prompts / Resources lists (may be unset → empty
	 * state). Idempotent: a second call is a no-op.
	 */
	void Register(
		const TSharedRef<FUnrealMcpEditorViewModel>& InViewModel,
		TFunction<TArray<FUnrealMcpToolListEntry>()> InToolListProvider,
		TFunction<FString()> InPortStatusProvider,
		TFunction<TArray<FUnrealMcpFeatureEntry>()> InPromptProvider,
		TFunction<TArray<FUnrealMcpFeatureEntry>()> InResourceProvider);

	/** Unregister the tab spawners + the settings section (Shutdown). Idempotent. Neutralizes the providers first. */
	void Unregister();

	/**
	 * Flip the shared alive-flag the Tools/Settings providers close over, so any widget that outlives Unregister()
	 * (a deferred RequestCloseTab still queued when the runtime frees the registry/bridge) short-circuits its
	 * provider to a safe empty result instead of dereferencing freed memory. Called from Unregister(), which the
	 * runtime invokes BEFORE the BridgeServer/Registry resets. Idempotent; safe before Register(). Game-thread only.
	 */
	void NeutralizeProviders();

private:
	TSharedRef<SDockTab> SpawnToolsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnPromptsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnResourcesTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnSettingsTab(const FSpawnTabArgs& Args);

	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	TFunction<TArray<FUnrealMcpToolListEntry>()> ToolListProvider;
	TFunction<FString()> PortStatusProvider;
	TFunction<TArray<FUnrealMcpFeatureEntry>()> PromptProvider;
	TFunction<TArray<FUnrealMcpFeatureEntry>()> ResourceProvider;
	// Shared with every widget-held copy of the registry/bridge-touching providers; NeutralizeProviders() flips it
	// false during teardown so a surviving widget's next paint returns empty rather than dereferencing freed memory.
	TSharedPtr<bool> ProvidersAlive;
	bool bRegistered = false;
	bool bSettingsRegistered = false;
};
