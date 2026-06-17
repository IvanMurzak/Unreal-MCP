// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/UnrealMcpMainWindowTab.h"
#include "UI/SUnrealMcpMainWindow.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UnrealMcpLog.h"

#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "Textures/SlateIcon.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

const FName FUnrealMcpMainWindowTab::TabId(TEXT("UnrealMcpMainWindow"));

void FUnrealMcpMainWindowTab::Register(
	const TSharedRef<FUnrealMcpEditorViewModel>& InViewModel,
	const FString& InPluginVersion,
	FSimpleDelegate InOnRestartBridge,
	TFunction<FString()> InBridgeStatusProvider,
	TFunction<FAiAgentConnectionInfo()> InConnectionInfoProvider,
	TFunction<bool(const TSharedPtr<FJsonObject>&)> InSendAgentConfigRequest)
{
	if (bRegistered)
		return;

	ViewModel = InViewModel;
	PluginVersion = InPluginVersion;
	OnRestartBridge = InOnRestartBridge;
	BridgeStatusProvider = MoveTemp(InBridgeStatusProvider);
	ConnectionInfoProvider = MoveTemp(InConnectionInfoProvider);
	SendAgentConfigRequest = MoveTemp(InSendAgentConfigRequest);

	// Slate may be unavailable in a commandlet / -nullrhi headless run without a slate application; guard so
	// the Automation/smoke runs (which load the module) never crash on tab registration.
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] Slate not initialized; skipping main-window tab registration (headless)."));
		return;
	}

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		TabId,
		FOnSpawnTab::CreateRaw(this, &FUnrealMcpMainWindowTab::SpawnTab))
		.SetDisplayName(LOCTEXT("MainWindowTabTitle", "AI Game Developer"))
		.SetTooltipText(LOCTEXT("MainWindowTabTooltip", "Open the AI Game Developer (Unreal-MCP) window."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Server"));

	bRegistered = true;
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] registered the AI Game Developer main-window tab."));
}

void FUnrealMcpMainWindowTab::Unregister()
{
	if (!bRegistered)
		return;
	if (FSlateApplication::IsInitialized())
	{
		// Unregistering the spawner alone does NOT close an already-open tab: a live SDockTab keeps the hosted
		// SUnrealMcpMainWindow (and its strong ref to the view-model) alive, and that widget captures raw
		// bridge/sidecar pointers via OnRestartBridge / BridgeStatusProvider. On plugin disable / hot-reload
		// (ShutdownModule while the editor and tab live on) those subsystems are destroyed, so a surviving tab
		// would dereference freed memory on the next click. Close the live tab first so the widget is torn down.
		if (TSharedPtr<SDockTab> LiveTab = FGlobalTabmanager::Get()->FindExistingLiveTab(TabId))
			LiveTab->RequestCloseTab();
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
	}
	bRegistered = false;
}

TSharedRef<SDockTab> FUnrealMcpMainWindowTab::SpawnTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	TSharedRef<SUnrealMcpMainWindow> Window =
		SNew(SUnrealMcpMainWindow)
		.ViewModel(ViewModel)
		.PluginVersion(PluginVersion)
		.OnRestartBridge(OnRestartBridge)
		.BridgeStatusProvider(BridgeStatusProvider)
		.ConnectionInfoProvider(ConnectionInfoProvider)
		.SendAgentConfigRequest(SendAgentConfigRequest);
	// Track the live window (weak) so the runtime's agent-config-result feed reaches its panel; a re-spawn
	// (re-opened tab) replaces it, and a closed tab leaves a stale weak ptr that DeliverAgentConfigResult ignores.
	ActiveWindow = Window;
	Tab->SetContent(Window);
	return Tab;
}

void FUnrealMcpMainWindowTab::DeliverAgentConfigResult(const TSharedPtr<FJsonObject>& Result)
{
	if (TSharedPtr<SUnrealMcpMainWindow> Window = ActiveWindow.Pin())
		Window->DeliverAgentConfigResult(Result);
}

#undef LOCTEXT_NAMESPACE
