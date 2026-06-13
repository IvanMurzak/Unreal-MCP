// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Agents/AiAgentConfigurator.h"

class FUnrealMcpEditorViewModel;
class SDockTab;
class FSpawnTabArgs;
class FTabManager;
class FWorkspaceItem;

/**
 * Registers the "AI Game Developer" main window (docs/ARCHITECTURE.md §7) as a nomad dockable tab with
 * FGlobalTabmanager and a Window-menu entry (via a WorkspaceMenuStructure group). Owned by the runtime;
 * Register() runs in StartupModule (after the bridge/view-model exist) and Unregister() in Shutdown so the
 * tab spawner never dangles past module unload.
 *
 * The tab content (SUnrealMcpMainWindow) binds to the shared view-model passed at Register() time. The
 * optional providers feed the §7 bridge-status line and the Restart-bridge action without coupling the
 * widget to the runtime's subsystems directly.
 */
class FUnrealMcpMainWindowTab
{
public:
	/** The nomad tab id (§7). */
	static const FName TabId;

	/**
	 * Register the nomad tab spawner + Window-menu entry. @p InViewModel is the shared state model the tab
	 * content binds to (must outlive the tab — the runtime owns it). @p InPluginVersion seeds the header.
	 * Idempotent: a second call is a no-op.
	 */
	void Register(
		const TSharedRef<FUnrealMcpEditorViewModel>& InViewModel,
		const FString& InPluginVersion,
		FSimpleDelegate InOnRestartBridge,
		TFunction<FString()> InBridgeStatusProvider,
		TFunction<FAiAgentConnectionInfo()> InConnectionInfoProvider);

	/** Unregister the tab spawner (Shutdown). Idempotent. */
	void Unregister();

private:
	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);

	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	FString PluginVersion;
	FSimpleDelegate OnRestartBridge;
	TFunction<FString()> BridgeStatusProvider;
	TFunction<FAiAgentConnectionInfo()> ConnectionInfoProvider;
	bool bRegistered = false;
};
