// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;
class FUnrealMcpGameThreadDispatcher;
class FUnrealMcpBridgeServer;
class FUnrealMcpSidecarManager;
class FUnrealMcpServerManager;
class FUnrealMcpExtensionManager;
class FUnrealMcpEditorViewModel;
class FUnrealMcpMainWindowTab;
class FUnrealMcpAuxWindows;
class FUnrealMcpDevControlServer;

/**
 * Plugin-lifetime EDITOR coordinator (docs/ARCHITECTURE.md §0, §12.3 Model A). Builds the runtime-owned
 * subsystems (tool registry + core tools, the game-thread dispatcher §4, the IPC bridge server §1, the
 * sidecar manager §6 — all now living in the UnrealMcpRuntime module) and layers the editor-only families
 * + Slate UI + local-server manager + dev-control bridge on top of the SAME registry. Owned by the editor
 * module; started in StartupModule and torn down on editor pre-exit / ShutdownModule so the sidecar never
 * orphans (§6 layer 1). Installs the §12.6 EDITOR world resolver into FUnrealMcpWorldProvider on Startup.
 *
 * Renamed from the misleading `FUnrealMcpRuntime` (it was always the editor coordinator, never a runtime
 * one — §12 executive finding 3). The actual runtime bootstrap is UUnrealMcpRuntimeSubsystem, landing in R3.
 */
class FUnrealMcpEditorCoordinator
{
public:
	// Declared (not defaulted inline) and defined in the .cpp so the TUniquePtr deleters for the
	// forward-declared subsystem members are instantiated where those types are complete. Without this,
	// a non-unity / adaptive build of UnrealMcpEditorModule.cpp (which only sees the forward decls)
	// fails with C4150 "deletion of pointer to incomplete type".
	FUnrealMcpEditorCoordinator();
	~FUnrealMcpEditorCoordinator();

	void Startup();
	void Shutdown();

	FUnrealMcpToolRegistry* GetRegistry() const { return Registry.Get(); }
	FUnrealMcpBridgeServer* GetBridgeServer() const { return BridgeServer.Get(); }
	FUnrealMcpExtensionManager* GetExtensionManager() const { return ExtensionManager.Get(); }

private:
	TUniquePtr<FUnrealMcpToolRegistry> Registry;
	TUniquePtr<FUnrealMcpGameThreadDispatcher> Dispatcher;
	TUniquePtr<FUnrealMcpBridgeServer> BridgeServer;
	TUniquePtr<FUnrealMcpSidecarManager> SidecarManager;
	// §7 in-UI local-server (issue #95): owns the LOCAL gamedev-mcp-server process (Custom+http only), the
	// plugin-side analog of Unity's McpServerManager. Distinct from SidecarManager (the always-on bridge).
	// Force-stopped in Shutdown so no gamedev-mcp-server orphans on editor close.
	TUniquePtr<FUnrealMcpServerManager> ServerManager;
	TUniquePtr<FUnrealMcpExtensionManager> ExtensionManager;

	// §7 UI: the main-window view-model (shared so the nomad tab's widget binds to it) and its tab spawner,
	// plus the four §7 auxiliary windows (MCP Tools / Prompts / Resources / Settings) sharing the same view-model.
	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	TUniquePtr<FUnrealMcpMainWindowTab> MainWindowTab;
	TUniquePtr<FUnrealMcpAuxWindows> AuxWindows;

	// DEV-ONLY inject/control HTTP bridge over the live dock (docs/ARCHITECTURE.md §7). Created + started in
	// Startup() ONLY when the editor process env UNREAL_MCP_DEV_CONTROL == "1"; null (never listening) otherwise.
	TUniquePtr<FUnrealMcpDevControlServer> DevControlServer;

	FDelegateHandle PreExitHandle;
	bool bStarted = false;
};
