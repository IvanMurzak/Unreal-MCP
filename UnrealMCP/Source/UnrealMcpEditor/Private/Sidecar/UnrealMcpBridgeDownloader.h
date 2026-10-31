// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"

class IHttpRequest;
class IHttpResponse;

/**
 * Downloads the unreal-mcp-bridge sidecar for THIS plugin version when no binary resolves (docs/ARCHITECTURE.md
 * §6.8). A Fab install ships source only — Epic rejects executables in a listing — so the editor fetches
 * `unreal-mcp-bridge-<rid>.zip` from the plugin's own GitHub release (`v<VersionName>`), verifies it against the
 * SHA-256 manifest the release job wrote INTO the plugin (`Source/ThirdParty/UnrealMcpBridge/SHA256SUMS`), and
 * extracts it into the per-user cache FUnrealMcpSidecarManager resolves (§6.3 step 3).
 *
 * Integrity: the expected digest never comes from the download origin. It ships inside the plugin package, which
 * the user already trusts to run code (the CLI minisign-verifies the source zip; Fab distributes the reviewed
 * package), so a swapped release asset is rejected. No manifest (a dev checkout) means no download — fail-closed.
 *
 * Non-blocking: the HTTP request completes on the game thread's HTTP tick; hashing + extraction run on a worker
 * thread; the completion callback runs on the game thread. Every state read/write happens on the game thread.
 */
class FUnrealMcpBridgeDownloader : public TSharedFromThis<FUnrealMcpBridgeDownloader, ESPMode::ThreadSafe>
{
public:
	enum class EState : uint8
	{
		Idle,
		Downloading,
		Ready,
		Failed
	};

	/** Called on the game thread: true + the binary path, or false + the user-facing error. */
	using FOnComplete = TFunction<void(bool /*bOk*/, const FString& /*BinaryPathOrError*/)>;

	UNREALMCPEDITOR_API ~FUnrealMcpBridgeDownloader();

	/** The checksum manifest's file name inside Source/ThirdParty/UnrealMcpBridge/ (coreutils `sha256sum` format). */
	static UNREALMCPEDITOR_API const TCHAR* ManifestFileName;

	/** `unreal-mcp-bridge-<rid>.zip` — the release asset name and the manifest key. Pure. */
	static UNREALMCPEDITOR_API FString BridgeAssetName(const FString& Rid);

	/**
	 * The release download base: UNREAL_MCP_BRIDGE_RELEASE_BASE_URL when set (a test/fork hook; still subject to the
	 * github.com-only host check and the shipped checksum), else https://github.com/IvanMurzak/Unreal-MCP/releases/download.
	 */
	static UNREALMCPEDITOR_API FString ReleaseBaseUrl();

	/** `<Base>/v<Version>/unreal-mcp-bridge-<rid>.zip`. Empty when any input is empty. Pure. */
	static UNREALMCPEDITOR_API FString BridgeDownloadUrl(const FString& Base, const FString& Version, const FString& Rid);

	/** `<PluginBaseDir>/Source/ThirdParty/UnrealMcpBridge/SHA256SUMS`. Pure. */
	static UNREALMCPEDITOR_API FString ManifestPath(const FString& PluginBaseDir);

	/**
	 * The integrity gate: true only when @p ManifestText has an entry for this RID's asset AND it equals the SHA-256
	 * of @p ZipBytes. Otherwise false with a user-facing @p OutError. Pure (no IO).
	 */
	static UNREALMCPEDITOR_API bool VerifyBridgeZip(const FString& ManifestText, const FString& Rid, const TArray<uint8>& ZipBytes, FString& OutError);

	/**
	 * Extract a verified bridge zip into @p TargetDir atomically: unpack into a sibling staging dir, write the
	 * FUnrealMcpSidecarManager::DownloadedBridgeMarkerName marker, set the exec bit (macOS/Linux), then rename the
	 * staging dir to @p TargetDir. Only flat entries (single path segment) are written — anything else is skipped
	 * (zip-slip hardening). A concurrent editor that finished first wins: its complete dir is kept. Returns false
	 * with @p OutError when the zip is unreadable or the bridge binary is missing.
	 */
	static UNREALMCPEDITOR_API bool ExtractBridgeZip(const TArray<uint8>& ZipBytes, const FString& TargetDir, FString& OutError);

	/**
	 * Start the download of @p Version for @p Rid (game thread). No-op while a download is already running. Fails
	 * fast (callback still invoked, on the game thread) when the plugin carries no manifest or the URL is untrusted.
	 */
	UNREALMCPEDITOR_API void Start(const FString& Version, const FString& Rid, const FString& PluginBaseDir, const FString& CacheRoot, FOnComplete OnComplete);

	/** Cancel an in-flight request and wait for a running extraction (shutdown). The callback is not invoked afterwards. */
	UNREALMCPEDITOR_API void Cancel();

	EState GetState() const { return State; }
	/** The progress line while downloading, the error after a failure, empty otherwise. */
	const FString& GetMessage() const { return Message; }

private:
	void Finish(bool bOk, const FString& BinaryPathOrError);
	void OnHttpComplete(TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> InRequest, TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bConnectedOk);

	EState State = EState::Idle;
	FString Message;
	FString Url;
	FString Rid;
	FString ManifestText;
	FString TargetDir;
	FOnComplete OnCompleteCallback;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
	TFuture<void> Worker;
	bool bCancelled = false;
};
