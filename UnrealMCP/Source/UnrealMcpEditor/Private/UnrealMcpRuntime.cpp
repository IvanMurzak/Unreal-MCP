// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntime.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"
#include "Bridge/UnrealMcpBridgeServer.h"
#include "Sidecar/UnrealMcpSidecarManager.h"

#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Interfaces/IPluginManager.h"

void FUnrealMcpRuntime::Startup()
{
	if (bStarted)
		return;
	bStarted = true;

	Registry = MakeUnique<FUnrealMcpToolRegistry>();
	UnrealMcpPingTool::Register(*Registry);

	Dispatcher = MakeUnique<FUnrealMcpGameThreadDispatcher>();
	BridgeServer = MakeUnique<FUnrealMcpBridgeServer>(*Registry, *Dispatcher);

	const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString EngineVersion = FEngineVersion::Current().ToString();

	FString PluginVersion = TEXT("0.1.0");
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP")))
		PluginVersion = Plugin->GetDescriptor().VersionName;

	// Generate the one-shot IPC token ONCE; both the server (validates the handshake) and the sidecar
	// manager (delivers it over stdin) must agree on it (§1.4).
	const FString Token = FUnrealMcpSidecarManager::GenerateToken();

	const int32 BoundPort = BridgeServer->Start(Token, ProjectPath, PluginVersion, EngineVersion);
	if (BoundPort <= 0)
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] bridge server failed to start; sidecar not launched."));
		return;
	}

	SidecarManager = MakeUnique<FUnrealMcpSidecarManager>();
	SidecarManager->StartForPort(BoundPort, Token);

	// §1.5: tear down on editor pre-exit so the sidecar never orphans (layer 1).
	PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([this]() { Shutdown(); });
}

void FUnrealMcpRuntime::Shutdown()
{
	if (!bStarted)
		return;
	bStarted = false;

	if (PreExitHandle.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		PreExitHandle.Reset();
	}

	// §1.5 graceful teardown ordering. Doing SidecarManager->Stop() (TerminateProc KillTree) BEFORE the
	// bridge sends its `shutdown` Bye would mean the graceful path only ever worked for a MANUALLY launched
	// sidecar — a spawned one would be killed before the Bye. So: (1) stop auto-restarts (the bridge's Bye
	// must not be undone by a relaunch), (2) let the bridge send `shutdown` to the connected sidecar, (3)
	// give the child a bounded grace to self-exit, (4) terminate as a backstop.
	if (SidecarManager.IsValid())
		SidecarManager->StopRestarts();

	if (BridgeServer.IsValid())
	{
		BridgeServer->Shutdown(); // sends the §1.5 `shutdown` Bye to the connected sidecar, then tears down
		BridgeServer.Reset();
	}

	if (SidecarManager.IsValid())
	{
		SidecarManager->WaitForExit(FTimespan::FromSeconds(3)); // bounded grace for a clean self-exit
		SidecarManager->Stop();                                 // backstop: TerminateProc if still alive
		SidecarManager.Reset();
	}

	Dispatcher.Reset();
	Registry.Reset();
}
