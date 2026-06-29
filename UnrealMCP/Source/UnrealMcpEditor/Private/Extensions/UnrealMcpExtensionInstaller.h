// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Extensions/UnrealMcpExtensionCatalog.h"

struct FUnrealMcpExtensionRecord; // runtime module (Extensions/UnrealMcpExtensionManager.h)

/** The terminal outcome of an install (native mirror of the CLI's ExtensionInstallOutcome). */
enum class EUnrealMcpInstallOutcome : uint8
{
	Added,            // freshly installed (no prior version on disk)
	Updated,          // materialized a different version over an existing install
	Enabled,          // files already at target version; only the .uproject enable entry was (re)written
	AlreadyUpToDate,  // present at the target version and already enabled — nothing written
};

/** Options for an in-editor extension install. */
struct FUnrealMcpInstallOptions
{
	/** The UE project directory containing the `.uproject` to enable the extension in. */
	FString ProjectDir;
	/** The resolved catalog descriptor (plugin name, repo, version, gating engine plugins). */
	FUnrealMcpCatalogEntry Descriptor;
	/** Local source override: when non-empty, materialize from this directory instead of the GitHub release. */
	FString SourceDir;
	/** Force re-materialization even when the installed version already matches. */
	bool bForce = false;
	/** Bounded ceiling for the GitHub-release zip download (game-thread sync wait), seconds. */
	double DownloadTimeoutSeconds = 120.0;
};

/** The structured result of an install (mirrors the CLI's InstallExtensionResult, UE types). */
struct FUnrealMcpInstallResult
{
	bool bSuccess = false;
	EUnrealMcpInstallOutcome Outcome = EUnrealMcpInstallOutcome::AlreadyUpToDate;
	bool bChanged = false;
	/** True when the extension's C++ must be (re)compiled to load — the compile-on-install reality (§5). */
	bool bRebuildRequired = false;
	FString ExtensionId;
	FString PluginName;
	FString InstalledPath;
	FString FromVersion;
	FString ToVersion;
	/** The gating engine plugins enabled alongside the extension (e.g. Niagara). */
	TArray<FString> EnabledPlugins;
	FString UprojectPath;
	FString Message;
	TArray<FString> Warnings;
	/** Non-empty on failure (bSuccess == false). */
	FString Error;
};

/** One catalog ∪ installed row the panel renders (a merge of the catalog, loaded providers, and disk). */
struct FUnrealMcpExtensionRow
{
	FString ExtensionId;
	FString DisplayName;
	FString Description;
	FString PluginName;
	FString Repo;
	/** The version shown for this row (installed version if present, else catalog pin). */
	FString Version;
	/** The catalog-pinned version (empty when not in the catalog / unpinned). */
	FString CatalogVersion;
	bool bInCatalog = false;
	/** Present on disk under Plugins/<PluginName>/ (compiled or not). */
	bool bInstalled = false;
	/** A live IModularFeatures provider is registered (compiled + loaded) for this extension. */
	bool bLoaded = false;
	/** Enabled state (from the loaded provider's record); meaningful only when bLoaded. */
	bool bEnabled = true;
	bool bHasError = false;
	FString Error;
	int32 ToolCount = 0;
	/** Installed on disk at a version that differs from the catalog pin → an update is offered. */
	bool bUpdateAvailable = false;
};

/** A minimal on-disk install fact (plugin folder name + its `.uplugin` VersionName) the row-merge consumes. */
struct FUnrealMcpInstalledOnDisk
{
	FString PluginName;
	FString Version;
};

/**
 * The in-editor extension install SERVICE — the native C++ implementation of the SAME install contract
 * the CLI's `install-extension` (#172) implements in TypeScript, so install channel #3 (the in-editor
 * "Extensions" Slate panel, docs/ARCHITECTURE.md §7 item 10) is behaviorally identical to the CLI + app
 * channels. It reuses the plugin's existing Http/FileUtilities/Json deps:
 *
 *   1. resolve the catalog descriptor (caller supplies it);
 *   2. resolve the install source — local `SourceDir` (offline/dev/CI) or the extension's
 *      github.com-only release zip (fail-closed host trust, mirrors extension-source.ts);
 *   3. materialize into `<project>/Plugins/<PluginName>/` (stage-then-swap, build-cache filtered);
 *   4. enable the extension + its gating engine plugins in the `.uproject` Plugins[] (idempotent);
 *   5. the extension ships as SOURCE → the editor recompiles it on next open (rebuildRequired) — the
 *      panel surfaces the compile-on-install messaging + a Live Coding affordance (§5 key UX risk).
 *
 * The pure helpers (URL build, host trust, outcome/materialize decision, the catalog ∪ loaded ∪ disk
 * row-merge) carry no engine-process / no network, so every decision is unit-testable in an Automation
 * spec; the live HTTP download + real zip extract are exercised via the local `SourceDir` channel in
 * tests and operator-verified for the network channel.
 */
class FUnrealMcpExtensionInstaller
{
public:
	// --- Pure helpers (mirror extension-source.ts) ----------------------------------------------------

	/** Drop a single leading `v`/`V` from a version string. Pure. */
	static UNREALMCPEDITOR_API FString StripLeadingV(const FString& Version);
	/** The git release TAG for a version (`1.0.0` → `v1.0.0`); an already-`v` input passes through. Pure. */
	static UNREALMCPEDITOR_API FString ReleaseTag(const FString& Version);
	/** The release-asset name `<PluginName>-<version>.zip` (bare version, no leading `v`). Pure. */
	static UNREALMCPEDITOR_API FString ExtensionAssetName(const FString& PluginName, const FString& Version);
	/** The github.com release-zip download URL for an extension version. Pure string build. */
	static UNREALMCPEDITOR_API FString ExtensionDownloadUrl(const FString& Repo, const FString& PluginName, const FString& Version);
	/** Fail-closed host trust: true iff @p Url is https AND host is EXACTLY github.com. Pure. */
	static UNREALMCPEDITOR_API bool IsTrustedDownloadUrl(const FString& Url);

	// --- Pure helpers (mirror install-extension.ts decisions) -----------------------------------------

	/** True when files must be (re)materialized: force, or absent, or a known target version differs. Pure. */
	static UNREALMCPEDITOR_API bool IsMaterializeNeeded(
		bool bForce, bool bInstalledPresent, const FString& FromVersion, const FString& ToVersion);
	/** Map (changed, materialized, hadPrevVersion) → the terminal outcome. Pure. */
	static UNREALMCPEDITOR_API EUnrealMcpInstallOutcome ComputeOutcome(
		bool bChanged, bool bMaterializeNeeded, bool bHadPrevVersion);
	/** A short printable status line for an outcome (mirrors the CLI's buildMessage). Pure. */
	static UNREALMCPEDITOR_API FString BuildOutcomeMessage(
		EUnrealMcpInstallOutcome Outcome, const FString& PluginName, const FString& FromVersion,
		const FString& ToVersion, bool bRebuildRequired, const TArray<FString>& GatingPlugins);

	/** The catalog ∪ loaded-providers ∪ on-disk row-merge the panel renders. Pure. */
	static UNREALMCPEDITOR_API TArray<FUnrealMcpExtensionRow> BuildRows(
		const TArray<FUnrealMcpCatalogEntry>& Catalog,
		const TArray<FUnrealMcpExtensionRecord>& LoadedRecords,
		const TArray<FUnrealMcpInstalledOnDisk>& InstalledOnDisk);

	// --- Filesystem helpers (no network; project Saved/Intermediate scratch — spec-testable) ----------

	/** The shallowest `*.uplugin` file under @p Dir (build-cache subtrees skipped), or empty. */
	static UNREALMCPEDITOR_API FString FindUPluginFile(const FString& Dir);
	/** Scan `<ProjectDir>/Plugins/*` for installed extension plugins (folder name + `.uplugin` VersionName). */
	static UNREALMCPEDITOR_API TArray<FUnrealMcpInstalledOnDisk> ScanInstalledPlugins(const FString& ProjectDir);
	/** Copy a plugin source root into @p InstalledPath via stage-then-swap, filtering build-cache subtrees. */
	static UNREALMCPEDITOR_API bool PlacePluginDir(const FString& SourcePluginRoot, const FString& InstalledPath, FString& OutError);

	// --- The install entry point ----------------------------------------------------------------------

	/**
	 * Bounded synchronous HTTP GET of the shared catalog JSON → parsed entries (game-thread, user-initiated
	 * via the panel's Refresh). Returns false + a reason on a non-200 / timeout / malformed-JSON. The catalog
	 * URL must be https; defaults to FUnrealMcpExtensionCatalog::DefaultCatalogUrl().
	 */
	static UNREALMCPEDITOR_API bool FetchCatalogSync(
		const FString& Url, double TimeoutSeconds, TArray<FUnrealMcpCatalogEntry>& OutEntries, FString& OutError);

	/**
	 * Install (or update / enable) the extension. The local `SourceDir` channel runs fully headless; the
	 * github-release channel performs a bounded synchronous HTTP download + zip extract on the GAME thread
	 * (the user explicitly clicked Install, mirroring the local-server download's bounded wait). Returns a
	 * structured result; never throws.
	 */
	static UNREALMCPEDITOR_API FUnrealMcpInstallResult Install(const FUnrealMcpInstallOptions& Options);

private:
	/** Resolve a `--source` arg to the plugin root (dir directly holding a `.uplugin`, else its parent). */
	static FString ResolveLocalPluginRoot(const FString& SourceDir, FString& OutError);
	/** Bounded sync HTTP GET of the github-release zip → extract to a fresh staging dir → return its plugin root. */
	static bool DownloadAndExtract(const FString& Url, double TimeoutSeconds, FString& OutPluginRoot, FString& OutError);
	/** Recursively copy @p SourceRoot → @p DestRoot, skipping build-cache / VCS subtrees. */
	static bool CopyPluginTreeFiltered(const FString& SourceRoot, const FString& DestRoot, FString& OutError);
};
