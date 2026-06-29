// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/UnrealMcpAuxWindows.h"
#include "UI/SUnrealMcpSerializationCheckWindow.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UnrealMcpLog.h"

#include "Misc/App.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

const FName FUnrealMcpAuxWindows::ToolsTabId(TEXT("UnrealMcpToolsWindow"));
const FName FUnrealMcpAuxWindows::PromptsTabId(TEXT("UnrealMcpPromptsWindow"));
const FName FUnrealMcpAuxWindows::ResourcesTabId(TEXT("UnrealMcpResourcesWindow"));
const FName FUnrealMcpAuxWindows::SerializationCheckTabId(TEXT("UnrealMcpSerializationCheckWindow"));
const FName FUnrealMcpAuxWindows::ExtensionsTabId(TEXT("UnrealMcpExtensionsWindow"));

void FUnrealMcpAuxWindows::Register(
	const TSharedRef<FUnrealMcpEditorViewModel>& InViewModel,
	TFunction<TArray<FUnrealMcpToolListEntry>()> InToolListProvider,
	TFunction<TArray<FUnrealMcpFeatureEntry>()> InPromptProvider,
	TFunction<TArray<FUnrealMcpFeatureEntry>()> InResourceProvider,
	FUnrealMcpExtensionsPanelWiring InExtensionsWiring)
{
	if (bRegistered)
		return;

	ViewModel = InViewModel;
	ExtensionsWiring = MoveTemp(InExtensionsWiring);

	// The Tools provider closes over the runtime-owned registry (raw, non-owning). A widget can outlive
	// Unregister() — a deferred RequestCloseTab still queued when the runtime frees those subsystems — and
	// paint once more, dereferencing freed memory. Guard the provider with a shared alive-flag that
	// NeutralizeProviders() (from Unregister(), ahead of the runtime's BridgeServer/Registry resets) flips off, so
	// every surviving widget copy short-circuits to a safe empty result. Mirrors the runtime's view-model
	// side-effect-sink nulling: same teardown race, same defense, for the widget-held provider.
	ProvidersAlive = MakeShared<bool>(true);
	TSharedPtr<bool> Alive = ProvidersAlive;
	ToolListProvider = [Alive, Inner = MoveTemp(InToolListProvider)]() -> TArray<FUnrealMcpToolListEntry>
	{
		return (Alive.IsValid() && *Alive && Inner) ? Inner() : TArray<FUnrealMcpToolListEntry>();
	};
	PromptProvider = MoveTemp(InPromptProvider);
	ResourceProvider = MoveTemp(InResourceProvider);

	// Guard the Extensions panel's loaded-provider snapshot with the SAME teardown alive-flag: a deferred
	// widget paint after Unregister() (which frees the runtime extension manager) must short-circuit to an
	// empty result rather than dereference freed memory. The enable/install delegates are click-driven on a
	// live editor, but guard the read-each-paint InstalledProvider exactly like the Tools provider.
	if (ExtensionsWiring.InstalledProvider)
	{
		ExtensionsWiring.InstalledProvider =
			[Alive, Inner = MoveTemp(ExtensionsWiring.InstalledProvider)]() -> TArray<FUnrealMcpExtensionRecord>
			{
				return (Alive.IsValid() && *Alive && Inner) ? Inner() : TArray<FUnrealMcpExtensionRecord>();
			};
	}

	// Slate may be unavailable in a commandlet / -nullrhi headless run; guard so the Automation/smoke runs
	// (which load the module) never crash on tab registration (mirrors the main-window tab).
	//
	// All aux tabs registered below are DORMANT nomad spawners that construct their widget lazily on invoke,
	// so IsInitialized() alone would suffice for them. We additionally require FApp::CanEverRender() (false
	// under `-nullrhi`) to stay consistent with the rest of the windowed UI — the same "rendering present"
	// gate the screenshot family uses for its GPU-bound branch — and to avoid the GenericWindow
	// GetRestoredDimensions crash any later eager-widget path could hit headless (issue #103). With the
	// standalone Settings window removed (issue #107), no aux registration eagerly constructs a widget.
	if (!FSlateApplication::IsInitialized() || !FApp::CanEverRender())
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] Slate unavailable or rendering disabled; skipping aux-window registration (headless / -nullrhi)."));
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

	TabManager.RegisterNomadTabSpawner(SerializationCheckTabId, FOnSpawnTab::CreateRaw(this, &FUnrealMcpAuxWindows::SpawnSerializationCheckTab))
		.SetDisplayName(LOCTEXT("SerCheckTabTitle", "Serialization Check"))
		.SetTooltipText(LOCTEXT("SerCheckTabTooltip", "Serialize a selected UObject/Actor to JSON and inspect the output (Unity-MCP parity)."))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Adjust"));

	// §7 item 10 Extensions panel — install channel #3 (browse/install/enable/disable/update extensions in-editor).
	TabManager.RegisterNomadTabSpawner(ExtensionsTabId, FOnSpawnTab::CreateRaw(this, &FUnrealMcpAuxWindows::SpawnExtensionsTab))
		.SetDisplayName(LOCTEXT("ExtensionsTabTitle", "Extensions"))
		.SetTooltipText(LOCTEXT("ExtensionsTabTooltip", "Browse, install, enable/disable, and update Unreal-MCP extensions without leaving the editor."))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"));

	// Connection settings live entirely in the single "AI Game Developer" main window (issue #107, Unity-MCP
	// parity). The separate "AI Game Developer Settings" nomad tab and the "Project → Plugins → AI Game Developer"
	// ISettingsModule section that both rendered SUnrealMcpSettingsWindow were removed — the main window's
	// connection section owns all settings, including the read-only IPC-bridge-port line folded in from the
	// removed window's Ports row.

	bRegistered = true;
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] registered the MCP Tools/Prompts/Resources aux windows."));
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
		for (const FName& TabId : { ToolsTabId, PromptsTabId, ResourcesTabId, SerializationCheckTabId, ExtensionsTabId })
		{
			if (TSharedPtr<SDockTab> LiveTab = TabManager.FindExistingLiveTab(TabId))
				LiveTab->RequestCloseTab();
			TabManager.UnregisterNomadTabSpawner(TabId);
		}
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

TSharedRef<SDockTab> FUnrealMcpAuxWindows::SpawnSerializationCheckTab(const FSpawnTabArgs& Args)
{
	// The Serialization Check window is self-contained (it serializes in-process via FUnrealMcpPropertyJson and
	// resolves the target via FUnrealMcpObjectRef) — it binds NO view-model, so there is no teardown race to
	// guard here (unlike the registry-backed Tools window).
	TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	Tab->SetContent(SNew(SUnrealMcpSerializationCheckWindow));
	return Tab;
}

TSharedRef<SDockTab> FUnrealMcpAuxWindows::SpawnExtensionsTab(const FSpawnTabArgs& Args)
{
	// The §7 Extensions panel (install channel #3). The InstalledProvider was alive-flag-guarded in Register so a
	// deferred paint after teardown returns empty rather than dereferencing the freed runtime extension manager;
	// the catalog fetch + install + enable delegates are click-driven on a live editor.
	TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	Tab->SetContent(
		SNew(SUnrealMcpExtensionsWindow)
		.ProjectDir(ExtensionsWiring.ProjectDir)
		.InitialCatalog(ExtensionsWiring.InitialCatalog)
		.InstalledProvider(ExtensionsWiring.InstalledProvider)
		.CatalogFetcher(ExtensionsWiring.CatalogFetcher)
		.OnSetEnabled(ExtensionsWiring.OnSetEnabled)
		.OnInstall(ExtensionsWiring.OnInstall)
		.OnTriggerLiveCompile(ExtensionsWiring.OnTriggerLiveCompile));
	return Tab;
}

bool FUnrealMcpAuxWindows::TryInvokeSerializationCheckTab()
{
	// IsInitialized() alone is too weak under `-nullrhi` automation (the editor runs an initialized
	// FSlateApplication with no RHI). TryInvokeTab spawns a REAL top-level SWindow regardless of whether the
	// nomad spawner is registered — if the spawner is missing it falls back to an "unrecognized tab" hosted in
	// a fresh layout window all the same. Slate then ticks and measures that window via GenericWindow's
	// GetRestoredDimensions, which is FATAL with no RHI (issue #103: the crash fires on a deferred tick a few
	// seconds after the invoke, so it lands on whichever spec is running then). Gate on FApp::CanEverRender()
	// (false under `-nullrhi`) — the same "rendering present" check the screenshot family uses — so the window
	// is never spawned headless. The DevControl `check` route + the main-window footer button both treat a
	// false return as a no-op, preserving their headless coverage.
	if (!FSlateApplication::IsInitialized() || !FApp::CanEverRender())
		return false; // headless / -nullrhi: nothing to open without a renderer

	FGlobalTabmanager::Get()->TryInvokeTab(SerializationCheckTabId);
	return true;
}

#undef LOCTEXT_NAMESPACE
