// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Templates/Function.h"
#include "Extensions/UnrealMcpExtensionCatalog.h"
#include "Extensions/UnrealMcpExtensionInstaller.h"
#include "Extensions/UnrealMcpExtensionManager.h" // FUnrealMcpExtensionRecord (runtime module; by value in the wiring's installed-provider signature)

class SVerticalBox;

/**
 * Wiring for the §7 Extensions panel (docs/ARCHITECTURE.md §7 item 10 — install channel #3). Bundled so the
 * hosts that construct the panel (the "AI Game Developer" main window's Extensions section, via
 * FUnrealMcpMainWindowTab) keep a readable construction call. The providers/delegates are owned by the editor
 * coordinator (catalog fetch + installer in the editor module; the enable toggle routes to the runtime
 * extension manager's §5 hot-load path). The coordinator guards the InstalledProvider with a teardown
 * alive-flag so a deferred widget paint after the runtime extension manager is freed returns empty rather than
 * dereferencing freed memory.
 */
struct FUnrealMcpExtensionsPanelWiring
{
	FString ProjectDir;
	TArray<FUnrealMcpCatalogEntry> InitialCatalog;
	TFunction<TArray<FUnrealMcpExtensionRecord>()> InstalledProvider;
	TFunction<bool(TArray<FUnrealMcpCatalogEntry>&, FString&)> CatalogFetcher;
	TFunction<void(const FString&, bool)> OnSetEnabled;
	TFunction<FUnrealMcpInstallResult(const FUnrealMcpCatalogEntry&, bool)> OnInstall;
	TFunction<FString()> OnTriggerLiveCompile;

	/** True once the coordinator has wired the live providers (false for a default-constructed placeholder). */
	bool IsWired() const { return static_cast<bool>(InstalledProvider) || static_cast<bool>(OnInstall) || static_cast<bool>(CatalogFetcher); }
};

/**
 * The "Extensions" window (docs/ARCHITECTURE.md §7 item 10) — install channel #3 of three, the most
 * NATIVE no-terminal path. A pure-Slate compound widget (NO UMG) that lists AVAILABLE (catalog) +
 * INSTALLED extensions with state and offers install / update / enable / disable from inside the editor,
 * reusing the SAME catalog format + install contract as the CLI (#172) and the app GUI so all three
 * channels are behaviorally identical (FUnrealMcpExtensionInstaller).
 *
 * The list is the pure catalog ∪ loaded-providers ∪ on-disk merge (FUnrealMcpExtensionInstaller::BuildRows):
 *  - INSTALLED + LOADED rows carry a live enable/disable toggle routed to the §5 hot-load path
 *    (FUnrealMcpExtensionManager::SetExtensionEnabled → manifest revision bump → bridge re-proxies), plus
 *    the per-extension error badge the registry exposes.
 *  - AVAILABLE (catalog-only) rows carry an Install button; an installed-but-outdated row carries Update.
 *  - After any install/update the row surfaces the compile-on-install reality (§5 key UX risk): a
 *    "needs editor restart to finish compiling" hint + a best-effort "Compile (Live Coding)" affordance.
 *
 * Catalog fetch is on-demand (a Refresh button), never on construct — so the headless boot smoke / Automation
 * runs never block on the network. The window renders the INSTALLED/LOADED set immediately from the supplied
 * providers and merges the catalog in once fetched.
 *
 * Embedded mode (issue #179): this same widget is the Extensions SECTION of the "AI Game Developer" main window
 * (Unity-parity — Unity surfaces the list inline in its main window, not a separate one). Pass `bEmbedded(true)`
 * and the widget drops its own bold "Extensions" title (the host section supplies it) and its inner FillHeight
 * SScrollBox (the host main window already scrolls — a nested FillHeight inside the host's AutoHeight slot would
 * collapse to zero), rendering the rows as an AutoHeight list instead. Every other affordance — Refresh, the
 * per-row Install / Update / enable-disable, the compile-on-install hint + Live Coding button, the error badge —
 * is byte-identical between the two modes, so there is ONE row-builder and ONE install path (no duplicated logic).
 */
class SUnrealMcpExtensionsWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpExtensionsWindow)
		: _bEmbedded(false)
	{}
		/** The UE project directory (scanned for installed plugins + the install target). */
		SLATE_ARGUMENT(FString, ProjectDir)
		/**
		 * Embedded-in-the-main-window mode (issue #179): drop the standalone title header + the inner FillHeight
		 * scroll box so the panel composes into the host main window's own scroll area as an AutoHeight section.
		 */
		SLATE_ARGUMENT(bool, bEmbedded)
		/** An initial catalog (usually empty at boot — fetched on Refresh to avoid a boot-time network call). */
		SLATE_ARGUMENT(TArray<FUnrealMcpCatalogEntry>, InitialCatalog)
		/** Snapshots the live loaded-provider records (id / display name / version / tool count / enabled / error). */
		SLATE_ARGUMENT(TFunction<TArray<FUnrealMcpExtensionRecord>()>, InstalledProvider)
		/** Fetch the shared catalog over HTTP; returns false + an error message on failure. Optional. */
		SLATE_ARGUMENT(TFunction<bool(TArray<FUnrealMcpCatalogEntry>&, FString&)>, CatalogFetcher)
		/** Enable/disable a loaded extension (the §5 hot-load toggle). Optional. */
		SLATE_ARGUMENT(TFunction<void(const FString&, bool)>, OnSetEnabled)
		/** Install/update an extension from its catalog descriptor (force re-materialize when @p bForce). Optional. */
		SLATE_ARGUMENT(TFunction<FUnrealMcpInstallResult(const FUnrealMcpCatalogEntry&, bool)>, OnInstall)
		/** Best-effort Live Coding trigger; returns a human-readable status. Optional. */
		SLATE_ARGUMENT(TFunction<FString()>, OnTriggerLiveCompile)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// When true, render as a section of the host main window (no own title, AutoHeight list) — see the class note.
	bool bEmbedded = false;
	FString ProjectDir;
	TArray<FUnrealMcpCatalogEntry> Catalog;
	TFunction<TArray<FUnrealMcpExtensionRecord>()> InstalledProvider;
	TFunction<bool(TArray<FUnrealMcpCatalogEntry>&, FString&)> CatalogFetcher;
	TFunction<void(const FString&, bool)> OnSetEnabled;
	TFunction<FUnrealMcpInstallResult(const FUnrealMcpCatalogEntry&, bool)> OnInstall;
	TFunction<FString()> OnTriggerLiveCompile;

	TSharedPtr<SVerticalBox> ListContainer;
	FString StatusMessage;
	bool bRestartHintVisible = false;

	/** Recompute the merged rows and refill the list container. */
	void RebuildList();
	TSharedRef<SWidget> BuildRow(const FUnrealMcpExtensionRow& Row);

	FReply OnRefreshClicked();
	FReply OnInstallClicked(FString ExtensionId, bool bForce);
	FReply OnCompileClicked();
	void OnEnabledChanged(FString ExtensionId, bool bEnabled);

	FText GetStatusText() const;
	EVisibility GetRestartHintVisibility() const;
};
