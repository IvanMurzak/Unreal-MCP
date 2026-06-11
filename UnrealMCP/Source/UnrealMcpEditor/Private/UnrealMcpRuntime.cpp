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
#include "Extensions/UnrealMcpExtensionManager.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UI/UnrealMcpMainWindowTab.h"
#include "UI/UnrealMcpAuxWindows.h"
#include "UI/SUnrealMcpToolsWindow.h"
#include "UI/SUnrealMcpFeatureListWindow.h"
#include "Tools/UnrealMcpLogCollector.h"

#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"

// Defined here (where every subsystem type is complete) so the TUniquePtr member deleters instantiate
// correctly regardless of unity-build grouping. See the header comment.
FUnrealMcpRuntime::FUnrealMcpRuntime() = default;
FUnrealMcpRuntime::~FUnrealMcpRuntime() = default;

void FUnrealMcpRuntime::Startup()
{
	if (bStarted)
		return;
	bStarted = true;

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
	ViewModel->OnOpenBrowser = [](const FString& Url)
	{
		if (!Url.IsEmpty())
			FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
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
	if (BridgeServerPtr)
	{
		BridgeServerPtr->SetStatusSink([WeakViewModel](const FString& Type, TSharedPtr<FJsonObject> Message)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakViewModel, Type, Message]()
			{
				TSharedPtr<FUnrealMcpEditorViewModel> VM = WeakViewModel.Pin();
				if (!VM.IsValid())
					return;
				if (Type == TEXT("status"))
					VM->ApplyStatus(Message);
				else if (Type == TEXT("device-auth"))
					VM->ApplyDeviceAuth(Message);
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
		});

	// §7 auxiliary windows (MCP Tools / Prompts / Resources / Settings). The Tools window snapshots the registry on
	// open for its list — a §5 extension hot-reload can change the set after boot, so an already-open window shows its
	// open-time snapshot and reopen refreshes it; the Settings window surfaces the bound IPC port read-only. Prompts/
	// Resources have no plugin-side feed yet (the .NET sidecar owns those features, §2) — their providers return
	// empty and the windows render an honest empty state rather than a fabricated registry.
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
		[BridgeServerPtr]() -> FString
		{
			const int32 Port = BridgeServerPtr ? BridgeServerPtr->GetBoundPort() : -1;
			return Port > 0 ? FString::Printf(TEXT("IPC bridge port: %d"), Port) : FString();
		},
		[]() -> TArray<FUnrealMcpFeatureEntry> { return {}; },  // prompts (none surfaced to the plugin yet)
		[]() -> TArray<FUnrealMcpFeatureEntry> { return {}; }); // resources (none surfaced to the plugin yet)

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

	// §7 UI teardown FIRST: unregister the tab and drop the bridge's status sink so no marshalled status can
	// touch the view-model after this, then release the view-model. Clear the sink before the bridge tears
	// down so a reader-thread invoke cannot race the reset.
	if (AuxWindows.IsValid())
	{
		AuxWindows->Unregister(); // neutralizes the widget-held providers + closes live aux tabs + unregisters the ISettingsModule section, all before the bridge/registry below are freed
		AuxWindows.Reset();
	}
	if (MainWindowTab.IsValid())
	{
		MainWindowTab->Unregister(); // also closes a live tab so its widget (a strong view-model ref) is freed
		MainWindowTab.Reset();
	}
	if (BridgeServer.IsValid())
		BridgeServer->SetStatusSink(nullptr);
	// Null the view-model's side-effect sinks before destroying the bridge/sidecar below: those sinks capture
	// raw FUnrealMcpBridgeServer* pointers, so if anything still holds the view-model after the tab close
	// (a deferred RequestCloseTab, a queued widget event) it cannot dereference the soon-to-be-freed bridge.
	if (ViewModel.IsValid())
	{
		ViewModel->OnPersistConfig = nullptr;
		ViewModel->OnPushConfig = nullptr;
		ViewModel->OnSendAuth = nullptr;
		ViewModel->OnOpenBrowser = nullptr;
		ViewModel->OnToolEnablementChanged = nullptr; // captures the raw registry/bridge pointers freed below
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

	// Unsubscribe from modular-feature events before the registry it mutates is torn down.
	if (ExtensionManager.IsValid())
	{
		ExtensionManager->Shutdown();
		ExtensionManager.Reset();
	}

	Dispatcher.Reset();
	Registry.Reset();

	// §10 editor/reflection family: deregister the GLog listener LAST so no dangling FOutputDevice remains
	// on GLog — an orphaned device crashes the editor on exit. Idempotent (safe if Startup never ran).
	FUnrealMcpLogCollector::Get().Shutdown();
}
