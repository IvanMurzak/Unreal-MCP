// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntime.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Config/UnrealMcpConfig.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"
#include "Bridge/UnrealMcpBridgeServer.h"
#include "Sidecar/UnrealMcpSidecarManager.h"

#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformMisc.h"
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

	// §8 connection config: parse the project-root .env once, export UNREAL_MCP_BRIDGE_PATH into the process
	// env (only-if-absent, so process env still wins) so a GUI-launched editor's .env can feed the dev
	// sidecar binary path. Then resolve the config (process env > .env > file > defaults) from that same
	// parsed .env. The resolved EFFECTIVE connection config (incl. host/cloudUrl/token) is handed to the
	// bridge for the §1.3 `config` push — the sidecar never re-resolves it (§1.5), so HOST/CLOUD_URL/TOKEN
	// are deliberately NOT exported to the process env.
	const TMap<FString, FString> DotEnv = FUnrealMcpConfig::LoadEnvFile(FUnrealMcpConfig::DefaultEnvFilePath());
	FUnrealMcpConfig::ExportDotEnvToProcessEnv(DotEnv);

	const FUnrealMcpConfig Config = FUnrealMcpConfig::LoadAndResolve(DotEnv);

	const TSharedPtr<FJsonObject> EffectiveConfig = Config.BuildEffectiveConnectionConfig();
	// Token is NEVER logged at any level (§8) — log the shape with the bearer masked.
	UE_LOG(LogUnrealMcp, Log,
		TEXT("[Unreal-MCP] connection config resolved (mode=%s, host=%s, cloudUrl=%s, token=%s, keepConnected=%s)."),
		Config.ConnectionMode == EUnrealMcpConnectionMode::Cloud ? TEXT("Cloud") : TEXT("Custom"),
		*Config.ResolveCustomHost(), *Config.ResolveCloudBaseUrl(),
		*FUnrealMcpConfig::MaskSecret(Config.ResolveEffectiveToken()),
		Config.bKeepConnected ? TEXT("true") : TEXT("false"));

	// Generate the one-shot IPC token ONCE; both the server (validates the handshake) and the sidecar
	// manager (delivers it over stdin) must agree on it (§1.4).
	const FString Token = FUnrealMcpSidecarManager::GenerateToken();

	const int32 BoundPort = BridgeServer->Start(Token, ProjectPath, PluginVersion, EngineVersion, EffectiveConfig);
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
