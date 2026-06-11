// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/UnrealMcpAuxWindows.h"
#include "UI/SUnrealMcpSettingsWindow.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UnrealMcpLog.h"

#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

const FName FUnrealMcpAuxWindows::ToolsTabId(TEXT("UnrealMcpToolsWindow"));
const FName FUnrealMcpAuxWindows::PromptsTabId(TEXT("UnrealMcpPromptsWindow"));
const FName FUnrealMcpAuxWindows::ResourcesTabId(TEXT("UnrealMcpResourcesWindow"));
const FName FUnrealMcpAuxWindows::SettingsTabId(TEXT("UnrealMcpSettingsWindow"));

namespace
{
	const FName SettingsContainer(TEXT("Project"));
	const FName SettingsCategory(TEXT("Plugins"));
	const FName SettingsSection(TEXT("AI Game Developer"));
}

void FUnrealMcpAuxWindows::Register(
	const TSharedRef<FUnrealMcpEditorViewModel>& InViewModel,
	TFunction<TArray<FUnrealMcpToolListEntry>()> InToolListProvider,
	TFunction<FString()> InPortStatusProvider,
	TFunction<TArray<FUnrealMcpFeatureEntry>()> InPromptProvider,
	TFunction<TArray<FUnrealMcpFeatureEntry>()> InResourceProvider)
{
	if (bRegistered)
		return;

	ViewModel = InViewModel;

	// The Tools/Settings providers close over the runtime-owned registry/bridge (raw, non-owning). A widget can
	// outlive Unregister() — a deferred RequestCloseTab still queued when the runtime frees those subsystems — and
	// paint once more, dereferencing freed memory. Guard both providers with a shared alive-flag that
	// NeutralizeProviders() (from Unregister(), ahead of the runtime's BridgeServer/Registry resets) flips off, so
	// every surviving widget copy short-circuits to a safe empty result. Mirrors the runtime's view-model
	// side-effect-sink nulling: same teardown race, same defense, for the widget-held providers.
	ProvidersAlive = MakeShared<bool>(true);
	TSharedPtr<bool> Alive = ProvidersAlive;
	ToolListProvider = [Alive, Inner = MoveTemp(InToolListProvider)]() -> TArray<FUnrealMcpToolListEntry>
	{
		return (Alive.IsValid() && *Alive && Inner) ? Inner() : TArray<FUnrealMcpToolListEntry>();
	};
	PortStatusProvider = [Alive, Inner = MoveTemp(InPortStatusProvider)]() -> FString
	{
		return (Alive.IsValid() && *Alive && Inner) ? Inner() : FString();
	};
	PromptProvider = MoveTemp(InPromptProvider);
	ResourceProvider = MoveTemp(InResourceProvider);

	// Slate may be unavailable in a commandlet / -nullrhi headless run; guard so the Automation/smoke runs
	// (which load the module) never crash on tab registration (mirrors the main-window tab).
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] Slate not initialized; skipping aux-window registration (headless)."));
		return;
	}

	const TSharedRef<FWorkspaceItem> Group = WorkspaceMenu::GetMenuStructure().GetToolsCategory();
	FGlobalTabmanager& TabManager = *FGlobalTabmanager::Get();

	TabManager.RegisterNomadTabSpawner(ToolsTabId, FOnSpawnTab::CreateRaw(this, &FUnrealMcpAuxWindows::SpawnToolsTab))
		.SetDisplayName(LOCTEXT("ToolsTabTitle", "MCP Tools"))
		.SetTooltipText(LOCTEXT("ToolsTabTooltip", "List and enable/disable the registered MCP tools."))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"));

	TabManager.RegisterNomadTabSpawner(PromptsTabId, FOnSpawnTab::CreateRaw(this, &FUnrealMcpAuxWindows::SpawnPromptsTab))
		.SetDisplayName(LOCTEXT("PromptsTabTitle", "MCP Prompts"))
		.SetTooltipText(LOCTEXT("PromptsTabTooltip", "List the registered MCP prompts."))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Documentation"));

	TabManager.RegisterNomadTabSpawner(ResourcesTabId, FOnSpawnTab::CreateRaw(this, &FUnrealMcpAuxWindows::SpawnResourcesTab))
		.SetDisplayName(LOCTEXT("ResourcesTabTitle", "MCP Resources"))
		.SetTooltipText(LOCTEXT("ResourcesTabTooltip", "List the registered MCP resources."))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Box"));

	TabManager.RegisterNomadTabSpawner(SettingsTabId, FOnSpawnTab::CreateRaw(this, &FUnrealMcpAuxWindows::SpawnSettingsTab))
		.SetDisplayName(LOCTEXT("SettingsTabTitle", "AI Game Developer Settings"))
		.SetTooltipText(LOCTEXT("SettingsTabTooltip", "Edit the AI Game Developer (Unreal-MCP) connection settings."))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Toolbar.Settings"));

	// Also register the Settings page as a Project Settings section rendering a SECOND SUnrealMcpSettingsWindow
	// instance (§7) — UE users expect Project Settings discoverability. The nomad tab satisfies the 4-aux-windows
	// mandate; this is the additional, conventional entry point. Both instances bind the same view-model, so they
	// read/write identical state and stay consistent.
	if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		Settings->RegisterSettings(
			SettingsContainer, SettingsCategory, SettingsSection,
			LOCTEXT("SettingsSectionName", "AI Game Developer"),
			LOCTEXT("SettingsSectionDesc", "Connection settings for the AI Game Developer (Unreal-MCP) plugin."),
			SNew(SUnrealMcpSettingsWindow)
				.ViewModel(ViewModel)
				.PortStatusProvider(PortStatusProvider));
		bSettingsRegistered = true;
	}

	bRegistered = true;
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] registered the MCP Tools/Prompts/Resources/Settings aux windows."));
}

void FUnrealMcpAuxWindows::Unregister()
{
	if (!bRegistered)
		return;

	// Neutralize the widget-held providers FIRST: a tab closed via the deferred RequestCloseTab below (or any
	// queued widget event) could otherwise paint once more after the runtime frees the registry/bridge. Runtime
	// Shutdown calls Unregister() ahead of the BridgeServer/Registry resets, so flipping the alive-flag here makes
	// every surviving provider copy return a safe empty result.
	NeutralizeProviders();

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager& TabManager = *FGlobalTabmanager::Get();
		// Close any live tab first so its hosted widget (a strong view-model ref) is torn down before the runtime
		// frees the view-model — same use-after-free guard as the main-window tab.
		for (const FName& TabId : { ToolsTabId, PromptsTabId, ResourcesTabId, SettingsTabId })
		{
			if (TSharedPtr<SDockTab> LiveTab = TabManager.FindExistingLiveTab(TabId))
				LiveTab->RequestCloseTab();
			TabManager.UnregisterNomadTabSpawner(TabId);
		}
	}

	if (bSettingsRegistered)
	{
		if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
			Settings->UnregisterSettings(SettingsContainer, SettingsCategory, SettingsSection);
		bSettingsRegistered = false;
	}

	bRegistered = false;
}

void FUnrealMcpAuxWindows::NeutralizeProviders()
{
	if (ProvidersAlive.IsValid())
		*ProvidersAlive = false;
}

TSharedRef<SDockTab> FUnrealMcpAuxWindows::SpawnToolsTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	Tab->SetContent(
		SNew(SUnrealMcpToolsWindow)
		.ViewModel(ViewModel)
		.ToolListProvider(ToolListProvider));
	return Tab;
}

TSharedRef<SDockTab> FUnrealMcpAuxWindows::SpawnPromptsTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	Tab->SetContent(
		SNew(SUnrealMcpFeatureListWindow)
		.Heading(LOCTEXT("PromptsHeading", "MCP Prompts"))
		.EmptyMessage(LOCTEXT("PromptsEmpty", "No MCP prompts are registered yet. Prompts are provided by the sidecar's McpPlugin feature managers; this window will list them once a prompts feed is surfaced to the plugin."))
		.FeatureProvider(PromptProvider));
	return Tab;
}

TSharedRef<SDockTab> FUnrealMcpAuxWindows::SpawnResourcesTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	Tab->SetContent(
		SNew(SUnrealMcpFeatureListWindow)
		.Heading(LOCTEXT("ResourcesHeading", "MCP Resources"))
		.EmptyMessage(LOCTEXT("ResourcesEmpty", "No MCP resources are registered yet. Resources are provided by the sidecar's McpPlugin feature managers; this window will list them once a resources feed is surfaced to the plugin."))
		.FeatureProvider(ResourceProvider));
	return Tab;
}

TSharedRef<SDockTab> FUnrealMcpAuxWindows::SpawnSettingsTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	Tab->SetContent(
		SNew(SUnrealMcpSettingsWindow)
		.ViewModel(ViewModel)
		.PortStatusProvider(PortStatusProvider));
	return Tab;
}

#undef LOCTEXT_NAMESPACE
