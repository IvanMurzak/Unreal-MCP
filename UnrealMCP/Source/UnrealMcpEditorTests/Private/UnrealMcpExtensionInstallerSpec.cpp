// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Extensions/UnrealMcpExtensionInstaller.h"
#include "Extensions/UnrealMcpExtensionManager.h" // FUnrealMcpExtensionRecord

/**
 * Extensions panel — install-service specs (docs/ARCHITECTURE.md §7 item 10, issue #176). The in-editor
 * install channel (#3) is the native C++ implementation of the SAME contract the CLI's install-extension
 * (#172) implements; these specs prove the pure decisions (download-URL build, github-only host trust,
 * materialize/outcome/message), the catalog ∪ loaded ∪ disk row-merge the panel renders, and a full
 * LOCAL-SOURCE install round-trip (place in Plugins/ with build-cache filtering + enable in the .uproject +
 * idempotent re-run) — all headless. The live github-release download + the Slate window are
 * operator/network-verified (the local-source channel exercises the same post-materialization path here).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpExtensionInstallerSpec, "UnrealMcp.ExtensionInstaller",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static FString InstallerSpecScratchRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(),
			TEXT("UnrealMcpInstallerSpec"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	static void InstallerSpecWrite(const FString& Path, const FString& Contents)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		// Surface a failed scratch write here (fatal in a dev/automation build) instead of letting it turn
		// into an opaque "file not found" / "install failed" further down the spec.
		verifyf(FFileHelper::SaveStringToFile(Contents, *Path), TEXT("InstallerSpecWrite: failed to write '%s'"), *Path);
	}

	static FUnrealMcpExtensionRecord InstallerSpecRecord(const FString& Id, const FString& Version, bool bEnabled, int32 Tools, const FString& Error)
	{
		FUnrealMcpExtensionRecord R;
		R.Id = Id;
		R.DisplayName = FText::FromString(Id);
		R.Version = Version;
		R.bEnabled = bEnabled;
		R.ToolCount = Tools;
		R.Error = Error;
		return R;
	}

	static FUnrealMcpCatalogEntry InstallerSpecCatalogEntry(const FString& Id, const FString& PluginName, const FString& Version)
	{
		FUnrealMcpCatalogEntry E;
		E.ExtensionId = Id;
		E.Name = PluginName;
		E.PluginName = PluginName;
		E.Version = Version;
		E.Repo = TEXT("IvanMurzak/Some-Ext");
		return E;
	}

END_DEFINE_SPEC(FUnrealMcpExtensionInstallerSpec)

void FUnrealMcpExtensionInstallerSpec::Define()
{
	Describe("download-source helpers", [this]()
	{
		It("builds the github-release zip URL with parity to the CLI", [this]()
		{
			TestEqual("strip leading v", FUnrealMcpExtensionInstaller::StripLeadingV(TEXT("v1.2.3")), FString(TEXT("1.2.3")));
			TestEqual("release tag adds v", FUnrealMcpExtensionInstaller::ReleaseTag(TEXT("1.2.3")), FString(TEXT("v1.2.3")));
			TestEqual("release tag no double-v", FUnrealMcpExtensionInstaller::ReleaseTag(TEXT("v1.2.3")), FString(TEXT("v1.2.3")));
			TestEqual("asset name", FUnrealMcpExtensionInstaller::ExtensionAssetName(TEXT("UnrealAINiagara"), TEXT("v0.1.0")), FString(TEXT("UnrealAINiagara-0.1.0.zip")));
			TestEqual("download url",
				FUnrealMcpExtensionInstaller::ExtensionDownloadUrl(TEXT("IvanMurzak/Unreal-AI-Niagara"), TEXT("UnrealAINiagara"), TEXT("0.1.0")),
				FString(TEXT("https://github.com/IvanMurzak/Unreal-AI-Niagara/releases/download/v0.1.0/UnrealAINiagara-0.1.0.zip")));
		});

		It("fail-closes host trust to github.com over https only", [this]()
		{
			TestTrue("github https ok", FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(TEXT("https://github.com/a/b/releases/download/v1/x.zip")));
			TestFalse("http rejected", FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(TEXT("http://github.com/a/b.zip")));
			TestFalse("subdomain rejected", FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(TEXT("https://raw.github.com/a/b.zip")));
			TestFalse("look-alike host rejected", FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(TEXT("https://github.com.evil.test/a/b.zip")));
			TestFalse("userinfo trick rejected", FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(TEXT("https://github.com@evil.test/a/b.zip")));
		});
	});

	Describe("materialize + outcome decisions", [this]()
	{
		It("decides when files must be (re)materialized", [this]()
		{
			TestTrue("force always materializes", FUnrealMcpExtensionInstaller::IsMaterializeNeeded(true, true, TEXT("1.0.0"), TEXT("1.0.0")));
			TestTrue("absent materializes", FUnrealMcpExtensionInstaller::IsMaterializeNeeded(false, false, TEXT(""), TEXT("1.0.0")));
			TestTrue("version mismatch materializes", FUnrealMcpExtensionInstaller::IsMaterializeNeeded(false, true, TEXT("1.0.0"), TEXT("2.0.0")));
			TestFalse("present + same version skips", FUnrealMcpExtensionInstaller::IsMaterializeNeeded(false, true, TEXT("v1.0.0"), TEXT("1.0.0")));
			TestFalse("present + no target version skips", FUnrealMcpExtensionInstaller::IsMaterializeNeeded(false, true, TEXT("1.0.0"), TEXT("")));
		});

		It("maps changed/materialized/hadPrev to the terminal outcome", [this]()
		{
			TestTrue("nothing changed", FUnrealMcpExtensionInstaller::ComputeOutcome(false, false, false) == EUnrealMcpInstallOutcome::AlreadyUpToDate);
			TestTrue("enabled only", FUnrealMcpExtensionInstaller::ComputeOutcome(true, false, true) == EUnrealMcpInstallOutcome::Enabled);
			TestTrue("added (no prev)", FUnrealMcpExtensionInstaller::ComputeOutcome(true, true, false) == EUnrealMcpInstallOutcome::Added);
			TestTrue("updated (had prev)", FUnrealMcpExtensionInstaller::ComputeOutcome(true, true, true) == EUnrealMcpInstallOutcome::Updated);
		});

		It("builds a human message carrying the compile-on-install hint + gating list", [this]()
		{
			const FString Msg = FUnrealMcpExtensionInstaller::BuildOutcomeMessage(
				EUnrealMcpInstallOutcome::Added, TEXT("UnrealAINiagara"), TEXT(""), TEXT("0.1.0"), /*rebuild*/ true, { TEXT("Niagara") });
			TestTrue("names the plugin", Msg.Contains(TEXT("UnrealAINiagara")));
			TestTrue("lists gating Niagara", Msg.Contains(TEXT("Niagara")));
			TestTrue("surfaces the rebuild hint", Msg.Contains(TEXT("Rebuild required")));
		});
	});

	Describe("BuildRows merge (catalog ∪ loaded ∪ disk)", [this]()
	{
		It("renders an available catalog-only extension", [this]()
		{
			const TArray<FUnrealMcpCatalogEntry> Catalog = { InstallerSpecCatalogEntry(TEXT("com.x.a"), TEXT("PluginA"), TEXT("1.0.0")) };
			const TArray<FUnrealMcpExtensionRow> Rows = FUnrealMcpExtensionInstaller::BuildRows(Catalog, {}, {});
			TestEqual("one row", Rows.Num(), 1);
			TestTrue("in catalog", Rows[0].bInCatalog);
			TestFalse("not installed", Rows[0].bInstalled);
			TestFalse("not loaded", Rows[0].bLoaded);
			TestEqual("shows catalog version", Rows[0].Version, FString(TEXT("1.0.0")));
		});

		It("marks installed-on-disk-but-not-loaded and computes update-available", [this]()
		{
			const TArray<FUnrealMcpCatalogEntry> Catalog = { InstallerSpecCatalogEntry(TEXT("com.x.a"), TEXT("PluginA"), TEXT("2.0.0")) };
			TArray<FUnrealMcpInstalledOnDisk> OnDisk;
			OnDisk.Add({ TEXT("PluginA"), TEXT("1.0.0") }); // older than the catalog pin
			const TArray<FUnrealMcpExtensionRow> Rows = FUnrealMcpExtensionInstaller::BuildRows(Catalog, {}, OnDisk);
			TestTrue("installed on disk", Rows[0].bInstalled);
			TestFalse("not loaded (needs compile)", Rows[0].bLoaded);
			TestTrue("update available (1.0.0 < 2.0.0)", Rows[0].bUpdateAvailable);
			TestEqual("shows the INSTALLED version", Rows[0].Version, FString(TEXT("1.0.0")));
		});

		It("reflects a loaded provider's enable state, tool count, and error badge", [this]()
		{
			const TArray<FUnrealMcpCatalogEntry> Catalog = { InstallerSpecCatalogEntry(TEXT("com.x.a"), TEXT("PluginA"), TEXT("1.0.0")) };
			TArray<FUnrealMcpExtensionRecord> Loaded;
			Loaded.Add(InstallerSpecRecord(TEXT("com.x.a"), TEXT("1.0.0"), /*enabled*/ false, /*tools*/ 4, TEXT("bad tool id")));
			const TArray<FUnrealMcpExtensionRow> Rows = FUnrealMcpExtensionInstaller::BuildRows(Catalog, Loaded, {});
			TestTrue("loaded", Rows[0].bLoaded);
			TestFalse("disabled", Rows[0].bEnabled);
			TestEqual("tool count", Rows[0].ToolCount, 4);
			TestTrue("has error", Rows[0].bHasError);
			TestFalse("no update at same version", Rows[0].bUpdateAvailable);
		});

		It("appends a loaded provider that is not in the catalog (e.g. a local dev install)", [this]()
		{
			TArray<FUnrealMcpExtensionRecord> Loaded;
			Loaded.Add(InstallerSpecRecord(TEXT("com.dev.local"), TEXT("0.0.1"), /*enabled*/ true, /*tools*/ 1, TEXT("")));
			const TArray<FUnrealMcpExtensionRow> Rows = FUnrealMcpExtensionInstaller::BuildRows({}, Loaded, {});
			TestEqual("one extra row", Rows.Num(), 1);
			TestFalse("not in catalog", Rows[0].bInCatalog);
			TestTrue("loaded + installed", Rows[0].bLoaded && Rows[0].bInstalled);
		});
	});

	Describe("local-source install round-trip", [this]()
	{
		It("places the plugin (build-cache filtered), enables it + its gating plugin, and is idempotent", [this]()
		{
			const FString Root = InstallerSpecScratchRoot();
			const FString ProjectDir = Root / TEXT("Project");
			const FString SourceDir = Root / TEXT("Source") / TEXT("UnrealAINiagara");
			ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true); };

			// A minimal target project + a source plugin (with a gating dep + a build-cache dir that MUST be filtered).
			InstallerSpecWrite(ProjectDir / TEXT("MyProj.uproject"),
				TEXT("{\n\t\"FileVersion\": 3,\n\t\"EngineAssociation\": \"5.7\",\n\t\"Plugins\": []\n}\n"));
			InstallerSpecWrite(SourceDir / TEXT("UnrealAINiagara.uplugin"),
				TEXT("{\n\t\"VersionName\": \"0.1.0\",\n\t\"Plugins\": [ { \"Name\": \"Niagara\", \"Enabled\": true } ]\n}\n"));
			InstallerSpecWrite(SourceDir / TEXT("Source") / TEXT("Keep.txt"), TEXT("keep me"));
			InstallerSpecWrite(SourceDir / TEXT("Binaries") / TEXT("Drop.txt"), TEXT("stale build cache"));

			FUnrealMcpInstallOptions Opts;
			Opts.ProjectDir = ProjectDir;
			Opts.SourceDir = SourceDir;
			Opts.Descriptor.ExtensionId = TEXT("com.ivanmurzak.unreal-ai-niagara");
			Opts.Descriptor.PluginName = TEXT("UnrealAINiagara");
			Opts.Descriptor.Version = TEXT("0.1.0");
			Opts.Descriptor.EnginePlugins = { TEXT("Niagara") };

			const FUnrealMcpInstallResult Result = FUnrealMcpExtensionInstaller::Install(Opts);
			TestTrue("install succeeded", Result.bSuccess);
			TestTrue("outcome is Added", Result.Outcome == EUnrealMcpInstallOutcome::Added);
			TestTrue("rebuild required (source extension)", Result.bRebuildRequired);

			const FString InstalledUplugin = ProjectDir / TEXT("Plugins") / TEXT("UnrealAINiagara") / TEXT("UnrealAINiagara.uplugin");
			TestTrue("placed the .uplugin", FPaths::FileExists(InstalledUplugin));
			TestTrue("copied a Source file", FPaths::FileExists(ProjectDir / TEXT("Plugins") / TEXT("UnrealAINiagara") / TEXT("Source") / TEXT("Keep.txt")));
			TestFalse("filtered out Binaries", FPaths::FileExists(ProjectDir / TEXT("Plugins") / TEXT("UnrealAINiagara") / TEXT("Binaries") / TEXT("Drop.txt")));

			// The .uproject now enables the extension + its gating Niagara plugin.
			FString UprojectText;
			FFileHelper::LoadFileToString(UprojectText, *(ProjectDir / TEXT("MyProj.uproject")));
			TestTrue("uproject enables the extension", UprojectText.Contains(TEXT("UnrealAINiagara")));
			TestTrue("uproject enables Niagara", UprojectText.Contains(TEXT("Niagara")));
			TestTrue("result lists Niagara enabled", Result.EnabledPlugins.Contains(TEXT("Niagara")));

			// Idempotent: a second install of the same version + already enabled writes nothing new.
			const FUnrealMcpInstallResult Again = FUnrealMcpExtensionInstaller::Install(Opts);
			TestTrue("second install succeeded", Again.bSuccess);
			TestTrue("idempotent → already-up-to-date", Again.Outcome == EUnrealMcpInstallOutcome::AlreadyUpToDate);
			TestFalse("no rebuild needed the second time", Again.bRebuildRequired);
		});

		It("fails clearly when no .uproject is present in the project dir", [this]()
		{
			const FString Root = InstallerSpecScratchRoot();
			ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true); };
			IFileManager::Get().MakeDirectory(*Root, /*Tree*/ true);

			FUnrealMcpInstallOptions Opts;
			Opts.ProjectDir = Root; // exists, but holds no .uproject
			Opts.Descriptor.ExtensionId = TEXT("com.x.a");
			Opts.Descriptor.PluginName = TEXT("PluginA");
			Opts.SourceDir = Root; // irrelevant — the .uproject check fails first

			const FUnrealMcpInstallResult Result = FUnrealMcpExtensionInstaller::Install(Opts);
			TestFalse("install fails", Result.bSuccess);
			TestTrue("error mentions .uproject", Result.Error.Contains(TEXT(".uproject")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
