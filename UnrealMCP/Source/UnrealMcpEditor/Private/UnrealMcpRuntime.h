// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;
class FUnrealMcpGameThreadDispatcher;
class FUnrealMcpBridgeServer;
class FUnrealMcpSidecarManager;
class FUnrealMcpExtensionManager;

/**
 * Plugin-lifetime coordinator (docs/ARCHITECTURE.md §0). Wires the four subsystems together: the tool
 * registry (+ core tools), the game-thread dispatcher (§4), the IPC bridge server (§1), and the sidecar
 * manager (§6). Owned by the editor module; started in StartupModule and torn down on editor pre-exit /
 * ShutdownModule so the sidecar never orphans (§6 layer 1).
 */
class FUnrealMcpRuntime
{
public:
	// Declared (not defaulted inline) and defined in the .cpp so the TUniquePtr deleters for the
	// forward-declared subsystem members are instantiated where those types are complete. Without this,
	// a non-unity / adaptive build of UnrealMcpEditorModule.cpp (which only sees the forward decls)
	// fails with C4150 "deletion of pointer to incomplete type".
	FUnrealMcpRuntime();
	~FUnrealMcpRuntime();

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
	TUniquePtr<FUnrealMcpExtensionManager> ExtensionManager;

	FDelegateHandle PreExitHandle;
	bool bStarted = false;
};
