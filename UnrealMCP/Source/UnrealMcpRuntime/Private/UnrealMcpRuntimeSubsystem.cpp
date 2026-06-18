// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntimeSubsystem.h"
#include "UnrealMcpRuntimeSettings.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpRuntimeCoreTools.h"
#include "UnrealMcpRuntimeCorePrompts.h"
#include "UnrealMcpRuntimeCoreResources.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpPromptRegistry.h"
#include "UnrealMcpResourceRegistry.h"
#include "Config/UnrealMcpConfig.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"
#include "Bridge/UnrealMcpBridgeServer.h"
#include "Sidecar/UnrealMcpSidecarManager.h"
#include "Extensions/UnrealMcpExtensionManager.h"
#include "Tools/UnrealMcpWorldProvider.h"
#include "IUnrealMcpToolProvider.h"
#include "IUnrealMcpPromptProvider.h"
#include "IUnrealMcpResourceProvider.h"

#include "Features/IModularFeatures.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"

// §12.8 #3: build-config gating. The bUnrealMcpAllowShipping Build.cs flag defines this to 0 (default) or 1.
// With 0, Connect() in a Shipping build logs + returns false. Defaulted here so a build that predates the
// Build.cs edit (or a foreign target) still compiles with the conservative "off" value.
#ifndef UNREAL_MCP_ALLOW_SHIPPING
#define UNREAL_MCP_ALLOW_SHIPPING 0
#endif

// The runtime-owned machinery, held behind the subsystem's TPimplPtr (see the header note). Defined here,
// where every member type is complete; TPimplPtr captures a typed deleter at MakePimpl time so destruction
// is correct even though the header only forward-declares FRuntimeImpl.
struct UUnrealMcpRuntimeSubsystem::FRuntimeImpl
{
	TUniquePtr<FUnrealMcpToolRegistry> Registry;
	TUniquePtr<FUnrealMcpPromptRegistry> PromptRegistry;
	TUniquePtr<FUnrealMcpResourceRegistry> ResourceRegistry;
	TUniquePtr<FUnrealMcpGameThreadDispatcher> Dispatcher;
	TUniquePtr<FUnrealMcpBridgeServer> BridgeServer;
	TUniquePtr<FUnrealMcpSidecarManager> SidecarManager;
	TUniquePtr<FUnrealMcpExtensionManager> ExtensionManager;
};

void UUnrealMcpRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// §12.6: install the RUNTIME world resolver BEFORE any tool body can run, so a runtime tool resolves the
	// live game world (not the editor world). Held weakly via `this` — the subsystem outlives every tool call
	// (it owns the bridge), and Deinitialize clears the resolver, so the captured `this` never dangles.
	FUnrealMcpWorldProvider::SetWorldResolver([this]() -> UWorld*
	{
		const UGameInstance* GI = GetGameInstance();
		return GI ? GI->GetWorld() : nullptr;
	});

	// Build the runtime-owned machinery (mirrors FUnrealMcpEditorCoordinator, §12.3 Model A) — registry +
	// runtime-safe families + dispatcher + bridge. Editor-only families are NOT registered here (this path
	// runs in a packaged game where they would not compile/link). R4 (§12.7) registers the full runtime-safe
	// set: ping, actor/component, the console+reflection subset, the screenshot subset, and level-get-data.
	Impl = MakePimpl<FRuntimeImpl>();
	Impl->Registry = MakeUnique<FUnrealMcpToolRegistry>();
	UnrealMcpPingTool::Register(*Impl->Registry);
	UnrealMcpActorTools::Register(*Impl->Registry); // §10 actor / component family (World->SpawnActor runtime branch)
	UnrealMcpConsoleReflectionTools::Register(*Impl->Registry); // §10 console + reflection runtime subset
	UnrealMcpRuntimeScreenshotTools::Register(*Impl->Registry); // §10 screenshot runtime subset (game-view, camera)
	UnrealMcpRuntimeLevelTools::Register(*Impl->Registry); // §10 read-only level-get-data

	// §A.1 prompt registry (P1): the prompt sibling of the tool registry, built on the SAME Model A path. The
	// core prompt family registers before the bridge arms so the first prompt-manifest a v2 sidecar reads on
	// handshake already includes it. No §7/§8 prompt filters here (the runtime path applies none for tools either).
	Impl->PromptRegistry = MakeUnique<FUnrealMcpPromptRegistry>();
	UnrealMcpCorePrompts::Register(*Impl->PromptRegistry);

	// §A.1 resource registry (P2): the resource sibling of the tool/prompt registries, built on the SAME Model A
	// path. The core resource family registers before the bridge arms so the first resource-manifest a v2 sidecar
	// reads on handshake already includes it. No §7/§8 resource filters here (the runtime path applies none).
	Impl->ResourceRegistry = MakeUnique<FUnrealMcpResourceRegistry>();
	UnrealMcpCoreResources::Register(*Impl->ResourceRegistry);

	Impl->Dispatcher = MakeUnique<FUnrealMcpGameThreadDispatcher>();
	Impl->BridgeServer = MakeUnique<FUnrealMcpBridgeServer>(
		*Impl->Registry, *Impl->Dispatcher, Impl->PromptRegistry.Get(), Impl->ResourceRegistry.Get());

	// Discover §5 extension providers and merge them BEFORE the bridge arms, so the first manifest a sidecar
	// reads on a later Connect already includes them; a late register/unregister re-pushes the manifest.
	// §A.1 kind-aware OnChanged: a rebuild re-pushes ALL THREE manifests (tool + prompt + resource). The
	// prompt/resource pushes are scaffold no-ops until P1/P2 wire those registries (and are gated on a
	// v2-negotiated link); firing all three keeps the change contract kind-uniform with the editor coordinator.
	Impl->ExtensionManager = MakeUnique<FUnrealMcpExtensionManager>(
		*Impl->Registry,
		[this]()
		{
			if (Impl && Impl->BridgeServer.IsValid())
			{
				Impl->BridgeServer->PushManifest();
				Impl->BridgeServer->PushPromptManifest();
				Impl->BridgeServer->PushResourceManifest();
			}
		},
		/*InConfigPath*/ FString(),
		Impl->PromptRegistry.Get(), // §A.2 kind-aware: also merge prompt-provider extensions into the prompt registry
		Impl->ResourceRegistry.Get()); // §A.2 kind-aware: also merge resource-provider extensions into the resource registry
	Impl->ExtensionManager->Startup();

	const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString EngineVersion = FEngineVersion::Current().ToString();

	FString PluginVersion = TEXT("0.1.0");
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP")))
		PluginVersion = Plugin->GetDescriptor().VersionName;

	// Generate the one-shot IPC token ONCE (§1.4): the bridge validates the handshake against it and every
	// Connect hands the SAME token to the sidecar over stdin, so both sides agree across reconnects.
	IpcToken = FUnrealMcpSidecarManager::GenerateToken();

	// ARM the loopback listener now (the bridge accepts on its own thread) — but DO NOT spawn the sidecar
	// and DO NOT push a connection config yet. A game that never calls Connect() spawns zero child processes
	// (§12.4). The effective config is supplied at Connect() time, not here.
	BoundPort = Impl->BridgeServer->Start(IpcToken, ProjectPath, PluginVersion, EngineVersion, /*EffectiveConfig*/ nullptr);
	if (BoundPort <= 0)
	{
		UE_LOG(LogUnrealMcp, Error,
			TEXT("[Unreal-MCP] runtime bridge listener failed to arm; Connect() will be rejected until a port binds."));
	}
	else
	{
		UE_LOG(LogUnrealMcp, Log,
			TEXT("[Unreal-MCP] runtime subsystem initialized (listener armed on port %d, NOT connected — call Connect() to opt in)."),
			BoundPort);
	}

	RegisterConsoleCommands();
}

void UUnrealMcpRuntimeSubsystem::Deinitialize()
{
	UnregisterConsoleCommands();

	if (Impl)
	{
		// §1.5 graceful teardown ordering (mirrors the editor coordinator): stop auto-restarts so the bridge's
		// Bye is not undone by a relaunch, let the bridge send `shutdown`, give the child a bounded grace, then
		// terminate as a backstop — so closing the game leaves no orphaned sidecar (§12.4 DoD).
		if (Impl->SidecarManager.IsValid())
			Impl->SidecarManager->StopRestarts();

		if (Impl->BridgeServer.IsValid())
		{
			Impl->BridgeServer->Shutdown(); // sends the §1.5 `shutdown` Bye to a connected sidecar, then tears down
			Impl->BridgeServer.Reset();
		}

		if (Impl->SidecarManager.IsValid())
		{
			Impl->SidecarManager->WaitForExit(FTimespan::FromSeconds(3)); // bounded grace for a clean self-exit
			Impl->SidecarManager->Stop();                                 // backstop: TerminateProc if still alive
			Impl->SidecarManager.Reset();
		}

		if (Impl->ExtensionManager.IsValid())
		{
			Impl->ExtensionManager->Shutdown();
			Impl->ExtensionManager.Reset();
		}

		Impl->Dispatcher.Reset();
		Impl->ResourceRegistry.Reset(); // after ExtensionManager (holds a raw pointer to it) is torn down above
		Impl->PromptRegistry.Reset();
		Impl->Registry.Reset();
		Impl.Reset();
	}
	BoundPort = -1;

	// §12.6: drop the runtime world resolver so no stale `this`-capturing lambda survives this subsystem.
	FUnrealMcpWorldProvider::ClearWorldResolver();

	Super::Deinitialize();
}

bool UUnrealMcpRuntimeSubsystem::Connect(const FString& Host, const FString& Token,
	EUnrealMcpRuntimeConnectionMode Mode, bool bAllowRemoteHost)
{
	// --- Security gate 4 (§12.8): the kill switch. Default FALSE — a shipped game does not expose its
	// in-game MCP surface unless the developer turned the setting on. ---
	const UUnrealMcpRuntimeSettings* Settings = GetDefault<UUnrealMcpRuntimeSettings>();
	if (!Settings || !Settings->bRuntimeMcpEnabled)
	{
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] runtime Connect rejected: the runtime kill switch (UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled) is off. ")
			TEXT("Enable it in Project Settings -> Plugins -> Unreal MCP (Runtime) to opt in."));
		return false;
	}

	// --- Security gate 3 (§12.8): build-config gating. In a Shipping build with UNREAL_MCP_ALLOW_SHIPPING==0,
	// reject regardless of the kill switch. Guarded so the dead branch is stripped from non-Shipping builds. ---
#if UE_BUILD_SHIPPING && (UNREAL_MCP_ALLOW_SHIPPING == 0)
	// Host/Token/Mode/bAllowRemoteHost are unused on this branch (the §else branch consumes them); silence the
	// unused-parameter warning so a Shipping compile stays clean under -WarningsAsErrors.
	(void)Host; (void)Token; (void)Mode; (void)bAllowRemoteHost;
	UE_LOG(LogUnrealMcp, Warning,
		TEXT("[Unreal-MCP] runtime Connect rejected: this is a Shipping build and bUnrealMcpAllowShipping is off ")
		TEXT("(UNREAL_MCP_ALLOW_SHIPPING=0). Enable the Build.cs flag to permit runtime MCP in Shipping."));
	return false;
#else

	// --- Security gate 5 (§12.8): loopback-host default. Reject a non-loopback host unless explicitly allowed. ---
	if (!bAllowRemoteHost && !IsLoopbackHost(Host))
	{
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] runtime Connect rejected: host '%s' is not loopback and bAllowRemoteHost is false. ")
			TEXT("Pass bAllowRemoteHost=true to connect to a remote host (you accept the remote-control exposure)."),
			*Host);
		return false;
	}

	// The listener must be armed (Initialize bound a port) before a sidecar can dial in.
	if (BoundPort <= 0 || !Impl || !Impl->BridgeServer.IsValid())
	{
		UE_LOG(LogUnrealMcp, Error,
			TEXT("[Unreal-MCP] runtime Connect rejected: the loopback listener is not armed (Initialize failed to bind a port)."));
		return false;
	}

	// Build the effective connection config the bridge pushes to the sidecar (§1.3 `config` / handshake-ack).
	// Start from the resolved §8 config so disk/.env/env defaults apply, then override with the explicit
	// Connect arguments (the in-game caller's intent wins over persisted config).
	FUnrealMcpConfig Config = FUnrealMcpConfig::LoadAndResolve();
	Config.ConnectionMode = (Mode == EUnrealMcpRuntimeConnectionMode::Cloud)
		? EUnrealMcpConnectionMode::Cloud
		: EUnrealMcpConnectionMode::Custom;
	if (Mode == EUnrealMcpRuntimeConnectionMode::Custom)
	{
		Config.CustomHost = Host;
		if (!Token.IsEmpty())
		{
			Config.CustomToken = Token;
			Config.AuthOption = EUnrealMcpAuthOption::Required;
		}
	}
	else
	{
		// Cloud: Host overrides the cloud base URL; Token is the cloud bearer.
		if (!Host.IsEmpty())
			Config.CloudUrl = Host;
		if (!Token.IsEmpty())
			Config.CloudToken = Token;
	}
	Config.bKeepConnected = true; // §12.4: the runtime config push carries keepConnected:true

	const TSharedPtr<FJsonObject> EffectiveConfig = Config.BuildEffectiveConnectionConfig();
	Impl->BridgeServer->SetEffectiveConfig(EffectiveConfig); // also pushes the §1.3 `config` if a sidecar is already up

	// Display values for the connection logs below: the mode label, and a host that falls back to a
	// readable "(default)" when no Host override was passed (Cloud with no override resolves to the
	// default cloud URL, Custom to the §8 default host) instead of logging an empty string.
	const TCHAR* const ModeForLog = Mode == EUnrealMcpRuntimeConnectionMode::Cloud ? TEXT("Cloud") : TEXT("Custom");
	const FString HostForLog = Host.IsEmpty() ? TEXT("(default)") : Host;

	// Spawn the sidecar (§12.4: deferred to Connect so a never-connecting game spawns zero child procs). The
	// sidecar dials the armed loopback port and authenticates with IpcToken over stdin (§1.4).
	if (!Impl->SidecarManager.IsValid())
		Impl->SidecarManager = MakeUnique<FUnrealMcpSidecarManager>();

	if (Impl->SidecarManager->IsRunning())
	{
		// Already connected/connecting: the SetEffectiveConfig above re-pushed the new config to the live
		// sidecar. Treat as success (idempotent reconnect with updated target).
		UE_LOG(LogUnrealMcp, Log,
			TEXT("[Unreal-MCP] runtime Connect: sidecar already running; pushed updated config (mode=%s, host=%s)."),
			ModeForLog, *HostForLog);
		return true;
	}

	const bool bSpawned = Impl->SidecarManager->StartForPort(BoundPort, IpcToken);
	if (!bSpawned)
	{
		UE_LOG(LogUnrealMcp, Error,
			TEXT("[Unreal-MCP] runtime Connect: bridge sidecar binary could not be resolved (packaged-without-<rid> ")
			TEXT("or unset UNREAL_MCP_BRIDGE_PATH); no connection established."));
		return false;
	}

	UE_LOG(LogUnrealMcp, Log,
		TEXT("[Unreal-MCP] runtime Connect: sidecar spawned, dialing loopback port %d (mode=%s, host=%s, token=%s)."),
		BoundPort, ModeForLog, *HostForLog, *FUnrealMcpConfig::MaskSecret(Token));
	return true;
#endif // UE_BUILD_SHIPPING && UNREAL_MCP_ALLOW_SHIPPING==0
}

void UUnrealMcpRuntimeSubsystem::Disconnect()
{
	if (!Impl || !Impl->SidecarManager.IsValid())
		return;

	// Tear DOWN the sidecar but leave the bridge listener ARMED, so a later Connect() can re-spawn the sidecar
	// against the same port/token without a full re-Initialize. (Deinitialize is the one that shuts the bridge.)
	// §1.5 graceful teardown ordering: stop restarts so the watchdog does not relaunch, bounded grace for a
	// clean self-exit, then terminate as a backstop. When the sidecar exits, the bridge's reader observes the
	// dropped connection and bClientConnected falls to false (IsConnected() then reports false).
	Impl->SidecarManager->StopRestarts();
	Impl->SidecarManager->WaitForExit(FTimespan::FromSeconds(3));
	Impl->SidecarManager->Stop();
	Impl->SidecarManager.Reset();

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] runtime Disconnect: sidecar torn down (no orphan); listener still armed."));
}

bool UUnrealMcpRuntimeSubsystem::IsConnected() const
{
	return Impl
		&& Impl->SidecarManager.IsValid() && Impl->SidecarManager->IsRunning()
		&& Impl->BridgeServer.IsValid() && Impl->BridgeServer->IsClientConnected();
}

UUnrealMcpRuntimeSubsystem* UUnrealMcpRuntimeSubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext)
		return nullptr;
	const UWorld* World = WorldContext->GetWorld();
	if (!World)
		return nullptr;
	UGameInstance* GI = World->GetGameInstance();
	return GI ? GI->GetSubsystem<UUnrealMcpRuntimeSubsystem>() : nullptr;
}

void UUnrealMcpRuntimeSubsystem::RegisterToolProvider(IUnrealMcpToolProvider* Provider)
{
	if (!Provider)
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] RegisterToolProvider: null provider ignored."));
		return;
	}

	// Thin wrapper over IModularFeatures (§12.9): the extension manager subscribed to the register event in
	// Initialize(), so this registration triggers the same registry rebuild + manifest re-push that a
	// plugin-load-time registration does. The subsystem does NOT take ownership — the caller keeps the
	// provider alive and is responsible for UnregisterToolProvider before destroying it.
	IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] runtime tool provider '%s' registered."), *Provider->GetExtensionId());
}

void UUnrealMcpRuntimeSubsystem::UnregisterToolProvider(IUnrealMcpToolProvider* Provider)
{
	if (!Provider)
		return; // null is a harmless no-op (mirrors a Disconnect() that was never connected)

	// The manager observes the unregister event, rebuilds, and re-pushes the manifest so a connected
	// sidecar drops the provider's tools. IModularFeatures tolerates unregistering a feature that was never
	// registered, so a double-unregister or an unknown provider is harmless.
	IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] runtime tool provider '%s' unregistered."), *Provider->GetExtensionId());
}

void UUnrealMcpRuntimeSubsystem::RegisterPromptProvider(IUnrealMcpPromptProvider* Provider)
{
	if (!Provider)
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] RegisterPromptProvider: null provider ignored."));
		return;
	}

	// Thin wrapper over IModularFeatures (§A.2): the extension manager subscribed to the prompt register event
	// in Initialize(), so this triggers the same prompt-registry rebuild + manifest re-push a plugin-load-time
	// registration does. Not owned — the caller keeps the provider alive and must UnregisterPromptProvider.
	IModularFeatures::Get().RegisterModularFeature(IUnrealMcpPromptProvider::GetModularFeatureName(), Provider);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] runtime prompt provider '%s' registered."), *Provider->GetExtensionId());
}

void UUnrealMcpRuntimeSubsystem::UnregisterPromptProvider(IUnrealMcpPromptProvider* Provider)
{
	if (!Provider)
		return; // null is a harmless no-op

	IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpPromptProvider::GetModularFeatureName(), Provider);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] runtime prompt provider '%s' unregistered."), *Provider->GetExtensionId());
}

void UUnrealMcpRuntimeSubsystem::RegisterResourceProvider(IUnrealMcpResourceProvider* Provider)
{
	if (!Provider)
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] RegisterResourceProvider: null provider ignored."));
		return;
	}

	// Thin wrapper over IModularFeatures (§A.2): the extension manager subscribed to the resource register event
	// in Initialize(), so this triggers the same resource-registry rebuild + manifest re-push a plugin-load-time
	// registration does. Not owned — the caller keeps the provider alive and must UnregisterResourceProvider.
	IModularFeatures::Get().RegisterModularFeature(IUnrealMcpResourceProvider::GetModularFeatureName(), Provider);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] runtime resource provider '%s' registered."), *Provider->GetExtensionId());
}

void UUnrealMcpRuntimeSubsystem::UnregisterResourceProvider(IUnrealMcpResourceProvider* Provider)
{
	if (!Provider)
		return; // null is a harmless no-op

	IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpResourceProvider::GetModularFeatureName(), Provider);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] runtime resource provider '%s' unregistered."), *Provider->GetExtensionId());
}

bool UUnrealMcpRuntimeSubsystem::IsLoopbackHost(const FString& Host)
{
	if (Host.IsEmpty())
		return true; // empty host resolves to the default loopback server (§8 DefaultCustomHost is localhost)

	// Extract the hostname from a possibly-schemed URL (e.g. "http://localhost:8080" -> "localhost").
	FString Work = Host;
	int32 SchemeIdx;
	if (Work.FindChar(TEXT(':'), SchemeIdx) && Work.Mid(SchemeIdx).StartsWith(TEXT("://")))
		Work = Work.RightChop(SchemeIdx + 3);

	// Strip any path / query.
	int32 SlashIdx;
	if (Work.FindChar(TEXT('/'), SlashIdx))
		Work = Work.Left(SlashIdx);

	// Strip a port. IPv6 literals are bracketed ("[::1]:8080"); handle that before the generic colon split.
	if (Work.StartsWith(TEXT("[")))
	{
		int32 CloseIdx;
		if (Work.FindChar(TEXT(']'), CloseIdx))
			Work = Work.Mid(1, CloseIdx - 1);
	}
	else
	{
		int32 ColonIdx;
		if (Work.FindChar(TEXT(':'), ColonIdx))
			Work = Work.Left(ColonIdx);
	}

	Work = Work.TrimStartAndEnd().ToLower();
	return Work == TEXT("localhost")
		|| Work == TEXT("127.0.0.1")
		|| Work.StartsWith(TEXT("127.")) // the whole 127.0.0.0/8 loopback block
		|| Work == TEXT("::1")
		|| Work == TEXT("0:0:0:0:0:0:0:1");
}

void UUnrealMcpRuntimeSubsystem::RegisterConsoleCommands()
{
	if (ConnectCommand || DisconnectCommand)
		return; // idempotent

	// `UnrealMcp.Connect <host> [token]` — QA opt-in without a recompile (§12.4). Loopback-only by default
	// (the console path never passes bAllowRemoteHost; a remote host is rejected exactly like the API).
	ConnectCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("UnrealMcp.Connect"),
		TEXT("Connect the runtime MCP bridge: UnrealMcp.Connect <host> [token]. Loopback hosts only."),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UUnrealMcpRuntimeSubsystem::HandleConnectCommand),
		ECVF_Default);

	DisconnectCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("UnrealMcp.Disconnect"),
		TEXT("Disconnect the runtime MCP bridge and tear down the sidecar."),
		FConsoleCommandDelegate::CreateUObject(this, &UUnrealMcpRuntimeSubsystem::Disconnect),
		ECVF_Default);
}

void UUnrealMcpRuntimeSubsystem::UnregisterConsoleCommands()
{
	if (ConnectCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ConnectCommand);
		ConnectCommand = nullptr;
	}
	if (DisconnectCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(DisconnectCommand);
		DisconnectCommand = nullptr;
	}
}

void UUnrealMcpRuntimeSubsystem::HandleConnectCommand(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] usage: UnrealMcp.Connect <host> [token]"));
		return;
	}

	const FString Host = Args[0];
	const FString Token = Args.Num() >= 2 ? Args[1] : FString();
	// The console path never passes bAllowRemoteHost — a remote host is rejected exactly like the API (§12.8 #5).
	Connect(Host, Token, EUnrealMcpRuntimeConnectionMode::Custom, /*bAllowRemoteHost*/ false);
}
