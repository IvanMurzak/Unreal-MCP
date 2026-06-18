// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpEditorCoordinator.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpRuntimeCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Config/UnrealMcpConfig.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"
#include "Bridge/UnrealMcpBridgeServer.h"
#include "Sidecar/UnrealMcpSidecarManager.h"
#include "Server/UnrealMcpServerManager.h"
#include "Extensions/UnrealMcpExtensionManager.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UI/UnrealMcpMainWindowTab.h"
#include "UI/UnrealMcpAuxWindows.h"
#include "UI/SUnrealMcpToolsWindow.h"
#include "UI/SUnrealMcpFeatureListWindow.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "Tools/UnrealMcpWorldProvider.h"
#include "DevControl/UnrealMcpDevControlServer.h"

#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"

// Defined here (where every subsystem type is complete) so the TUniquePtr member deleters instantiate
// correctly regardless of unity-build grouping. See the header comment.
FUnrealMcpEditorCoordinator::FUnrealMcpEditorCoordinator() = default;
FUnrealMcpEditorCoordinator::~FUnrealMcpEditorCoordinator() = default;

void FUnrealMcpEditorCoordinator::Startup()
{
	if (bStarted)
		return;
	bStarted = true;

	// §12.6: install the EDITOR world resolver into the runtime module BEFORE any tool body can run. The
	// runtime module's FUnrealMcpObjectRef::GetEditorWorld() now delegates to FUnrealMcpWorldProvider; this
	// resolver preserves today's behaviour byte-for-byte (`GEditor ? GEditor->GetEditorWorldContext().World()
	// : nullptr`). Cleared in Shutdown(). The runtime bootstrap subsystem (R3) installs a game-world resolver
	// instead; in the editor this coordinator owns the resolver.
	FUnrealMcpWorldProvider::SetWorldResolver([]() -> UWorld*
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	});

	// §10 editor/reflection family: start the GLog ring-buffer collector BEFORE anything else so the
	// earliest startup log lines are already captured for console-get-logs. Shutdown() deregisters it.
	FUnrealMcpLogCollector::Get().Startup();

	Registry = MakeUnique<FUnrealMcpToolRegistry>();
	UnrealMcpPingTool::Register(*Registry);
	UnrealMcpActorTools::Register(*Registry);
	UnrealMcpBlueprintTools::Register(*Registry); // §10 flagship Blueprint family (CORE)
	UnrealMcpAssetTools::Register(*Registry); // §10 asset / Content-Browser family (issue #10)
	UnrealMcpEditorTools::Register(*Registry); // §10 editor / console / reflection family (issue #19)
	UnrealMcpLevelTools::Register(*Registry); // §10 level / map family (issue #16, Unity Scene.* analog)
	UnrealMcpScreenshotTools::Register(*Registry); // §10 screenshot / viewport-capture family (issue #17)
	UnrealMcpSourceTools::Register(*Registry); // §10 C++ source / script family (issue #18)

	Dispatcher = MakeUnique<FUnrealMcpGameThreadDispatcher>();
	BridgeServer = MakeUnique<FUnrealMcpBridgeServer>(*Registry, *Dispatcher);

	// Discover 3rd-party extension tool providers (§5) and merge them into the registry BEFORE the bridge
	// starts accepting, so the first manifest a sidecar reads on handshake already includes them. Late
	// register/unregister events rebuild the registry and re-push the manifest via the OnChanged callback.
	ExtensionManager = MakeUnique<FUnrealMcpExtensionManager>(
		*Registry,
		[this]() { if (BridgeServer.IsValid()) BridgeServer->PushManifest(); });
	ExtensionManager->Startup();

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

	// §7/§8 enable-map: apply the §8 `enabledTools` env whitelist and the persisted §7 `disabledTools` blocklist to
	// the registry BEFORE the bridge starts accepting, so the FIRST manifest a sidecar reads on handshake already
	// reflects them (excluded ProxyTools are never created sidecar-side, so they never appear in tools/list). The
	// registry RETAINS both filters, so a later §5 extension hot-reload re-applies them to the rebuilt tools.
	Registry->SetEnabledToolsFilter(Config.EnabledTools);
	Registry->ApplyDisabledTools(Config.DisabledTools);
	if (Config.DisabledTools.Num() > 0)
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] applied %d disabled tool(s) from the §8 enable-map (%d/%d tools enabled)."),
			Config.DisabledTools.Num(), Registry->NumEnabled(), Registry->Num());
	}

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

	// §7 in-UI local-server (issue #95): create the local gamedev-mcp-server manager. It does NOT auto-start —
	// the user launches it from the MCP-server card's Start button (Custom+http only). Reattach to a server this
	// plugin spawned in a previous module load (hot-reload survivor) so a later Stop / editor-quit tears it down
	// instead of orphaning it — only meaningful in Custom+http.
	ServerManager = MakeUnique<FUnrealMcpServerManager>();
	if (FUnrealMcpServerManager::IsLaunchAllowed(
			Config.ConnectionMode == EUnrealMcpConnectionMode::Custom,
			Config.ResolveEffectiveTransport() == EUnrealMcpTransportMethod::Http))
	{
		const int32 ReattachPort = FUnrealMcpServerManager::ParsePortFromHost(Config.ResolveCustomHost());
		ServerManager->ReattachIfRunning(
			ReattachPort, /*PluginTimeoutMs*/ 10000,
			Config.AuthOption == EUnrealMcpAuthOption::Required, Config.ResolveEffectiveToken());
	}

	// §7 UI: the main-window view-model owns all UI state; wire its side-effect sinks to the real subsystems.
	// All config writes go config-store-first (Save) then push the §1.3 `config` to the sidecar (§7 ownership).
	ViewModel = MakeShared<FUnrealMcpEditorViewModel>();
	ViewModel->InitializeConfig(Config);

	FUnrealMcpBridgeServer* BridgeServerPtr = BridgeServer.Get();
	ViewModel->OnPersistConfig = [](const FUnrealMcpConfig& Cfg)
	{
		Cfg.Save(FUnrealMcpConfig::DefaultConfigFilePath());
	};
	ViewModel->OnPushConfig = [BridgeServerPtr](const FUnrealMcpConfig& Cfg)
	{
		if (BridgeServerPtr)
			BridgeServerPtr->SetEffectiveConfig(Cfg.BuildEffectiveConnectionConfig());
	};
	ViewModel->OnSendAuth = [BridgeServerPtr](const FString& AuthType) -> bool
	{
		// Plumb the send result back so Authorize() does not enter a code-less Pending state when there is no
		// connected sidecar to receive the auth-start frame.
		return BridgeServerPtr ? BridgeServerPtr->SendAuthMessage(AuthType) : false;
	};
	// Issue #99: "ensure the sidecar is started" hook. When Cloud Authorize is pressed and no sidecar is connected
	// yet, the view-model calls this to (re)start the bridge, then QUEUES the auth-start to flush on handshake
	// (OnSendAuth above eventually succeeds once HandshakeSink fires). Already-running → true (a handshake is
	// imminent / already happening). Not running → StartForPort spawns it and returns false ONLY when the binary
	// genuinely cannot be resolved (packaged-without-<rid> / unset UNREAL_MCP_BRIDGE_PATH) — the view-model then
	// fails fast with an actionable message instead of awaiting a handshake that will never come.
	FUnrealMcpSidecarManager* SidecarPtrForAuth = SidecarManager.Get();
	ViewModel->OnEnsureSidecarStarted = [SidecarPtrForAuth, BoundPort, Token]() -> bool
	{
		if (!SidecarPtrForAuth)
			return false;
		if (SidecarPtrForAuth->IsRunning())
			return true;
		return SidecarPtrForAuth->StartForPort(BoundPort, Token);
	};
	ViewModel->OnOpenBrowser = [](const FString& Url)
	{
		if (!Url.IsEmpty())
			FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	};

	// §7 in-UI local-server (issue #95): wire the MCP-server card's Start/Stop to the local server manager. The
	// view-model already gates these to Custom+http (ToggleLocalServer no-ops otherwise) — the sinks just execute.
	// Re-resolve the §8 config on each Start so the port (from the Custom host), auth, and token reflect what the
	// user has set right now. The port the agent dials (Custom host) MUST equal the port the server listens on.
	FUnrealMcpServerManager* ServerPtr = ServerManager.Get();
	ViewModel->OnStartLocalServer = [ServerPtr]() -> bool
	{
		if (!ServerPtr)
			return false;
		const FUnrealMcpConfig Live = FUnrealMcpConfig::LoadAndResolve();
		const int32 ServerPort = FUnrealMcpServerManager::ParsePortFromHost(Live.ResolveCustomHost());
		return ServerPtr->Start(
			ServerPort, /*PluginTimeoutMs*/ 10000,
			Live.AuthOption == EUnrealMcpAuthOption::Required, Live.ResolveEffectiveToken());
	};
	ViewModel->OnStopLocalServer = [ServerPtr]()
	{
		if (ServerPtr)
			ServerPtr->Stop(/*bForce*/ false);
	};
	ViewModel->IsLocalServerRunningSink = [ServerPtr]() -> bool
	{
		return ServerPtr && ServerPtr->IsRunning();
	};
	// §7 per-tool enable-map: a Tools-window toggle re-applies the disabled set to the registry and re-pushes the
	// manifest so the sidecar's tools/list drops/restores the toggled tool over the wire (§2.2). The view-model
	// already persisted the choice to the §8 store before invoking this; here we only mutate the live registry +
	// push. Runs on the game thread (the UI calls it there), matching the §2.2 dynamic-re-registration contract.
	FUnrealMcpToolRegistry* RegistryPtr = Registry.Get();
	ViewModel->OnToolEnablementChanged = [RegistryPtr, BridgeServerPtr](const TArray<FString>& DisabledTools)
	{
		if (RegistryPtr)
			RegistryPtr->ApplyDisabledTools(DisabledTools);
		if (BridgeServerPtr)
			BridgeServerPtr->PushManifest();
	};

	// §7 live status feed: the bridge delivers `status` / `device-auth` on the IPC READER thread. Marshal onto
	// the game thread before touching any view-model/Slate state (the Godot M9b main-thread-marshalled rule),
	// and hold the view-model weakly so a late status straddling teardown is a no-op, not a use-after-free.
	TWeakPtr<FUnrealMcpEditorViewModel> WeakViewModel = ViewModel;
	// The agent-config-result feed (§7, issue #101) routes to the main-window tab's panel. The tab is owned by
	// `this` (the runtime) and torn down in Shutdown on the game thread BEFORE the status sink is cleared; the
	// sink marshals onto the game thread, so a result that straddles teardown runs after Shutdown and reads a
	// reset MainWindowTab — guard with the same weak view-model check (its reset coincides with teardown) and a
	// null MainWindowTab check so it is a no-op, never a use-after-free.
	FUnrealMcpEditorCoordinator* CoordinatorPtr = this;
	if (BridgeServerPtr)
	{
		BridgeServerPtr->SetStatusSink([WeakViewModel, CoordinatorPtr](const FString& Type, TSharedPtr<FJsonObject> Message)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakViewModel, CoordinatorPtr, Type, Message]()
			{
				TSharedPtr<FUnrealMcpEditorViewModel> VM = WeakViewModel.Pin();
				if (!VM.IsValid())
					return; // teardown straddled — also the guard for CoordinatorPtr->MainWindowTab below (reset in Shutdown)
				if (Type == TEXT("status"))
					VM->ApplyStatus(Message);
				else if (Type == TEXT("device-auth"))
					VM->ApplyDeviceAuth(Message);
				else if (Type == TEXT("agent-config-result") && CoordinatorPtr->MainWindowTab.IsValid())
					CoordinatorPtr->MainWindowTab->DeliverAgentConfigResult(Message);
			});
		});

		// Issue #99: on each sidecar handshake-complete, marshal to the game thread and flush a queued Cloud
		// auth-start (a no-op unless Authorize is awaiting in the Connecting state). Same M9b marshalling + weak
		// view-model guard as the status sink so a handshake straddling teardown is a no-op, not a use-after-free.
		BridgeServerPtr->SetHandshakeSink([WeakViewModel]()
		{
			AsyncTask(ENamedThreads::GameThread, [WeakViewModel]()
			{
				if (TSharedPtr<FUnrealMcpEditorViewModel> VM = WeakViewModel.Pin())
					VM->NotifySidecarHandshakeComplete();
			});
		});
	}

	// Register the nomad dockable tab + Window-menu entry (§7). A manual "Restart bridge" force-relaunches the
	// sidecar; the bridge-status line reflects the sidecar's run state.
	FUnrealMcpSidecarManager* SidecarPtr = SidecarManager.Get();
	MainWindowTab = MakeUnique<FUnrealMcpMainWindowTab>();
	MainWindowTab->Register(
		ViewModel.ToSharedRef(),
		PluginVersion,
		FSimpleDelegate::CreateLambda([SidecarPtr, BoundPort, Token]()
		{
			if (SidecarPtr)
			{
				SidecarPtr->Stop();
				SidecarPtr->StartForPort(BoundPort, Token);
			}
		}),
		[SidecarPtr]() -> FString
		{
			if (SidecarPtr && SidecarPtr->IsRunning())
				return FString::Printf(TEXT("Running (restarts: %d)"), SidecarPtr->GetRestartCount());
			return TEXT("Stopped");
		},
		// §7/§8 AI Agent Configurators: yield the live connection facts so the assembled STDIO/HTTP snippets
		// reflect the current config. Re-resolve the §8 config on each call (the user may have just edited the
		// host/token in the same window) and read the bound IPC port from the bridge. The local server binary
		// path is owned by the cli/§6 install layout — not the plugin — so it is left empty here; the STDIO
		// snippet is still a valid template (the user supplies the binary, exactly the cli's stance), and the
		// HTTP form is fully self-contained.
		[BridgeServerPtr]() -> FAiAgentConnectionInfo
		{
			const FUnrealMcpConfig Live = FUnrealMcpConfig::LoadAndResolve();
			const int32 Port = BridgeServerPtr ? BridgeServerPtr->GetBoundPort() : 0;
			return FAiAgentConnectionInfo::FromPluginConfig(Live, /*ServerPath*/ FString(), Port);
		},
		// §7 (issue #101) AI Agent Configurators: send a configurator request to the sidecar over IPC. Returns
		// false when no sidecar is connected — the thin panel then shows its graceful "bridge not connected"
		// state and re-requests on the next handshake. The result returns via the status sink's
		// `agent-config-result` route above.
		[BridgeServerPtr](const TSharedPtr<FJsonObject>& Request) -> bool
		{
			return BridgeServerPtr ? BridgeServerPtr->SendAgentConfigMessage(Request) : false;
		});

	// §7 auxiliary windows (MCP Tools / Prompts / Resources). The Tools window snapshots the registry on open for
	// its list — a §5 extension hot-reload can change the set after boot, so an already-open window shows its
	// open-time snapshot and reopen refreshes it. Prompts/Resources have no plugin-side feed yet (the .NET sidecar
	// owns those features, §2) — their providers return empty and the windows render an honest empty state rather
	// than a fabricated registry. Connection settings (incl. the read-only IPC-bridge-port line) live in the
	// single "AI Game Developer" main window, not an aux window (issue #107, Unity-MCP parity).
	AuxWindows = MakeUnique<FUnrealMcpAuxWindows>();
	AuxWindows->Register(
		ViewModel.ToSharedRef(),
		[RegistryPtr]() -> TArray<FUnrealMcpToolListEntry>
		{
			TArray<FUnrealMcpToolListEntry> Entries;
			if (RegistryPtr)
			{
				for (const FString& Name : RegistryPtr->GetToolNamesSorted())
				{
					if (const FUnrealMcpRegisteredTool* Tool = RegistryPtr->Find(Name))
						Entries.Add(FUnrealMcpToolListEntry{ Tool->Name, Tool->Title, Tool->Description, Tool->ExtensionId,
							RegistryPtr->PassesEnabledToolsWhitelist(Name) });
				}
			}
			return Entries;
		},
		[]() -> TArray<FUnrealMcpFeatureEntry> { return {}; },  // prompts (none surfaced to the plugin yet)
		[]() -> TArray<FUnrealMcpFeatureEntry> { return {}; }); // resources (none surfaced to the plugin yet)

	// DEV-ONLY inject/control HTTP bridge (docs/ARCHITECTURE.md §7). Started ONLY when the editor process env
	// UNREAL_MCP_DEV_CONTROL == "1" — OFF by default, so a shipped plugin never opens a port. The port comes
	// from UNREAL_MCP_DEV_CONTROL_PORT (default 9921). Handlers run on the game thread and drive the SAME live
	// view-model the dock binds to, so an injected state / simulated click is reflected in the open window.
	// Loopback-only (the server verifies the peer is 127.0.0.1/::1 in addition to the env gate).
	{
		// Resolve with precedence process env > project-root .env > default — so a developer can enable the
		// bridge by dropping the flag into the project's .env (the editor is launched from the GUI with no
		// shell exports) WITHOUT exporting a process env var. Mirrors the §8 connection-config env-file layer
		// (and Godot's GodotMcpPlugin.StartDevControlIfEnabled). UNREAL_MCP_DEV_CONTROL[_PORT] are NOT §8
		// recognized keys, so they are read straight from the .env via LookupEnvFileValue, never the DotEnv map.
		const FString DevEnvPath = FUnrealMcpConfig::DefaultEnvFilePath();
		auto ResolveDevVar = [&DevEnvPath](const TCHAR* Key) -> FString
		{
			const FString FromProcess = FPlatformMisc::GetEnvironmentVariable(Key);
			return !FromProcess.IsEmpty() ? FromProcess : FUnrealMcpConfig::LookupEnvFileValue(DevEnvPath, Key);
		};

		const bool bDevControlEnabled = ResolveDevVar(TEXT("UNREAL_MCP_DEV_CONTROL")) == TEXT("1");
		if (bDevControlEnabled)
		{
			uint32 DevControlPort = 9921;
			const FString PortRaw = ResolveDevVar(TEXT("UNREAL_MCP_DEV_CONTROL_PORT"));
			if (!PortRaw.IsEmpty())
			{
				const int32 Parsed = FCString::Atoi(*PortRaw);
				if (Parsed > 0 && Parsed <= 65535)
					DevControlPort = static_cast<uint32>(Parsed);
				else
					UE_LOG(LogUnrealMcp, Warning, TEXT("[dev-control] ignoring invalid UNREAL_MCP_DEV_CONTROL_PORT '%s'; using %u."), *PortRaw, DevControlPort);
			}

			DevControlServer = MakeUnique<FUnrealMcpDevControlServer>();
			if (!DevControlServer->Start(DevControlPort, ViewModel))
			{
				UE_LOG(LogUnrealMcp, Warning, TEXT("[dev-control] failed to start on port %u; inject/control bridge disabled."), DevControlPort);
				DevControlServer.Reset();
			}
		}
	}

	// §1.5: tear down on editor pre-exit so the sidecar never orphans (layer 1).
	PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([this]() { Shutdown(); });
}

void FUnrealMcpEditorCoordinator::Shutdown()
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

	// DEV-ONLY inject/control bridge: stop it FIRST so no in-flight HTTP handler can touch the view-model
	// after the teardown below frees it (the server holds the view-model weakly, but stopping unbinds the
	// routes so the port closes cleanly). No-op when it was never started (env gate off).
	if (DevControlServer.IsValid())
	{
		DevControlServer->Stop();
		DevControlServer.Reset();
	}

	// §7 UI teardown FIRST: unregister the tab and drop the bridge's status sink so no marshalled status can
	// touch the view-model after this, then release the view-model. Clear the sink before the bridge tears
	// down so a reader-thread invoke cannot race the reset.
	if (AuxWindows.IsValid())
	{
		AuxWindows->Unregister(); // neutralizes the widget-held providers + closes live aux tabs, all before the bridge/registry below are freed
		AuxWindows.Reset();
	}
	if (MainWindowTab.IsValid())
	{
		MainWindowTab->Unregister(); // also closes a live tab so its widget (a strong view-model ref) is freed
		MainWindowTab.Reset();
	}
	if (BridgeServer.IsValid())
	{
		BridgeServer->SetStatusSink(nullptr);
		BridgeServer->SetHandshakeSink(nullptr); // issue #99: drop the handshake-flush sink before the VM is freed
	}
	// Null the view-model's side-effect sinks before destroying the bridge/sidecar below: those sinks capture
	// raw FUnrealMcpBridgeServer* pointers, so if anything still holds the view-model after the tab close
	// (a deferred RequestCloseTab, a queued widget event) it cannot dereference the soon-to-be-freed bridge.
	if (ViewModel.IsValid())
	{
		ViewModel->OnPersistConfig = nullptr;
		ViewModel->OnPushConfig = nullptr;
		ViewModel->OnSendAuth = nullptr;
		ViewModel->OnEnsureSidecarStarted = nullptr; // issue #99: captures the raw sidecar-manager pointer freed below
		ViewModel->OnOpenBrowser = nullptr;
		ViewModel->OnToolEnablementChanged = nullptr; // captures the raw registry/bridge pointers freed below
		// §7 in-UI local-server sinks capture the raw FUnrealMcpServerManager* freed below — null before that.
		ViewModel->OnStartLocalServer = nullptr;
		ViewModel->OnStopLocalServer = nullptr;
		ViewModel->IsLocalServerRunningSink = nullptr;
	}
	ViewModel.Reset();

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

	// §7 in-UI local-server (issue #95): force-stop the local gamedev-mcp-server on editor shutdown so NO
	// orphaned server process survives editor close (the DoD "Start then close → server dies" requirement).
	// bForce blocks (bounded) until the child has actually exited. No-op when one was never started.
	if (ServerManager.IsValid())
	{
		ServerManager->Stop(/*bForce*/ true);
		ServerManager.Reset();
	}

	// Unsubscribe from modular-feature events before the registry it mutates is torn down.
	if (ExtensionManager.IsValid())
	{
		ExtensionManager->Shutdown();
		ExtensionManager.Reset();
	}

	Dispatcher.Reset();
	Registry.Reset();

	// §12.6: drop the editor world resolver so no stale GEditor-capturing lambda survives teardown. The
	// captured lambda is GEditor-free of captured state (it reads the global), but clearing keeps the
	// provider's lifecycle symmetric with Startup. Idempotent.
	FUnrealMcpWorldProvider::ClearWorldResolver();

	// §10 editor/reflection family: deregister the GLog listener LAST so no dangling FOutputDevice remains
	// on GLog — an orphaned device crashes the editor on exit. Idempotent (safe if Startup never ran).
	FUnrealMcpLogCollector::Get().Shutdown();
}
