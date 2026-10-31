// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Sidecar/UnrealMcpBridgeDownloader.h"
#include "Extensions/UnrealMcpExtensionInstaller.h"   // IsTrustedDownloadUrl (github.com-only)
#include "Server/UnrealMcpServerChecksum.h"            // the coreutils SHA256SUMS parser + FIPS 180-4 SHA-256
#include "Sidecar/UnrealMcpSidecarManager.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpProcessUtil.h"

#include "Async/Async.h"
#include "FileUtilities/ZipArchiveReader.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

const TCHAR* FUnrealMcpBridgeDownloader::ManifestFileName = TEXT("SHA256SUMS");

namespace
{
	// Generous ceiling for a ~32 MB zip on a slow link; the editor stays responsive throughout (non-blocking).
	constexpr float BridgeDownloadTimeoutSeconds = 600.0f;

	bool BridgeDownloadIsCompleteDir(const FString& Dir)
	{
		return FPaths::FileExists(Dir / FUnrealMcpSidecarManager::DownloadedBridgeMarkerName)
			&& FPaths::FileExists(Dir / FUnrealMcpSidecarManager::BridgeBinaryBasename());
	}
}

FUnrealMcpBridgeDownloader::~FUnrealMcpBridgeDownloader()
{
	Cancel();
}

FString FUnrealMcpBridgeDownloader::BridgeAssetName(const FString& InRid)
{
	return FString::Printf(TEXT("unreal-mcp-bridge-%s.zip"), *InRid);
}

FString FUnrealMcpBridgeDownloader::ReleaseBaseUrl()
{
	const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("UNREAL_MCP_BRIDGE_RELEASE_BASE_URL"));
	return Override.IsEmpty() ? FString(TEXT("https://github.com/IvanMurzak/Unreal-MCP/releases/download")) : Override;
}

FString FUnrealMcpBridgeDownloader::BridgeDownloadUrl(const FString& Base, const FString& Version, const FString& InRid)
{
	if (Base.IsEmpty() || Version.IsEmpty() || InRid.IsEmpty())
		return FString();
	FString Trimmed = Base;
	Trimmed.RemoveFromEnd(TEXT("/"));
	return FString::Printf(TEXT("%s/v%s/%s"), *Trimmed, *Version, *BridgeAssetName(InRid));
}

FString FUnrealMcpBridgeDownloader::ManifestPath(const FString& PluginBaseDir)
{
	return FPaths::ConvertRelativePathToFull(
		PluginBaseDir / TEXT("Source") / TEXT("ThirdParty") / TEXT("UnrealMcpBridge") / ManifestFileName);
}

bool FUnrealMcpBridgeDownloader::VerifyBridgeZip(const FString& InManifestText, const FString& InRid, const TArray<uint8>& ZipBytes, FString& OutError)
{
	const FString Asset = BridgeAssetName(InRid);
	const FUnrealMcpServerChecksum::EChecksumVerdict Verdict = FUnrealMcpServerChecksum::VerifyZipChecksum(
		InManifestText, Asset, FUnrealMcpServerChecksum::ComputeSha256Hex(ZipBytes));
	if (Verdict == FUnrealMcpServerChecksum::EChecksumVerdict::Verified)
		return true;
	OutError = FString::Printf(
		TEXT("MCP bridge download rejected: %s (the checksum shipped inside this plugin). The file was not installed. Reinstall the plugin, or set UNREAL_MCP_BRIDGE_PATH."),
		*FUnrealMcpServerChecksum::ChecksumFailureReason(Verdict, Asset));
	return false;
}

bool FUnrealMcpBridgeDownloader::ExtractBridgeZip(const TArray<uint8>& ZipBytes, const FString& InTargetDir, FString& OutError)
{
	IFileManager& FM = IFileManager::Get();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString Parent = FPaths::GetPath(InTargetDir);
	const FString Staging = InTargetDir + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	const FString StagingZip = Staging + TEXT(".zip");
	if (!FM.MakeDirectory(*Staging, /*Tree*/ true) || !FFileHelper::SaveArrayToFile(ZipBytes, *StagingZip))
	{
		OutError = FString::Printf(TEXT("MCP bridge download could not be written to '%s'."), *Parent);
		return false;
	}
	ON_SCOPE_EXIT
	{
		FM.Delete(*StagingZip, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		FM.DeleteDirectory(*Staging, /*RequireExists*/ false, /*Tree*/ true);   // gone after a successful rename
	};

	{
		IFileHandle* ZipHandle = PlatformFile.OpenRead(*StagingZip);
		if (ZipHandle == nullptr)
		{
			OutError = TEXT("MCP bridge download could not be reopened for extraction.");
			return false;
		}
		FZipArchiveReader Reader(ZipHandle); // owns the handle; closed before the staging zip is deleted
		if (!Reader.IsValid())
		{
			OutError = TEXT("MCP bridge download is not a readable zip.");
			return false;
		}
		for (const FString& Name : Reader.GetFileNames())
		{
			// The release zips are flat (the apphost alone). Write only single safe segments; skip the rest.
			if (Name.IsEmpty() || Name.EndsWith(TEXT("/")) || Name.Contains(TEXT("/")) || Name.Contains(TEXT("\\"))
				|| Name.Contains(TEXT("..")) || Name.Contains(TEXT(":")) || !FPaths::IsRelative(Name)
				|| Name == FUnrealMcpSidecarManager::DownloadedBridgeMarkerName)
			{
				UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] skipped bridge zip entry '%s'."), *Name);
				continue;
			}
			TArray<uint8> Data;
			if (!Reader.TryReadFile(Name, Data) || !FFileHelper::SaveArrayToFile(Data, *(Staging / Name)))
			{
				OutError = FString::Printf(TEXT("MCP bridge download: failed to extract '%s'."), *Name);
				return false;
			}
		}
	}

	const FString StagedBinary = Staging / FUnrealMcpSidecarManager::BridgeBinaryBasename();
	if (!FPaths::FileExists(StagedBinary))
	{
		OutError = FString::Printf(TEXT("MCP bridge download does not contain '%s'."), *FUnrealMcpSidecarManager::BridgeBinaryBasename());
		return false;
	}
	FUnrealMcpProcessUtil::MakeExecutable(StagedBinary);
	if (!FFileHelper::SaveStringToFile(FUnrealMcpServerChecksum::ComputeSha256Hex(ZipBytes),
		*(Staging / FUnrealMcpSidecarManager::DownloadedBridgeMarkerName)))
	{
		OutError = TEXT("MCP bridge download: could not write the verification marker.");
		return false;
	}

	// Publish atomically. Another editor may have finished the same version first: keep its complete dir. A stale,
	// incomplete dir (never produced by this code, but possibly by a user) is replaced.
	if (PlatformFile.MoveFile(*InTargetDir, *Staging))
		return true;
	if (BridgeDownloadIsCompleteDir(InTargetDir))
		return true;
	FM.DeleteDirectory(*InTargetDir, /*RequireExists*/ false, /*Tree*/ true);
	if (PlatformFile.MoveFile(*InTargetDir, *Staging))
		return true;
	OutError = FString::Printf(TEXT("MCP bridge download could not be moved into '%s'."), *InTargetDir);
	return false;
}

void FUnrealMcpBridgeDownloader::Start(const FString& Version, const FString& InRid, const FString& PluginBaseDir, const FString& CacheRoot, FOnComplete OnComplete)
{
	check(IsInGameThread());
	if (State == EState::Downloading)
		return;
	bCancelled = false;
	OnCompleteCallback = MoveTemp(OnComplete);
	Rid = InRid;
	Url = BridgeDownloadUrl(ReleaseBaseUrl(), Version, InRid);
	TargetDir = FUnrealMcpSidecarManager::ComposeDownloadedBridgeDir(CacheRoot, Version, InRid);
	State = EState::Downloading;
	Message = FString::Printf(TEXT("Downloading the MCP bridge (unreal-mcp-bridge %s, %s) from GitHub…"), *Version, *InRid);

	if (Url.IsEmpty() || TargetDir.IsEmpty())
	{
		Finish(false, TEXT("Cannot download the MCP bridge: the plugin version or platform is unknown."));
		return;
	}
	const FString Manifest = ManifestPath(PluginBaseDir);
	if (!FFileHelper::LoadFileToString(ManifestText, *Manifest))
	{
		Finish(false, FString::Printf(
			TEXT("Cannot download the MCP bridge: this UnrealMCP build has no bridge checksum manifest (%s), so a download ")
			TEXT("could not be verified. Install a released plugin, or (developers) set UNREAL_MCP_BRIDGE_PATH."), *Manifest));
		return;
	}
	if (!FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(Url))
	{
		Finish(false, FString::Printf(TEXT("Cannot download the MCP bridge: refusing untrusted URL '%s' (only https://github.com)."), *Url));
		return;
	}

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] no bridge binary resolved; downloading %s"), *Url);
	Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(BridgeDownloadTimeoutSeconds);
	Request->OnProcessRequestComplete().BindThreadSafeSP(AsShared(), &FUnrealMcpBridgeDownloader::OnHttpComplete);
	if (!Request->ProcessRequest())
		Finish(false, FString::Printf(TEXT("MCP bridge download could not start (%s)."), *Url));
}

void FUnrealMcpBridgeDownloader::OnHttpComplete(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedOk)
{
	Request.Reset();
	if (bCancelled)
		return;
	if (!bConnectedOk || !Response.IsValid())
	{
		Finish(false, FString::Printf(
			TEXT("MCP bridge download failed: could not reach github.com (offline, or blocked by a firewall/proxy?). ")
			TEXT("Check the connection, then click \"Restart bridge\". URL: %s"), *Url));
		return;
	}
	if (Response->GetResponseCode() != 200)
	{
		Finish(false, FString::Printf(TEXT("MCP bridge download failed (HTTP %d) from %s. Click \"Restart bridge\" to retry."),
			Response->GetResponseCode(), *Url));
		return;
	}

	// Hash + extract off the game thread (a ~32 MB zip, ~75 MB unpacked), then finish back on it.
	TWeakPtr<FUnrealMcpBridgeDownloader, ESPMode::ThreadSafe> WeakSelf = AsShared();
	Worker = Async(EAsyncExecution::Thread,
		[WeakSelf, Bytes = Response->GetContent(), Text = ManifestText, InRid = Rid, Dir = TargetDir]()
		{
			FString Error;
			const bool bOk = FUnrealMcpBridgeDownloader::VerifyBridgeZip(Text, InRid, Bytes, Error)
				&& FUnrealMcpBridgeDownloader::ExtractBridgeZip(Bytes, Dir, Error);
			const FString Result = bOk ? Dir / FUnrealMcpSidecarManager::BridgeBinaryBasename() : Error;
			AsyncTask(ENamedThreads::GameThread, [WeakSelf, bOk, Result]()
			{
				if (TSharedPtr<FUnrealMcpBridgeDownloader, ESPMode::ThreadSafe> This = WeakSelf.Pin())
				{
					if (!This->bCancelled)
						This->Finish(bOk, Result);
				}
			});
		});
}

void FUnrealMcpBridgeDownloader::Finish(bool bOk, const FString& BinaryPathOrError)
{
	State = bOk ? EState::Ready : EState::Failed;
	Message = bOk ? FString() : BinaryPathOrError;
	if (bOk)
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] MCP bridge downloaded and verified: %s"), *BinaryPathOrError);
	}
	else
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] %s"), *BinaryPathOrError);
	}
	FOnComplete Callback = MoveTemp(OnCompleteCallback);
	OnCompleteCallback = nullptr;
	if (Callback)
		Callback(bOk, BinaryPathOrError);
}

void FUnrealMcpBridgeDownloader::Cancel()
{
	bCancelled = true;
	OnCompleteCallback = nullptr;
	if (Request.IsValid())
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
		Request.Reset();
	}
	if (Worker.IsValid())
		Worker.Wait();
	if (State == EState::Downloading)
		State = EState::Idle;
}
