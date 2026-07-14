// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Server/UnrealMcpServerManager.h"
#include "Server/UnrealMcpServerChecksum.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpProcessUtil.h"   // shared ResolveDotNetRid + MakeExecutable

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "GenericPlatform/GenericPlatformFile.h"

#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include "FileUtilities/ZipArchiveReader.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// Single source of the consumed shared-server version. Kept in lockstep with cli/src/lib/server-version.ts
// (SERVER_VERSION) and Unity's McpServerManager.ServerVersion — the three plugins consume the SAME server.
const TCHAR* FUnrealMcpServerManager::ServerVersion = TEXT("9.0.0");
const TCHAR* FUnrealMcpServerManager::ServerPathEnvVar = TEXT("UNREAL_MCP_SERVER_PATH");

namespace
{
	// The §1.5 backoff schedule + crash-rate constants now live in FUnrealMcpSupervisedProcess.
	const TCHAR* ServerProcessNameNeedle = TEXT("gamedev-mcp-server");

	// GitHub release-asset URL for a rid + version (v-prefixed tags), matching the CLI's serverDownloadUrl.
	FString ServerDownloadUrl(const FString& Rid, const FString& Version)
	{
		return FString::Printf(
			TEXT("https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v%s/gamedev-mcp-server-%s.zip"),
			*Version, *Rid);
	}
}

FUnrealMcpServerManager::FUnrealMcpServerManager() = default;

FUnrealMcpServerManager::~FUnrealMcpServerManager()
{
	Stop(/*bForce*/ true);
#if PLATFORM_WINDOWS
	// Close the kill-on-job-close Job Object handle. If any server process is still assigned to it, closing the
	// LAST handle here trips JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE and reaps it — a final backstop on top of Stop().
	if (JobHandle != nullptr)
	{
		::CloseHandle(static_cast<HANDLE>(JobHandle));
		JobHandle = nullptr;
	}
#endif
}

FString FUnrealMcpServerManager::ServerBinaryBasename()
{
#if PLATFORM_WINDOWS
	return TEXT("gamedev-mcp-server.exe");
#else
	return TEXT("gamedev-mcp-server");
#endif
}

FString FUnrealMcpServerManager::ResolveRid(bool bArm64DirExists)
{
	// The RID resolver is shared with FUnrealMcpSidecarManager — see FUnrealMcpProcessUtil::ResolveDotNetRid
	// (§6.2). This thin forwarder keeps the tested static-member entry point.
	return FUnrealMcpProcessUtil::ResolveDotNetRid(bArm64DirExists);
}

FString FUnrealMcpServerManager::ServerInstallDir(const FString& ProjectDir, const FString& Rid)
{
	if (ProjectDir.IsEmpty() || Rid.IsEmpty())
		return FString();
	const FString Path = ProjectDir / TEXT("Intermediate") / TEXT("UnrealMCP") / TEXT("server") / Rid;
	return FPaths::ConvertRelativePathToFull(Path);
}

FString FUnrealMcpServerManager::ServerBinaryPathFor(const FString& ProjectDir, const FString& Rid)
{
	const FString Dir = ServerInstallDir(ProjectDir, Rid);
	if (Dir.IsEmpty())
		return FString();
	return Dir / ServerBinaryBasename();
}

FString FUnrealMcpServerManager::ReadVersionMarker(const FString& InstallDir)
{
	if (InstallDir.IsEmpty())
		return FString();
	const FString Marker = InstallDir / TEXT("version");
	FString Contents;
	if (FFileHelper::LoadFileToString(Contents, *Marker))
		return Contents.TrimStartAndEnd();
	return FString();
}

int32 FUnrealMcpServerManager::ParsePortFromHost(const FString& HostUrl, int32 DefaultPort)
{
	const FString Trimmed = HostUrl.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
		return DefaultPort;

	// Strip the scheme, remember it for the default-port inference.
	FString Rest = Trimmed;
	bool bHttps = false;
	bool bHadScheme = false;
	if (Rest.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)) { Rest.RightChopInline(8); bHttps = true; bHadScheme = true; }
	else if (Rest.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)) { Rest.RightChopInline(7); bHadScheme = true; }

	// Drop any path / query after the authority.
	int32 SlashIdx;
	if (Rest.FindChar(TEXT('/'), SlashIdx))
		Rest.LeftInline(SlashIdx);

	// authority is host[:port]. An IPv6 literal (in []) has colons inside the brackets — guard against it.
	int32 ColonIdx;
	if (Rest.StartsWith(TEXT("[")))
	{
		int32 CloseIdx;
		if (Rest.FindChar(TEXT(']'), CloseIdx) && CloseIdx + 1 < Rest.Len() && Rest[CloseIdx + 1] == TEXT(':'))
		{
			const FString PortStr = Rest.RightChop(CloseIdx + 2);
			const int32 Port = FCString::Atoi(*PortStr);
			if (Port > 0 && Port <= 65535)
				return Port;
		}
	}
	else if (Rest.FindLastChar(TEXT(':'), ColonIdx))
	{
		const FString PortStr = Rest.RightChop(ColonIdx + 1);
		const int32 Port = FCString::Atoi(*PortStr);
		if (Port > 0 && Port <= 65535)
			return Port;
	}

	// No explicit port: infer from the scheme, else fall back to the supplied default.
	if (bHadScheme)
		return bHttps ? 443 : 80;
	return DefaultPort;
}

FString FUnrealMcpServerManager::BuildLaunchArgs(int32 Port, int32 PluginTimeoutMs, bool bAuthRequired, const FString& Token)
{
	// Unity BuildArguments parity. client-transport is ALWAYS streamableHttp for a launched local server.
	FString Args = FString::Printf(
		TEXT("port=%d plugin-timeout=%d client-transport=streamableHttp authorization=%s"),
		Port, PluginTimeoutMs, bAuthRequired ? TEXT("required") : TEXT("none"));
	if (bAuthRequired && !Token.IsEmpty())
		Args += FString::Printf(TEXT(" token=%s"), *Token);
	return Args;
}

bool FUnrealMcpServerManager::IsLaunchAllowed(bool bIsCustomMode, bool bIsHttpTransport)
{
	return bIsCustomMode && bIsHttpTransport;
}

void FUnrealMcpServerManager::PrepareBinaryForSpawn(const FString& Path)
{
	// §6.6: ensure the apphost is executable (shared +x helper — no-op on Windows). Not fatal on failure.
	FUnrealMcpProcessUtil::MakeExecutable(Path);
}

FString FUnrealMcpServerManager::ResolveBinaryPath() const
{
	// 1. Dev/CI override — wins when set + present (mirrors the sidecar's UNREAL_MCP_BRIDGE_PATH and the CLI's
	//    UNREAL_MCP_SERVER_PATH). Skips the version check, exactly like download-server.ts's override branch.
	const FString Override = FPlatformMisc::GetEnvironmentVariable(ServerPathEnvVar);
	if (!Override.IsEmpty() && FPaths::FileExists(Override))
		return Override;

	// 2. Cached binary under the §6 install dir whose `version` marker matches the pin → reuse (no network).
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString Rid = ResolveRid();
	const FString InstallDir = ServerInstallDir(ProjectDir, Rid);
	const FString BinPath = ServerBinaryPathFor(ProjectDir, Rid);
	if (!BinPath.IsEmpty() && FPaths::FileExists(BinPath) && ReadVersionMarker(InstallDir) == ServerVersion)
		return BinPath;

	// 3. Neither resolves — caller downloads (or surfaces the CLI hint).
	return FString();
}

bool FUnrealMcpServerManager::DownloadBinaryIfNeeded(FString& OutBinaryPath)
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString Rid = ResolveRid();
	if (Rid.IsEmpty())
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] no RID for this host; cannot download the local server."));
		return false;
	}
	const FString InstallDir = ServerInstallDir(ProjectDir, Rid);
	const FString BinPath = ServerBinaryPathFor(ProjectDir, Rid);
	const FString Url = ServerDownloadUrl(Rid, ServerVersion);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] downloading gamedev-mcp-server %s (%s) from %s"), ServerVersion, *Rid, *Url);

	// Blocking HTTP GET via the engine HTTP module. We tick the module on this (game) thread until the request
	// completes or a generous deadline elapses — the user explicitly clicked Start, so a bounded synchronous
	// wait is acceptable here (matches the CLI's await downloadServer()).
	FHttpModule& Http = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));

	bool bDone = false;
	bool bOk = false;
	TArray<uint8> ZipBytes;
	Request->OnProcessRequestComplete().BindLambda(
		[&bDone, &bOk, &ZipBytes](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedOk)
		{
			bDone = true;
			if (bConnectedOk && Resp.IsValid() && Resp->GetResponseCode() == 200)
			{
				ZipBytes = Resp->GetContent();
				bOk = true;
				UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] server download completed: %d bytes."), ZipBytes.Num());
			}
			else
			{
				UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] server download failed (HTTP %d)."),
					Resp.IsValid() ? Resp->GetResponseCode() : -1);
			}
		});

	if (!Request->ProcessRequest())
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] could not start the server download request."));
		return false;
	}

	const double Deadline = FPlatformTime::Seconds() + 120.0; // 2-minute ceiling
	while (!bDone && FPlatformTime::Seconds() < Deadline)
	{
		Http.GetHttpManager().Tick(0.1f);
		FPlatformProcess::Sleep(0.05f);
	}
	if (!bDone)
	{
		Request->CancelRequest();
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] server download timed out."));
		return false;
	}
	if (!bOk || ZipBytes.Num() == 0)
		return false;

	// FAIL-CLOSED INTEGRITY GATE (verify-before-execute, issue #155). The zip bytes are in hand but UNTRUSTED.
	// Before staging/opening/launching, download the release's SHA256SUMS manifest (sibling of the zip URL
	// under the same v<ServerVersion> tag), compute the downloaded zip's SHA256 (self-contained FIPS 180-4 —
	// UE's FPlatformMisc::GetSHA256Signature is a desktop stub that asserts),
	// and compare against the manifest entry for THIS RID. On MISMATCH / MISSING entry / unparsable-or-
	// unfetchable manifest we return WITHOUT extracting or launching — an unverified binary must NEVER be
	// executed (a compromised release asset or a trusted-CA MITM would otherwise yield arbitrary code
	// execution on the developer's machine).
	if (!VerifyDownloadedZip(ZipBytes, ServerVersion, FUnrealMcpServerChecksum::ServerZipAssetName(Rid)))
		return false;

	// Stage the downloaded bytes to a temp file and open it with FZipArchiveReader (libzip-backed: it inflates
	// deflate entries on read). Mirror download-server.ts: find the binary at the shallowest path, then move it
	// + its sidecar files (appsettings.json, NLog.config, server.json, …) into the install dir.
	IFileManager& FM = IFileManager::Get();
	// Absolute temp path: CreateTempFilename's dir is project-relative; OpenRead and SaveArrayToFile can resolve a
	// relative path against different CWDs across the engine, so make it absolute to keep both on the same file.
	const FString TempZipPath = FPaths::ConvertRelativePathToFull(
		FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("gamedev-mcp-server-"), TEXT(".zip")));
	if (!FFileHelper::SaveArrayToFile(ZipBytes, *TempZipPath))
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] could not stage the downloaded server zip."));
		return false;
	}
	ON_SCOPE_EXIT { FM.Delete(*TempZipPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true); };

	IFileHandle* ZipHandle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*TempZipPath);
	if (ZipHandle == nullptr)
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] could not reopen the staged server zip at %s."), *TempZipPath);
		return false;
	}
	FZipArchiveReader Reader(ZipHandle); // takes ownership of ZipHandle (closes it in its dtor)
	if (!Reader.IsValid())
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] downloaded server zip is malformed/unreadable (%lld bytes staged)."),
			(long long)ZipBytes.Num());
		return false;
	}

	const TArray<FString> Names = Reader.GetFileNames();
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] server zip opened: %d entries."), Names.Num());
	const FString Basename = ServerBinaryBasename();

	// Find the shallowest entry whose leaf name equals the binary (flat win zip OR nested <rid>/ unix zip).
	FString BinaryEntryName;
	int32 BestDepth = MAX_int32;
	for (const FString& Name : Names)
	{
		if (Name.EndsWith(TEXT("/")))
			continue;
		if (FPaths::GetCleanFilename(Name) == Basename)
		{
			int32 Depth = 0;
			for (const TCHAR C : Name)
				if (C == TEXT('/')) ++Depth;
			if (Depth < BestDepth) { BestDepth = Depth; BinaryEntryName = Name; }
		}
	}
	if (BinaryEntryName.IsEmpty())
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] '%s' not found inside the downloaded zip."), *Basename);
		return false;
	}

	// The zip-relative prefix the binary lives in — its sidecar files sit beside it.
	FString BinaryDirPrefix;
	{
		int32 SlashIdx;
		if (BinaryEntryName.FindLastChar(TEXT('/'), SlashIdx))
			BinaryDirPrefix = BinaryEntryName.Left(SlashIdx + 1); // includes trailing '/'
	}

	// Replace any stale install dir so a partial/old extract cannot linger.
	FM.DeleteDirectory(*InstallDir, /*RequireExists*/ false, /*Tree*/ true);
	FM.MakeDirectory(*InstallDir, /*Tree*/ true);

	int32 FilesWritten = 0;
	for (const FString& Name : Names)
	{
		if (Name.EndsWith(TEXT("/"))) // directory entry
			continue;
		// Only files in the binary's own folder. NOTE: FString::StartsWith returns FALSE for an empty prefix,
		// so a FLAT win zip (BinaryDirPrefix == "") would filter out EVERYTHING — guard the empty-prefix case
		// explicitly (every entry is "in" the root folder when the prefix is empty).
		if (!BinaryDirPrefix.IsEmpty() && !Name.StartsWith(BinaryDirPrefix))
			continue;
		const FString Leaf = Name.RightChop(BinaryDirPrefix.Len());
		// Zip-slip hardening (defense in depth): only write a SINGLE safe path segment directly under InstallDir.
		// Reject anything that could escape it — a nested path (forward OR backslash), a ".." traversal segment,
		// or a non-relative / drive-letter / absolute leaf (e.g. "..\x", "C:evil", "\\unc\x"). A crafted entry
		// that the slash-only check missed is dropped here rather than written outside InstallDir.
		if (Leaf.IsEmpty()
			|| Leaf.Contains(TEXT("/")) || Leaf.Contains(TEXT("\\"))   // no nested path segments (either separator)
			|| Leaf.Contains(TEXT(".."))                                // no parent-dir traversal segment
			|| !FPaths::IsRelative(Leaf)                                // reject absolute paths
			|| Leaf.Contains(TEXT(":")))                                // reject drive-letter / ADS prefixes (e.g. "C:evil")
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] skipped unsafe zip entry leaf '%s' (from '%s')."), *Leaf, *Name);
			continue;
		}

		TArray<uint8> FileData;
		if (!Reader.TryReadFile(Name, FileData))
		{
			UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] failed to extract '%s' from the server zip."), *Name);
			return false;
		}
		const FString OutPath = InstallDir / Leaf;
		if (!FFileHelper::SaveArrayToFile(FileData, *OutPath))
		{
			UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] failed to write '%s'."), *OutPath);
			return false;
		}
		++FilesWritten;
	}

	if (!FPaths::FileExists(BinPath))
	{
		UE_LOG(LogUnrealMcp, Error,
			TEXT("[Unreal-MCP] server binary missing after unpack at: %s (entry='%s', prefix='%s', files written=%d)"),
			*BinPath, *BinaryEntryName, *BinaryDirPrefix, FilesWritten);
		return false;
	}
	PrepareBinaryForSpawn(BinPath);
	FFileHelper::SaveStringToFile(FString(ServerVersion), *(InstallDir / TEXT("version")));

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] local server ready: %s (version %s)"), *BinPath, ServerVersion);
	OutBinaryPath = BinPath;
	return true;
}

namespace
{
	// Bounded transient-retry config for the SHA256SUMS manifest fetch (mirrors Unity's
	// Sha256SumsFetchAttempts/Sha256SumsRetryDelay). A transient network error on the manifest fetch is
	// retried (the binary is already downloaded; only the integrity manifest is missing) — but a persistent
	// failure NEVER falls through to executing an unverified binary.
	constexpr int32 Sha256SumsFetchAttempts = 3;
	constexpr float Sha256SumsRetryDelaySeconds = 1.0f;
}

bool FUnrealMcpServerManager::FetchSha256SumsText(const FString& SumsUrl, FString& OutText)
{
	for (int32 Attempt = 1; Attempt <= Sha256SumsFetchAttempts; ++Attempt)
	{
		FHttpModule& Http = FHttpModule::Get();
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
		Request->SetURL(SumsUrl);
		Request->SetVerb(TEXT("GET"));

		bool bDone = false;
		bool bOk = false;
		FString Body;
		Request->OnProcessRequestComplete().BindLambda(
			[&bDone, &bOk, &Body](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedOk)
			{
				bDone = true;
				if (bConnectedOk && Resp.IsValid() && Resp->GetResponseCode() == 200)
				{
					Body = Resp->GetContentAsString();
					bOk = true;
				}
			});

		if (Request->ProcessRequest())
		{
			// Tick the HTTP module on this (game) thread until the manifest GET completes or a deadline
			// elapses — same bounded synchronous wait the zip download uses.
			const double Deadline = FPlatformTime::Seconds() + 30.0;
			while (!bDone && FPlatformTime::Seconds() < Deadline)
			{
				Http.GetHttpManager().Tick(0.1f);
				FPlatformProcess::Sleep(0.05f);
			}
			if (!bDone)
				Request->CancelRequest();
		}

		if (bOk)
		{
			OutText = Body;
			return true;
		}

		if (Attempt < Sha256SumsFetchAttempts)
		{
			UE_LOG(LogUnrealMcp, Warning,
				TEXT("[Unreal-MCP] %s fetch attempt %d/%d failed; retrying…"),
				FUnrealMcpServerChecksum::Sha256SumsAssetName, Attempt, Sha256SumsFetchAttempts);
			FPlatformProcess::Sleep(Sha256SumsRetryDelaySeconds);
		}
	}
	return false;
}

bool FUnrealMcpServerManager::VerifyDownloadedZip(const TArray<uint8>& ZipBytes, const FString& Version, const FString& AssetZipName) const
{
	const FString SumsUrl = FUnrealMcpServerChecksum::Sha256SumsUrl(Version);

	// 1) Fetch the integrity manifest (bounded transient-retry). A failure after all attempts is fail-closed.
	FString Sha256SumsText;
	if (!FetchSha256SumsText(SumsUrl, Sha256SumsText))
	{
		UE_LOG(LogUnrealMcp, Error,
			TEXT("[Unreal-MCP] refusing to launch the server: could not download the %s integrity manifest from %s ")
			TEXT("after %d attempt(s). The downloaded binary was NOT verified and will not be executed (fail-closed)."),
			FUnrealMcpServerChecksum::Sha256SumsAssetName, *SumsUrl, Sha256SumsFetchAttempts);
		return false;
	}

	// 2) Compute the downloaded zip's SHA256 (self-contained FIPS 180-4 — no third-party dep; UE's
	//    FPlatformMisc::GetSHA256Signature is a desktop stub that asserts, so we hash it ourselves).
	const FString ActualHexDigest = FUnrealMcpServerChecksum::ComputeSha256Hex(ZipBytes);

	// 3) Parse + compare via the pure-managed verifier (unit-tested by an Automation spec, no editor needed).
	const FUnrealMcpServerChecksum::EChecksumVerdict Verdict =
		FUnrealMcpServerChecksum::VerifyZipChecksum(Sha256SumsText, AssetZipName, ActualHexDigest);
	if (Verdict != FUnrealMcpServerChecksum::EChecksumVerdict::Verified)
	{
		UE_LOG(LogUnrealMcp, Error,
			TEXT("[Unreal-MCP] refusing to launch the server: %s. The binary will not be extracted or executed (fail-closed)."),
			*FUnrealMcpServerChecksum::ChecksumFailureReason(Verdict, AssetZipName));
		return false;
	}

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] verified '%s' against %s (SHA256 OK)."),
		*AssetZipName, FUnrealMcpServerChecksum::Sha256SumsAssetName);
	return true;
}

bool FUnrealMcpServerManager::SpawnProcess(const FString& InBinaryPath)
{
	uint32 NewProcId = 0;
	FProcHandle NewHandle = FPlatformProcess::CreateProc(
		*InBinaryPath,
		*LaunchArgs,
		/*bLaunchDetached*/ false,
		/*bLaunchHidden*/ true,
		/*bLaunchReallyHidden*/ true,
		&NewProcId,
		/*PriorityModifier*/ 0,
		/*OptionalWorkingDirectory*/ *FPaths::GetPath(InBinaryPath),
		/*PipeWriteChild*/ nullptr,
		/*PipeReadChild*/ nullptr);

	if (!NewHandle.IsValid())
		return false;

	{
		FScopeLock Lock(&ProcessMutex);
		ProcHandle = NewHandle;
		ProcId = NewProcId;
	}
	// Bind the child to the kill-on-job-close job so it dies with the editor by ANY exit path (graceful, hard
	// TerminateProcess, or crash) — the load-bearing guarantee for "Start then close editor → no orphan" when
	// the engine's graceful OnEnginePreExit teardown never runs. The handle (HANDLE on Windows) is ProcHandle.Get().
	BindProcessToKillOnCloseJob(NewHandle.Get());
	WritePidFile(NewProcId);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] spawned local server pid=%u on port=%d."), NewProcId, ListenPort);
	return true;
}

void FUnrealMcpServerManager::BindProcessToKillOnCloseJob(void* ProcessHandle)
{
#if PLATFORM_WINDOWS
	if (ProcessHandle == nullptr)
		return;

	// Create the job lazily on the first spawn; reuse it for restarts so one editor session = one job.
	if (JobHandle == nullptr)
	{
		HANDLE NewJob = ::CreateJobObject(nullptr, nullptr);
		if (NewJob == nullptr)
		{
			UE_LOG(LogUnrealMcp, Warning,
				TEXT("[Unreal-MCP] CreateJobObject failed (err=%lu); relying on the graceful shutdown stop only."),
				::GetLastError());
			return;
		}
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info = {};
		Info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!::SetInformationJobObject(NewJob, JobObjectExtendedLimitInformation, &Info, sizeof(Info)))
		{
			UE_LOG(LogUnrealMcp, Warning,
				TEXT("[Unreal-MCP] SetInformationJobObject failed (err=%lu); job will not kill-on-close."),
				::GetLastError());
			::CloseHandle(NewJob);
			return;
		}
		JobHandle = NewJob;
	}

	if (!::AssignProcessToJobObject(static_cast<HANDLE>(JobHandle), static_cast<HANDLE>(ProcessHandle)))
	{
		// A common benign cause: the process was created in a job that disallows nesting/breakaway. Log and move
		// on — the graceful OnEnginePreExit stop still covers a clean editor close.
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] AssignProcessToJobObject failed (err=%lu); kill-on-editor-exit not guaranteed for this server."),
			::GetLastError());
	}
	else
	{
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] local server bound to kill-on-close job."));
	}
#else
	(void)ProcessHandle; // non-Windows: graceful PreExit stop + KillTree cover the supported cases.
#endif
}

bool FUnrealMcpServerManager::Start(int32 Port, int32 PluginTimeoutMs, bool bAuthRequired, const FString& Token)
{
	if (IsRunning() || bStarting)
	{
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] local server already running/starting; Start is a no-op."));
		return true;
	}

	bStarting = true;
	ListenPort = Port;
	LaunchArgs = BuildLaunchArgs(Port, PluginTimeoutMs, bAuthRequired, Token);

	// Resolve the binary (override → cache); download on a miss.
	BinaryPath = ResolveBinaryPath();
	if (BinaryPath.IsEmpty())
	{
		FString Downloaded;
		if (!DownloadBinaryIfNeeded(Downloaded))
		{
			bStarting = false;
			UE_LOG(LogUnrealMcp, Error,
				TEXT("[Unreal-MCP] could not resolve or download the local gamedev-mcp-server. ")
				TEXT("Set %s to a local build, or run `unreal-mcp-cli` to populate the server cache."),
				ServerPathEnvVar);
			return false;
		}
		BinaryPath = Downloaded;
	}

	PrepareBinaryForSpawn(BinaryPath);

	// Free the port if an orphaned server from a previous session is squatting on it.
	KillOrphanedServerOnPort(Port);

	if (!SpawnProcess(BinaryPath))
	{
		bStarting = false;
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] failed to spawn the local server from '%s'."), *BinaryPath);
		return false;
	}

	Watchdog.ResetRestartCount();   // fresh Start zeroes the restart counter (Watchdog.Start resets the crash window).
	StartWatchdog();
	bStarting = false;
	return true;
}

bool FUnrealMcpServerManager::ReattachIfRunning(int32 Port, int32 PluginTimeoutMs, bool bAuthRequired, const FString& Token)
{
	const int32 SavedPid = ReadPidFile();
	if (SavedPid <= 0 || !IsServerProcessAlive(SavedPid))
	{
		ClearPidFile();
		return false;
	}

	// Adopt the survivor: open a handle so a later Stop can terminate it, and re-arm supervision.
	FProcHandle Adopted = FPlatformProcess::OpenProcess(static_cast<uint32>(SavedPid));
	if (!Adopted.IsValid())
	{
		ClearPidFile();
		return false;
	}

	ListenPort = Port;
	LaunchArgs = BuildLaunchArgs(Port, PluginTimeoutMs, bAuthRequired, Token);
	BinaryPath = ResolveBinaryPath();
	{
		FScopeLock Lock(&ProcessMutex);
		ProcHandle = Adopted;
		ProcId = static_cast<uint32>(SavedPid);
	}
	// Re-bind the adopted survivor to a kill-on-close job so it too dies with THIS editor session by any exit
	// path (the original spawning session's job died with that session's editor).
	BindProcessToKillOnCloseJob(Adopted.Get());
	// Reattach does NOT reset the restart counter (only a fresh Start does); Watchdog.Start resets the crash window.
	StartWatchdog();
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] reattached to existing local server pid=%d on port=%d."), SavedPid, Port);
	return true;
}

void FUnrealMcpServerManager::StartWatchdog()
{
	// The §1.5 crash-restart loop lives in FUnrealMcpSupervisedProcess; supply the server-specific liveness probe
	// + respawn + give-up. The liveness read and the dead-handle close both take ProcessMutex (the watchdog thread
	// otherwise owns ProcHandle mutation); the respawn first frees any orphan squatting on the listen port, then
	// re-spawns; give-up clears the pid file. All run ON the watchdog thread.
	Watchdog.Start(
		/*IsAlive*/ [this]() -> bool
		{
			FScopeLock Lock(&ProcessMutex);
			return ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcHandle);
		},
		/*Respawn*/ [this]() -> bool
		{
			{
				FScopeLock Lock(&ProcessMutex);
				if (ProcHandle.IsValid())
				{
					FPlatformProcess::CloseProc(ProcHandle);
					ProcHandle.Reset();
				}
			}
			if (ListenPort > 0)
				KillOrphanedServerOnPort(ListenPort);
			return SpawnProcess(BinaryPath);
		},
		/*OnGiveUp*/ [this]() { ClearPidFile(); },
		TEXT("local server"));
}

void FUnrealMcpServerManager::StopWatchdog()
{
	Watchdog.Stop();
}

void FUnrealMcpServerManager::TerminateProcess(bool bForce)
{
	FProcHandle Handle;
	{
		FScopeLock Lock(&ProcessMutex);
		Handle = ProcHandle;
	}
	if (Handle.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(Handle))
			FPlatformProcess::TerminateProc(Handle, /*KillTree*/ true);
		if (bForce)
		{
			// Bounded wait so editor shutdown does not orphan the child if TerminateProc is still propagating.
			const double Deadline = FPlatformTime::Seconds() + 5.0;
			while (FPlatformProcess::IsProcRunning(Handle) && FPlatformTime::Seconds() < Deadline)
				FPlatformProcess::Sleep(0.05f);
		}
		FPlatformProcess::CloseProc(Handle);
	}
	{
		FScopeLock Lock(&ProcessMutex);
		ProcHandle.Reset();
		ProcId = 0;
	}
}

void FUnrealMcpServerManager::Stop(bool bForce)
{
	StopWatchdog();        // stop restarting BEFORE terminating, so a relaunch cannot undo the kill
	TerminateProcess(bForce);
	ClearPidFile();
}

bool FUnrealMcpServerManager::IsRunning() const
{
	FScopeLock Lock(&ProcessMutex);
	return ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(const_cast<FProcHandle&>(ProcHandle));
}

FString FUnrealMcpServerManager::PidFilePath()
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	return ProjectDir / TEXT("Intermediate") / TEXT("UnrealMCP") / TEXT("server") / TEXT("server.pid");
}

void FUnrealMcpServerManager::WritePidFile(uint32 Pid)
{
	const FString Path = PidFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	FFileHelper::SaveStringToFile(FString::FromInt(static_cast<int32>(Pid)), *Path);
}

void FUnrealMcpServerManager::ClearPidFile()
{
	IFileManager::Get().Delete(*PidFilePath(), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
}

int32 FUnrealMcpServerManager::ReadPidFile()
{
	FString Contents;
	if (FFileHelper::LoadFileToString(Contents, *PidFilePath()))
	{
		const int32 Pid = FCString::Atoi(*Contents.TrimStartAndEnd());
		return Pid > 0 ? Pid : -1;
	}
	return -1;
}

bool FUnrealMcpServerManager::IsServerProcessAlive(int32 Pid)
{
	if (Pid <= 0)
		return false;
	const FString Name = FPlatformProcess::GetApplicationName(static_cast<uint32>(Pid));
	if (Name.IsEmpty())
		return false;
	return Name.Contains(ServerProcessNameNeedle);
}

int32 FUnrealMcpServerManager::GetPidListeningOnPort(int32 Port)
{
	int32 ReturnCode = -1;
	FString StdOut;
	FString StdErr;

#if PLATFORM_WINDOWS
	const FString Cmd = TEXT("netstat.exe");
	const FString Params = TEXT("-ano -p tcp");
#else
	const FString Cmd = TEXT("lsof");
	const FString Params = FString::Printf(TEXT("-ti tcp:%d -sTCP:LISTEN"), Port);
#endif

	if (!FPlatformProcess::ExecProcess(*Cmd, *Params, &ReturnCode, &StdOut, &StdErr))
		return -1;

#if PLATFORM_WINDOWS
	const FString PortSuffix = FString::Printf(TEXT(":%d"), Port);
	TArray<FString> Lines;
	StdOut.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (!Trimmed.Contains(TEXT("LISTENING")))
			continue;
		TArray<FString> Parts;
		Trimmed.ParseIntoArray(Parts, TEXT(" "), /*CullEmpty*/ true);
		if (Parts.Num() < 5)
			continue;
		// Parts: Proto LocalAddr ForeignAddr State PID
		if (Parts[1].EndsWith(PortSuffix))
		{
			const int32 Pid = FCString::Atoi(*Parts.Last());
			if (Pid > 0)
				return Pid;
		}
	}
#else
	const FString Trimmed = StdOut.TrimStartAndEnd();
	if (!Trimmed.IsEmpty())
	{
		TArray<FString> Lines;
		Trimmed.ParseIntoArrayLines(Lines);
		if (Lines.Num() > 0)
		{
			const int32 Pid = FCString::Atoi(*Lines[0].TrimStartAndEnd());
			if (Pid > 0)
				return Pid;
		}
	}
#endif
	return -1;
}

void FUnrealMcpServerManager::KillOrphanedServerOnPort(int32 Port)
{
	const int32 ListeningPid = GetPidListeningOnPort(Port);
	if (ListeningPid <= 0)
		return;

	uint32 OwnPid;
	{
		FScopeLock Lock(&ProcessMutex);
		OwnPid = ProcId;
	}
	if (static_cast<uint32>(ListeningPid) == OwnPid)
		return; // our own child

	// Fail safe: only kill it when it is actually a gamedev-mcp-server (never a random port owner).
	if (!IsServerProcessAlive(ListeningPid))
	{
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] port %d is held by a non-server process (pid=%d); not killing it. ")
			TEXT("Free the port or change the Custom host."), Port, ListeningPid);
		return;
	}

	UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] killing orphaned local server pid=%d holding port %d."), ListeningPid, Port);
	FProcHandle Orphan = FPlatformProcess::OpenProcess(static_cast<uint32>(ListeningPid));
	if (Orphan.IsValid())
	{
		FPlatformProcess::TerminateProc(Orphan, /*KillTree*/ true);
		const double Deadline = FPlatformTime::Seconds() + 3.0;
		while (FPlatformProcess::IsProcRunning(Orphan) && FPlatformTime::Seconds() < Deadline)
			FPlatformProcess::Sleep(0.05f);
		FPlatformProcess::CloseProc(Orphan);
	}
}
