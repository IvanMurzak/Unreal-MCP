// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

// The local server's download path, in its own file because the Fab asset does not ship it (ARCHITECTURE §6.8).
#include "Server/UnrealMcpServerManager.h"
#include "Server/UnrealMcpServerChecksum.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpProcessUtil.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "GenericPlatform/GenericPlatformFile.h"

#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include "FileUtilities/ZipArchiveReader.h"

namespace
{
	// GitHub release-asset URL for a rid + version (v-prefixed tags), matching the CLI's serverDownloadUrl.
	FString ServerDownloadUrl(const FString& Rid, const FString& Version)
	{
		return FString::Printf(
			TEXT("https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v%s/gamedev-mcp-server-%s.zip"),
			*Version, *Rid);
	}
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
