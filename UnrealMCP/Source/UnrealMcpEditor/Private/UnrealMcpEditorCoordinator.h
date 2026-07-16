// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;
class FUnrealMcpPromptRegistry;
class FUnrealMcpResourceRegistry;
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

	/**
	 * Handle a sidecar `project-config-result` (mcp-authorize PR 4, design 04/06): cache the DERIVED per-project
	 * local-server port (ProjectIdentity + marker portOverride) and, on the FIRST result, reattach a survivor local
	 * server on it (Custom+http only). Runs on the game thread (the bridge status sink marshals onto it). A no-op on
	 * an `ok == false` / invalid-port result. Public so the wiring lambda routes to it (see Startup).
	 */
	void ApplyProjectConfigResult(const TSharedPtr<class FJsonObject>& Message);

	/**
	 * Handle a sidecar `server-launch-args-result` (mcp-authorize g5/g6 consolidation): the SHARED
	 * ServerLaunchArguments builder composed the local-server launch-arg string, so now execute the pending
	 * Start / reattach for the correlated requestId. Runs on the game thread (the bridge status sink marshals onto
	 * it). A no-op on an `ok == false` / unknown-requestId result. Public so the wiring lambda routes to it.
	 */
	void ApplyServerLaunchArgsResult(const TSharedPtr<class FJsonObject>& Message);

	FUnrealMcpToolRegistry* GetRegistry() const { return Registry.Get(); }
	FUnrealMcpBridgeServer* GetBridgeServer() const { return BridgeServer.Get(); }
	FUnrealMcpExtensionManager* GetExtensionManager() const { return ExtensionManager.Get(); }

private:
	TUniquePtr<FUnrealMcpToolRegistry> Registry;
	// §A.1 prompt registry (P1): built alongside the tool registry on the SAME Model A path.
	TUniquePtr<FUnrealMcpPromptRegistry> PromptRegistry;
	// §A.1 resource registry (P2): built alongside the tool/prompt registries on the SAME Model A path.
	TUniquePtr<FUnrealMcpResourceRegistry> ResourceRegistry;
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

	// Teardown alive-flag guarding the embedded Extensions panel's InstalledProvider (issue #179). The provider
	// captures the raw FUnrealMcpExtensionManager*; a deferred main-window paint (a queued RequestCloseTab) could
	// otherwise fire after Shutdown frees the extension manager. Flipped false at the START of Shutdown() — before
	// the main-window tab close and the ExtensionManager reset — so any surviving widget read returns empty. This
	// mirrors FUnrealMcpAuxWindows::ProvidersAlive for the Tools provider (the lifetime owner holds the flag here).
	TSharedPtr<bool> ExtProvidersAlive;

	// DEV-ONLY inject/control HTTP bridge over the live dock (docs/ARCHITECTURE.md §7). Created + started in
	// Startup() ONLY when the editor process env UNREAL_MCP_DEV_CONTROL == "1"; null (never listening) otherwise.
	TUniquePtr<FUnrealMcpDevControlServer> DevControlServer;

	// mcp-authorize PR 4 (design 04/06): the deterministic per-project LOCAL-server port the sidecar derives
	// (McpPlugin ProjectIdentity + the project marker's portOverride) and delivers over IPC on each handshake.
	// -1 until the first `project-config-result` arrives. Replaces the fixed-8080 ParsePortFromHost default for
	// the local `gamedev-mcp-server` start. Game-thread only (written from the game-thread-marshalled bridge
	// status sink, read by the OnStartLocalServer sink), so no lock is needed.
	int32 DerivedLocalServerPort = -1;
	// mcp-authorize g5/g6: the sidecar-derived routing pin (first 8 hex of the ProjectIdentity SHA-256), cached
	// alongside the port from the same `project-config-result`. Used to compose the oauth-mode `public-url`
	// (http://localhost:<port>/mcp/p/<pin>) forwarded to the sidecar's launch-arg builder. Empty until the first result.
	FString DerivedLocalServerPin;
	// Guards the one-time survivor reattach: the reattach that used to run in Startup now runs when the first
	// derived port arrives (the port is unknown until the sidecar handshakes) and must not repeat on a reconnect.
	bool bLocalServerReattachAttempted = false;

	// mcp-authorize g5/g6 consolidation: in-flight `server-launch-args` requests keyed by requestId. The
	// OnStartLocalServer sink (and the one-time reattach) send an IPC request for the composed launch args and
	// record the {port, bReattach} here; ApplyServerLaunchArgsResult pops the entry and runs Start/Reattach with the
	// returned string. Game-thread only (both the send and the marshalled result run there) — no lock needed.
	struct FPendingServerLaunch { int32 Port = -1; bool bReattach = false; };
	TMap<FString, FPendingServerLaunch> PendingServerLaunches;

	// Compose + send a `server-launch-args` IPC request for @p Live's resolved auth mode and, on the async result,
	// Start (or, when @p bReattach, ReattachIfRunning) the local server on @p Port with the sidecar-built args.
	void RequestServerLaunchArgs(int32 Port, int32 PluginTimeoutMs, const class FUnrealMcpConfig& Live, bool bReattach);

	FDelegateHandle PreExitHandle;
	bool bStarted = false;
};
