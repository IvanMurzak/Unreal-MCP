// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Extensions/UnrealMcpExtensionInstaller.h"
#include "Extensions/UnrealMcpUProjectPlugins.h"
#include "Extensions/UnrealMcpExtensionManager.h" // FUnrealMcpExtensionRecord (runtime module, reached via PrivateIncludePaths)
#include "UnrealMcpLog.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include "FileUtilities/ZipArchiveReader.h"

namespace
{
	/** The trusted download host — extension zips come ONLY from github.com (mirrors extension-source.ts). */
	const TCHAR* ExtInstTrustedDownloadHost = TEXT("github.com");

	/**
	 * Shared state for a bounded, synchronous HTTP wait. Heap-held via a thread-safe shared ref and captured
	 * BY VALUE in the completion lambda, so a cancelled request whose completion delegate fires on a LATER
	 * HttpManager tick (after the calling frame returned on the timeout path) writes into live memory rather
	 * than dangling stack references (use-after-return).
	 */
	struct FExtInstHttpWait
	{
		bool bDone = false;
		bool bOk = false;
		int32 ResponseCode = -1;
		TArray<uint8> Bytes; // DownloadAndExtract payload
		FString Body;        // FetchCatalogSync payload
	};

	/** Plugin-source subtrees never copied into the target project (mirrors install-extension.ts EXCLUDED_DIRS). */
	bool ExtInstIsExcludedSegment(const FString& Segment)
	{
		return Segment == TEXT("Intermediate") || Segment == TEXT("Binaries") || Segment == TEXT("Saved")
			|| Segment == TEXT("DerivedDataCache") || Segment == TEXT(".git") || Segment == TEXT(".vs")
			|| Segment == TEXT("node_modules");
	}

	/** Parse a file's JSON into an FJsonObject (or null on failure). */
	TSharedPtr<FJsonObject> ExtInstLoadJsonObject(const FString& FilePath)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath))
			return nullptr;
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			return nullptr;
		return Root;
	}

	/** Numeric dotted-version compare (`1.2.0` vs `1.10.0`). Returns -1 / 0 / 1; non-numeric parts compare as 0. */
	int32 ExtInstCompareSemver(const FString& A, const FString& B)
	{
		TArray<FString> PartsA, PartsB;
		A.ParseIntoArray(PartsA, TEXT("."));
		B.ParseIntoArray(PartsB, TEXT("."));
		const int32 Len = FMath::Max(PartsA.Num(), PartsB.Num());
		for (int32 i = 0; i < Len; ++i)
		{
			const int32 Da = PartsA.IsValidIndex(i) ? FCString::Atoi(*PartsA[i]) : 0;
			const int32 Db = PartsB.IsValidIndex(i) ? FCString::Atoi(*PartsB[i]) : 0;
			if (Da != Db)
				return Da < Db ? -1 : 1;
		}
		return 0;
	}

	/** Find the first top-level `*.uproject` in @p ProjectDir, or empty. */
	FString ExtInstFindUProjectFile(const FString& ProjectDir)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(ProjectDir / TEXT("*.uproject")), /*Files*/ true, /*Dirs*/ false);
		return Found.Num() > 0 ? (ProjectDir / Found[0]) : FString();
	}

	/** De-dupe strings case-insensitively, preserving first-seen order. */
	TArray<FString> ExtInstUniqueCaseless(const TArray<FString>& Values)
	{
		TArray<FString> Out;
		TSet<FString> Seen;
		for (const FString& V : Values)
		{
			const FString Key = V.ToLower();
			if (!Seen.Contains(Key))
			{
				Seen.Add(Key);
				Out.Add(V);
			}
		}
		return Out;
	}
}

// ---------------------------------------------------------------------------------------------------
// Pure helpers — extension-source.ts parity
// ---------------------------------------------------------------------------------------------------

FString FUnrealMcpExtensionInstaller::StripLeadingV(const FString& Version)
{
	const FString V = Version.TrimStartAndEnd();
	if (V.Len() > 0 && (V[0] == TEXT('v') || V[0] == TEXT('V')))
		return V.RightChop(1);
	return V;
}

FString FUnrealMcpExtensionInstaller::ReleaseTag(const FString& Version)
{
	const FString V = Version.TrimStartAndEnd();
	if (V.Len() > 0 && (V[0] == TEXT('v') || V[0] == TEXT('V')))
		return V;
	return FString::Printf(TEXT("v%s"), *V);
}

FString FUnrealMcpExtensionInstaller::ExtensionAssetName(const FString& PluginName, const FString& Version)
{
	return FString::Printf(TEXT("%s-%s.zip"), *PluginName, *StripLeadingV(Version));
}

FString FUnrealMcpExtensionInstaller::ExtensionDownloadUrl(const FString& Repo, const FString& PluginName, const FString& Version)
{
	// ExtensionAssetName strips the leading v internally, so pass Version through directly.
	return FString::Printf(TEXT("https://%s/%s/releases/download/%s/%s"),
		ExtInstTrustedDownloadHost, *Repo, *ReleaseTag(Version), *ExtensionAssetName(PluginName, Version));
}

bool FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(const FString& Url)
{
	const FString Trimmed = Url.TrimStartAndEnd();
	if (!Trimmed.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
		return false;
	FString Authority = Trimmed.RightChop(8); // drop "https://"
	int32 SlashIdx;
	if (Authority.FindChar(TEXT('/'), SlashIdx))
		Authority.LeftInline(SlashIdx);
	// Reject embedded userinfo ("user@github.com.evil") outright.
	if (Authority.Contains(TEXT("@")))
		return false;
	// Strip an optional :port and compare the host EXACTLY (no subdomains).
	FString Host = Authority;
	int32 ColonIdx;
	if (Host.FindChar(TEXT(':'), ColonIdx))
		Host.LeftInline(ColonIdx);
	return Host.ToLower() == FString(ExtInstTrustedDownloadHost);
}

// ---------------------------------------------------------------------------------------------------
// Pure helpers — install-extension.ts decisions
// ---------------------------------------------------------------------------------------------------

bool FUnrealMcpExtensionInstaller::IsMaterializeNeeded(
	bool bForce, bool bInstalledPresent, const FString& FromVersion, const FString& ToVersion)
{
	if (bForce || !bInstalledPresent)
		return true;
	const FString To = ToVersion.TrimStartAndEnd();
	if (To.IsEmpty())
		return false; // no target version known and already present → nothing to do
	return StripLeadingV(FromVersion) != StripLeadingV(To);
}

EUnrealMcpInstallOutcome FUnrealMcpExtensionInstaller::ComputeOutcome(
	bool bChanged, bool bMaterializeNeeded, bool bHadPrevVersion)
{
	if (!bChanged)
		return EUnrealMcpInstallOutcome::AlreadyUpToDate;
	if (!bMaterializeNeeded)
		return EUnrealMcpInstallOutcome::Enabled; // only the .uproject enable entry was (re)written
	return bHadPrevVersion ? EUnrealMcpInstallOutcome::Updated : EUnrealMcpInstallOutcome::Added;
}

FString FUnrealMcpExtensionInstaller::BuildOutcomeMessage(
	EUnrealMcpInstallOutcome Outcome, const FString& PluginName, const FString& FromVersion,
	const FString& ToVersion, bool bRebuildRequired, const TArray<FString>& GatingPlugins)
{
	const FString Gating = GatingPlugins.Num() > 0
		? FString::Printf(TEXT(" (also enabled: %s)"), *FString::Join(GatingPlugins, TEXT(", ")))
		: FString();
	const FString Compile = bRebuildRequired
		? TEXT(" Rebuild required — restart the editor (or trigger Live Coding) to finish compiling.")
		: FString();
	const FString ToSuffix = ToVersion.IsEmpty() ? FString() : FString::Printf(TEXT(" %s"), *ToVersion);
	const FString FromSuffix = FromVersion.IsEmpty() ? FString() : FString::Printf(TEXT(" %s"), *FromVersion);

	switch (Outcome)
	{
	case EUnrealMcpInstallOutcome::Added:
		return FString::Printf(TEXT("Installed extension %s%s and enabled it%s.%s"), *PluginName, *ToSuffix, *Gating, *Compile);
	case EUnrealMcpInstallOutcome::Updated:
		return FString::Printf(TEXT("Updated extension %s%s → %s%s.%s"),
			*PluginName, *FromSuffix, ToVersion.IsEmpty() ? TEXT("(unpinned)") : *ToVersion, *Gating, *Compile);
	case EUnrealMcpInstallOutcome::Enabled:
		return FString::Printf(TEXT("Enabled existing extension %s%s in the project%s.%s"), *PluginName, *FromSuffix, *Gating, *Compile);
	case EUnrealMcpInstallOutcome::AlreadyUpToDate:
	default:
		return FString::Printf(TEXT("Extension %s%s is already installed and enabled."), *PluginName, *ToSuffix);
	}
}

// ---------------------------------------------------------------------------------------------------
// The catalog ∪ loaded ∪ disk row-merge (pure)
// ---------------------------------------------------------------------------------------------------

TArray<FUnrealMcpExtensionRow> FUnrealMcpExtensionInstaller::BuildRows(
	const TArray<FUnrealMcpCatalogEntry>& Catalog,
	const TArray<FUnrealMcpExtensionRecord>& LoadedRecords,
	const TArray<FUnrealMcpInstalledOnDisk>& InstalledOnDisk)
{
	TArray<FUnrealMcpExtensionRow> Rows;
	TSet<FString> CatalogExtensionIds; // lowercased, to detect loaded-but-not-cataloged extras

	for (const FUnrealMcpCatalogEntry& Entry : Catalog)
	{
		FUnrealMcpExtensionRow Row;
		Row.ExtensionId = Entry.ExtensionId;
		Row.DisplayName = Entry.Name;
		Row.Description = Entry.Description;
		Row.PluginName = Entry.PluginName;
		Row.Repo = Entry.Repo;
		Row.CatalogVersion = Entry.Version;
		Row.bInCatalog = true;
		CatalogExtensionIds.Add(Entry.ExtensionId.ToLower());

		// On-disk match by plugin folder name (case-insensitive — UE plugin names are case-insensitive).
		FString InstalledVersion;
		for (const FUnrealMcpInstalledOnDisk& Disk : InstalledOnDisk)
		{
			if (Disk.PluginName.ToLower() == Entry.PluginName.ToLower())
			{
				Row.bInstalled = true;
				InstalledVersion = Disk.Version;
				break;
			}
		}

		// Loaded-provider match by extension id (compiled + registered via IModularFeatures).
		for (const FUnrealMcpExtensionRecord& Rec : LoadedRecords)
		{
			if (Rec.Id.ToLower() == Entry.ExtensionId.ToLower())
			{
				Row.bLoaded = true;
				Row.bInstalled = true; // loaded implies present
				Row.bEnabled = Rec.bEnabled;
				Row.bHasError = Rec.HasError();
				Row.Error = Rec.Error;
				Row.ToolCount = Rec.ToolCount;
				if (!Rec.Version.IsEmpty())
					InstalledVersion = Rec.Version;
				break;
			}
		}

		Row.Version = !InstalledVersion.IsEmpty() ? InstalledVersion : Entry.Version;
		// An update is offered when installed at a version that differs from the (newer) catalog pin.
		if (Row.bInstalled && Entry.HasVersion() && !InstalledVersion.IsEmpty()
			&& StripLeadingV(InstalledVersion) != StripLeadingV(Entry.Version))
		{
			Row.bUpdateAvailable = true;
		}
		Rows.Add(MoveTemp(Row));
	}

	// Loaded providers NOT in the catalog (e.g. a local --source dev install) — surface them too.
	for (const FUnrealMcpExtensionRecord& Rec : LoadedRecords)
	{
		if (CatalogExtensionIds.Contains(Rec.Id.ToLower()))
			continue;
		FUnrealMcpExtensionRow Row;
		Row.ExtensionId = Rec.Id;
		Row.DisplayName = Rec.DisplayName.IsEmpty() ? Rec.Id : Rec.DisplayName.ToString();
		Row.PluginName = Rec.Id;
		Row.Version = Rec.Version;
		Row.bInCatalog = false;
		Row.bInstalled = true;
		Row.bLoaded = true;
		Row.bEnabled = Rec.bEnabled;
		Row.bHasError = Rec.HasError();
		Row.Error = Rec.Error;
		Row.ToolCount = Rec.ToolCount;
		Rows.Add(MoveTemp(Row));
	}

	return Rows;
}

// ---------------------------------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------------------------------

FString FUnrealMcpExtensionInstaller::FindUPluginFile(const FString& Dir)
{
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	FString Best;
	int32 BestDepth = MAX_int32;

	TFunction<void(const FString&, int32)> Walk = [&](const FString& Current, int32 Depth)
	{
		if (Depth >= BestDepth)
			return;
		PF.IterateDirectory(*Current, [&](const TCHAR* Path, bool bIsDir) -> bool
		{
			const FString Full = Path;
			const FString Leaf = FPaths::GetCleanFilename(Full);
			if (bIsDir)
			{
				if (!ExtInstIsExcludedSegment(Leaf))
					Walk(Full, Depth + 1);
			}
			else if (Leaf.EndsWith(TEXT(".uplugin"), ESearchCase::IgnoreCase) && Depth < BestDepth)
			{
				Best = Full;
				BestDepth = Depth;
			}
			return true; // keep iterating
		});
	};
	Walk(Dir, 0);
	return Best;
}

TArray<FUnrealMcpInstalledOnDisk> FUnrealMcpExtensionInstaller::ScanInstalledPlugins(const FString& ProjectDir)
{
	TArray<FUnrealMcpInstalledOnDisk> Out;
	const FString PluginsDir = ProjectDir / TEXT("Plugins");
	IFileManager& FM = IFileManager::Get();
	if (!FM.DirectoryExists(*PluginsDir))
		return Out;

	TArray<FString> Dirs;
	FM.FindFiles(Dirs, *(PluginsDir / TEXT("*")), /*Files*/ false, /*Dirs*/ true);
	for (const FString& DirName : Dirs)
	{
		const FString PluginDir = PluginsDir / DirName;
		const FString Uplugin = FindUPluginFile(PluginDir);
		if (Uplugin.IsEmpty())
			continue;
		FUnrealMcpInstalledOnDisk Entry;
		Entry.PluginName = FPaths::GetBaseFilename(Uplugin); // UE requires folder == .uplugin basename
		Entry.Version = FUnrealMcpUProjectPlugins::ReadUPluginVersionName(ExtInstLoadJsonObject(Uplugin));
		Out.Add(MoveTemp(Entry));
	}
	return Out;
}

bool FUnrealMcpExtensionInstaller::CopyPluginTreeFiltered(const FString& SourceRoot, const FString& DestRoot, FString& OutError)
{
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	const FString SourceFull = FPaths::ConvertRelativePathToFull(SourceRoot);
	const FString DestFull = FPaths::ConvertRelativePathToFull(DestRoot);
	PF.CreateDirectoryTree(*DestFull);

	bool bOk = true;
	FString FailReason;
	// Recursive walk that PRUNES build-cache / VCS subtrees at the directory level (never descends into
	// them), rather than descending everywhere and discarding each excluded file after the fact.
	TFunction<void(const FString&, const FString&)> Walk = [&](const FString& SrcDir, const FString& DstDir)
	{
		PF.IterateDirectory(*SrcDir, [&](const TCHAR* Path, bool bIsDir) -> bool
		{
			const FString Full = Path;
			const FString Leaf = FPaths::GetCleanFilename(Full);
			if (bIsDir)
			{
				if (!ExtInstIsExcludedSegment(Leaf))
					Walk(Full, DstDir / Leaf);
				return bOk; // stop the walk once a copy has failed
			}
			const FString DestFile = DstDir / Leaf;
			PF.CreateDirectoryTree(*DstDir);
			if (!PF.CopyFile(*DestFile, Path))
			{
				bOk = false;
				FailReason = FString::Printf(TEXT("failed to copy '%s' → '%s'"), Path, *DestFile);
				return false; // stop iterating this directory
			}
			return true;
		});
	};
	Walk(SourceFull, DestFull);

	if (!bOk)
		OutError = FailReason;
	return bOk;
}

bool FUnrealMcpExtensionInstaller::PlacePluginDir(const FString& SourcePluginRoot, const FString& InstalledPath, FString& OutError)
{
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	const FString InstallReal = FPaths::ConvertRelativePathToFull(InstalledPath);
	const FString PluginsDir = FPaths::GetPath(InstallReal);
	PF.CreateDirectoryTree(*PluginsDir);

	const FString Staging = PluginsDir / FString::Printf(TEXT(".unreal-mcp-ext-staging-%u-%llx"),
		FPlatformProcess::GetCurrentProcessId(), (uint64)FPlatformTime::Cycles64());
	PF.DeleteDirectoryRecursively(*Staging);
	ON_SCOPE_EXIT { PF.DeleteDirectoryRecursively(*Staging); };

	// 1. Filtered copy into a sibling staging dir first, so the swap below only ever copies a validated
	//    tree (a mid-copy failure HERE leaves the existing install untouched — the staging dir is separate).
	if (!CopyPluginTreeFiltered(SourcePluginRoot, Staging, OutError))
		return false;
	if (FindUPluginFile(Staging).IsEmpty())
	{
		OutError = FString::Printf(TEXT("copy from '%s' produced no .uplugin"), *SourcePluginRoot);
		return false;
	}

	// 2. Swap with rollback. Preserve the prior install (move it aside to a sibling backup) BEFORE
	//    dropping it, so that if the final copy-into-place fails the project is restored to its previous
	//    working install rather than left half-written with nothing. (CopyDirectoryTree is on IPlatformFile.)
	const FString Backup = PluginsDir / FString::Printf(TEXT(".unreal-mcp-ext-backup-%u-%llx"),
		FPlatformProcess::GetCurrentProcessId(), (uint64)FPlatformTime::Cycles64());
	PF.DeleteDirectoryRecursively(*Backup);
	ON_SCOPE_EXIT { PF.DeleteDirectoryRecursively(*Backup); };

	const bool bHadExisting = PF.DirectoryExists(*InstallReal);
	if (bHadExisting && !PF.CopyDirectoryTree(*Backup, *InstallReal, /*bOverwriteAllExisting*/ true))
	{
		OutError = FString::Printf(TEXT("failed to back up the existing install at '%s' before replacing it"), *InstallReal);
		return false;
	}

	PF.DeleteDirectoryRecursively(*InstallReal);
	if (!PF.CopyDirectoryTree(*InstallReal, *Staging, /*bOverwriteAllExisting*/ true))
	{
		// Restore the prior install so a failed swap can't leave the project with no working install.
		if (bHadExisting)
		{
			PF.DeleteDirectoryRecursively(*InstallReal);
			PF.CopyDirectoryTree(*InstallReal, *Backup, /*bOverwriteAllExisting*/ true);
		}
		OutError = FString::Printf(TEXT("failed to move staged plugin into '%s'"), *InstallReal);
		return false;
	}
	return true;
}

FString FUnrealMcpExtensionInstaller::ResolveLocalPluginRoot(const FString& SourceDir, FString& OutError)
{
	const FString Resolved = FPaths::ConvertRelativePathToFull(SourceDir);
	if (!IFileManager::Get().DirectoryExists(*Resolved))
	{
		OutError = FString::Printf(TEXT("--source directory does not exist: %s"), *Resolved);
		return FString();
	}
	// A dir directly holding a .uplugin is the plugin root; otherwise the parent of the shallowest one.
	const FString Uplugin = FindUPluginFile(Resolved);
	if (Uplugin.IsEmpty())
	{
		OutError = FString::Printf(TEXT("source %s does not contain a .uplugin"), *Resolved);
		return FString();
	}
	return FPaths::GetPath(Uplugin);
}

bool FUnrealMcpExtensionInstaller::DownloadAndExtract(const FString& Url, double TimeoutSeconds, FString& OutPluginRoot, FString& OutExtractDir, FString& OutError)
{
	if (!IsTrustedDownloadUrl(Url))
	{
		OutError = FString::Printf(TEXT("refusing to download from untrusted URL '%s' (only https://github.com is trusted)"), *Url);
		return false;
	}

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] downloading extension zip from %s"), *Url);

	// Bounded synchronous HTTP GET on the game thread (the user explicitly clicked Install) — mirrors the
	// local-server download's bounded wait (FUnrealMcpServerManager::DownloadBinaryIfNeeded). The wait state
	// is heap-held + captured by value so a late completion after a timeout cancel can't write dangling refs.
	FHttpModule& Http = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));

	TSharedRef<FExtInstHttpWait, ESPMode::ThreadSafe> State = MakeShared<FExtInstHttpWait, ESPMode::ThreadSafe>();
	Request->OnProcessRequestComplete().BindLambda(
		[State](FHttpRequestPtr, FHttpResponsePtr Resp, bool bConnectedOk)
		{
			State->bDone = true;
			State->ResponseCode = Resp.IsValid() ? Resp->GetResponseCode() : -1;
			if (bConnectedOk && Resp.IsValid() && State->ResponseCode == 200)
			{
				State->Bytes = Resp->GetContent();
				State->bOk = true;
			}
		});

	if (!Request->ProcessRequest())
	{
		OutError = TEXT("could not start the extension download request");
		return false;
	}

	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!State->bDone && FPlatformTime::Seconds() < Deadline)
	{
		Http.GetHttpManager().Tick(0.1f);
		FPlatformProcess::Sleep(0.05f);
	}
	if (!State->bDone)
	{
		Request->CancelRequest();
		OutError = TEXT("the extension download timed out");
		return false;
	}
	if (!State->bOk || State->Bytes.Num() == 0)
	{
		OutError = FString::Printf(TEXT("extension download failed (HTTP %d) from %s. Verify the release exists, or install from a local source."),
			State->ResponseCode, *Url);
		return false;
	}

	// Stage the bytes to a temp file and open with FZipArchiveReader (libzip-backed), exactly as the server
	// manager does. Extract every entry into a fresh dir, zip-slip-guarded, then return the dir holding the
	// shallowest .uplugin as the plugin root for PlacePluginDir.
	IFileManager& FM = IFileManager::Get();
	const FString TempZip = FPaths::ConvertRelativePathToFull(
		FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("unreal-mcp-ext-"), TEXT(".zip")));
	if (!FFileHelper::SaveArrayToFile(State->Bytes, *TempZip))
	{
		OutError = TEXT("could not stage the downloaded extension zip");
		return false;
	}
	ON_SCOPE_EXIT { FM.Delete(*TempZip, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true); };

	IFileHandle* ZipHandle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*TempZip);
	if (ZipHandle == nullptr)
	{
		OutError = TEXT("could not reopen the staged extension zip");
		return false;
	}
	FZipArchiveReader Reader(ZipHandle); // takes ownership of the handle
	if (!Reader.IsValid())
	{
		OutError = TEXT("the downloaded extension zip is malformed / unreadable");
		return false;
	}

	const FString ExtractDir = FPaths::ConvertRelativePathToFull(
		FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("unreal-mcp-ext-extract-"), TEXT("")));
	FM.MakeDirectory(*ExtractDir, /*Tree*/ true);
	// Surface the temp extract dir so the caller deletes it once PlacePluginDir has consumed the plugin
	// root inside it — the returned OutPluginRoot points INTO this dir, so it cannot be cleaned here.
	OutExtractDir = ExtractDir;

	const TArray<FString> Names = Reader.GetFileNames();
	for (const FString& Name : Names)
	{
		if (Name.EndsWith(TEXT("/")))
			continue; // directory entry
		// Zip-slip guard: the resolved path must stay under ExtractDir (mirrors the CLI's extractZip).
		const FString Target = FPaths::ConvertRelativePathToFull(ExtractDir / Name);
		if (Target != ExtractDir && !Target.StartsWith(ExtractDir + TEXT("/")) && !Target.StartsWith(ExtractDir + TEXT("\\")))
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] skipped suspicious zip entry escaping the extract dir: %s"), *Name);
			continue;
		}
		TArray<uint8> FileData;
		if (!Reader.TryReadFile(Name, FileData))
		{
			OutError = FString::Printf(TEXT("failed to extract '%s' from the extension zip"), *Name);
			return false;
		}
		FM.MakeDirectory(*FPaths::GetPath(Target), /*Tree*/ true);
		if (!FFileHelper::SaveArrayToFile(FileData, *Target))
		{
			OutError = FString::Printf(TEXT("failed to write extracted file '%s'"), *Target);
			return false;
		}
	}

	const FString Uplugin = FindUPluginFile(ExtractDir);
	if (Uplugin.IsEmpty())
	{
		OutError = FString::Printf(TEXT("downloaded zip did not contain a .uplugin (%s is not a valid extension release)"), *Url);
		return false;
	}
	OutPluginRoot = FPaths::GetPath(Uplugin);
	return true;
}

bool FUnrealMcpExtensionInstaller::FetchCatalogSync(
	const FString& Url, double TimeoutSeconds, TArray<FUnrealMcpCatalogEntry>& OutEntries, FString& OutError)
{
	const FString Trimmed = Url.TrimStartAndEnd();
	if (!Trimmed.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("the catalog URL must be https");
		return false;
	}

	FHttpModule& Http = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
	Request->SetURL(Trimmed);
	Request->SetVerb(TEXT("GET"));

	TSharedRef<FExtInstHttpWait, ESPMode::ThreadSafe> State = MakeShared<FExtInstHttpWait, ESPMode::ThreadSafe>();
	Request->OnProcessRequestComplete().BindLambda(
		[State](FHttpRequestPtr, FHttpResponsePtr Resp, bool bConnectedOk)
		{
			State->bDone = true;
			State->ResponseCode = Resp.IsValid() ? Resp->GetResponseCode() : -1;
			if (bConnectedOk && Resp.IsValid() && State->ResponseCode == 200)
			{
				State->Body = Resp->GetContentAsString();
				State->bOk = true;
			}
		});

	if (!Request->ProcessRequest())
	{
		OutError = TEXT("could not start the catalog fetch request");
		return false;
	}
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!State->bDone && FPlatformTime::Seconds() < Deadline)
	{
		Http.GetHttpManager().Tick(0.1f);
		FPlatformProcess::Sleep(0.05f);
	}
	if (!State->bDone)
	{
		Request->CancelRequest();
		OutError = TEXT("the catalog fetch timed out");
		return false;
	}
	if (!State->bOk)
	{
		OutError = FString::Printf(TEXT("could not fetch the catalog (HTTP %d) from %s"), State->ResponseCode, *Trimmed);
		return false;
	}

	FString Warning;
	if (!FUnrealMcpExtensionCatalog::ParseCatalogJson(State->Body, OutEntries, OutError, Warning))
		return false;
	if (!Warning.IsEmpty())
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] %s"), *Warning);
	return true;
}

// ---------------------------------------------------------------------------------------------------
// Install entry point
// ---------------------------------------------------------------------------------------------------

FUnrealMcpInstallResult FUnrealMcpExtensionInstaller::Install(const FUnrealMcpInstallOptions& Options)
{
	FUnrealMcpInstallResult Result;
	Result.ExtensionId = Options.Descriptor.ExtensionId;
	Result.PluginName = Options.Descriptor.PluginName;

	const FString ProjectDir = Options.ProjectDir.TrimStartAndEnd();
	if (ProjectDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*ProjectDir))
	{
		Result.Error = FString::Printf(TEXT("project directory does not exist: %s"), *ProjectDir);
		return Result;
	}
	const FString PluginName = Options.Descriptor.PluginName.TrimStartAndEnd();
	if (PluginName.IsEmpty())
	{
		Result.Error = TEXT("the catalog descriptor has no pluginName");
		return Result;
	}

	const FString UprojectPath = ExtInstFindUProjectFile(ProjectDir);
	if (UprojectPath.IsEmpty())
	{
		Result.Error = FString::Printf(TEXT("no .uproject found in %s — run install inside an Unreal project directory"), *ProjectDir);
		return Result;
	}
	Result.UprojectPath = UprojectPath;

	const FString ToVersion = Options.Descriptor.HasVersion() ? Options.Descriptor.Version.TrimStartAndEnd() : FString();
	Result.ToVersion = ToVersion;
	const FString InstalledPath = ProjectDir / TEXT("Plugins") / PluginName;
	Result.InstalledPath = InstalledPath;

	const FString InstalledUplugin = FindUPluginFile(InstalledPath);
	const FString FromVersion = InstalledUplugin.IsEmpty()
		? FString()
		: FUnrealMcpUProjectPlugins::ReadUPluginVersionName(ExtInstLoadJsonObject(InstalledUplugin));
	Result.FromVersion = FromVersion;
	const bool bInstalledPresent = !InstalledUplugin.IsEmpty();
	const bool bMaterializeNeeded = IsMaterializeNeeded(Options.bForce, bInstalledPresent, FromVersion, ToVersion);

	// 1. Materialize the plugin files when needed (local source, else github release).
	if (bMaterializeNeeded)
	{
		FString PluginRoot;
		FString Err;
		// The github-release path extracts into a temp dir whose lifetime must outlast DownloadAndExtract
		// (PluginRoot points inside it) but end after PlacePluginDir consumes it — clean it on block exit.
		FString ExtractDirToClean;
		ON_SCOPE_EXIT
		{
			if (!ExtractDirToClean.IsEmpty())
				IFileManager::Get().DeleteDirectory(*ExtractDirToClean, /*RequireExists*/ false, /*Tree*/ true);
		};
		if (!Options.SourceDir.TrimStartAndEnd().IsEmpty())
		{
			PluginRoot = ResolveLocalPluginRoot(Options.SourceDir, Err);
			if (PluginRoot.IsEmpty())
			{
				Result.Error = Err;
				return Result;
			}
		}
		else if (!Options.Descriptor.Repo.TrimStartAndEnd().IsEmpty() && !ToVersion.IsEmpty())
		{
			const FString Url = ExtensionDownloadUrl(Options.Descriptor.Repo, PluginName, ToVersion);
			FString ExtractedRoot;
			if (!DownloadAndExtract(Url, Options.DownloadTimeoutSeconds, ExtractedRoot, ExtractDirToClean, Err))
			{
				Result.Error = Err;
				return Result;
			}
			PluginRoot = ExtractedRoot;
		}
		else if (Options.Descriptor.Repo.TrimStartAndEnd().IsEmpty())
		{
			Result.Error = FString::Printf(
				TEXT("no download source for '%s': the catalog entry has no release repo. Install from a local source instead."), *PluginName);
			return Result;
		}
		else
		{
			Result.Error = FString::Printf(
				TEXT("no version to download for '%s'. Set a catalog pin, or install from a local source."), *PluginName);
			return Result;
		}

		if (!PlacePluginDir(PluginRoot, InstalledPath, Err))
		{
			Result.Error = Err;
			return Result;
		}

		// The placed descriptor basename MUST equal the plugin name or UE will not load it.
		const FString Placed = FindUPluginFile(InstalledPath);
		if (Placed.IsEmpty())
		{
			Result.Error = FString::Printf(TEXT("no .uplugin found after materializing %s into %s"), *PluginName, *InstalledPath);
			return Result;
		}
		const FString PlacedName = FPaths::GetBaseFilename(Placed);
		if (PlacedName.ToLower() != PluginName.ToLower())
		{
			Result.Error = FString::Printf(
				TEXT("materialized descriptor '%s.uplugin' does not match the expected plugin name '%s' (UE requires the folder and .uplugin share a name)"),
				*PlacedName, *PluginName);
			return Result;
		}
	}

	// 2. Enable the extension + its gating engine plugins in the .uproject (idempotent).
	const FString EffectiveUplugin = FindUPluginFile(InstalledPath);
	const TArray<FString> DeclaredDeps = EffectiveUplugin.IsEmpty()
		? TArray<FString>()
		: FUnrealMcpUProjectPlugins::ParseUPluginDependencies(ExtInstLoadJsonObject(EffectiveUplugin));
	TArray<FString> Gating = DeclaredDeps;
	Gating.Append(Options.Descriptor.EnginePlugins);
	Gating = ExtInstUniqueCaseless(Gating);

	TArray<FString> ToEnable;
	ToEnable.Add(PluginName);
	ToEnable.Append(Gating);

	FString UprojectText;
	if (!FFileHelper::LoadFileToString(UprojectText, *UprojectPath))
	{
		Result.Error = FString::Printf(TEXT("could not read the .uproject at %s"), *UprojectPath);
		return Result;
	}
	FUnrealMcpEnablePluginsResult Enable;
	FString EnableErr;
	if (!FUnrealMcpUProjectPlugins::EnablePluginsInUProject(UprojectText, ToEnable, Enable, EnableErr))
	{
		Result.Error = EnableErr;
		return Result;
	}
	if (Enable.bChanged && !FFileHelper::SaveStringToFile(Enable.Text, *UprojectPath))
	{
		Result.Error = FString::Printf(TEXT("could not write the updated .uproject at %s"), *UprojectPath);
		return Result;
	}
	Result.EnabledPlugins = Enable.AddedOrFlipped;

	const bool bChanged = bMaterializeNeeded || Enable.bChanged;
	Result.bChanged = bChanged;

	// minCoreVersion advisory (warn-only, never blocks the install — mirrors the CLI).
	const FString MinCore = Options.Descriptor.MinCoreVersion.TrimStartAndEnd();
	if (!MinCore.IsEmpty())
	{
		const FString CoreUplugin = ProjectDir / TEXT("Plugins") / TEXT("UnrealMCP") / TEXT("UnrealMCP.uplugin");
		if (!FPaths::FileExists(CoreUplugin))
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("extension requires core Unreal-MCP >= %s, but no UnrealMCP plugin was found at %s."), *MinCore, *CoreUplugin));
		}
		else
		{
			const FString CoreVersion = FUnrealMcpUProjectPlugins::ReadUPluginVersionName(ExtInstLoadJsonObject(CoreUplugin));
			if (!CoreVersion.IsEmpty() && ExtInstCompareSemver(CoreVersion, MinCore) < 0)
			{
				Result.Warnings.Add(FString::Printf(
					TEXT("extension requires core Unreal-MCP >= %s, but the installed core plugin is %s. Update it."), *MinCore, *CoreVersion));
			}
		}
	}

	// 3. The extension ships as SOURCE: the editor recompiles on next open (no eager UBT in-editor here —
	//    the panel offers a Live Coding trigger). rebuildRequired iff something changed and was not compiled.
	Result.bRebuildRequired = bChanged;
	Result.Outcome = ComputeOutcome(bChanged, bMaterializeNeeded, !FromVersion.IsEmpty());
	Result.Message = BuildOutcomeMessage(Result.Outcome, PluginName, FromVersion, ToVersion, Result.bRebuildRequired, Gating);
	Result.bSuccess = true;
	return Result;
}
