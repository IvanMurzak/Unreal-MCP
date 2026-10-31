// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "FileUtilities/ZipArchiveWriter.h"
#include "Extensions/UnrealMcpExtensionInstaller.h"
#include "Server/UnrealMcpServerChecksum.h"
#include "Sidecar/UnrealMcpBridgeDownloader.h"
#include "Sidecar/UnrealMcpSidecarManager.h"

/**
 * The §6.8 bridge download (docs/ARCHITECTURE.md): the RID → release-asset URL contract, the github.com-only host
 * check it relies on, the shipped-manifest integrity gate (accept / tampered / missing entry / cross-RID / no
 * manifest), and the atomic, zip-slip-hardened extraction into the cache dir the resolver reads.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpBridgeDownloaderSpec, "UnrealMcp.BridgeDownloader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// SHA-256("abc") — the FIPS 180-4 reference vector, so the fixture zip "bytes" need no real zip.
	static FString BridgeDlSpecAbcDigest()
	{
		return TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	}

	static TArray<uint8> BridgeDlSpecBytes(const ANSICHAR* Text)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	}

	static FString BridgeDlSpecDigest(const ANSICHAR* Text)
	{
		return FUnrealMcpServerChecksum::ComputeSha256Hex(BridgeDlSpecBytes(Text));
	}

	static bool BridgeDlSpecExtract(const TArray<uint8>& Zip, const FString& Target, FString& Error)
	{
		return FUnrealMcpBridgeDownloader::ExtractBridgeZip(Zip, FUnrealMcpServerChecksum::ComputeSha256Hex(Zip), Target, Error);
	}

	static FString BridgeDlSpecRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("UnrealMcpBridgeDownloaderSpec"));
	}

	// A real (stored) zip holding @p Entries, read back as bytes.
	static TArray<uint8> BridgeDlSpecZip(const TMap<FString, FString>& Entries)
	{
		const FString Path = BridgeDlSpecRoot() / (FGuid::NewGuid().ToString() + TEXT(".zip"));
		IFileManager::Get().MakeDirectory(*BridgeDlSpecRoot(), /*Tree*/ true);
		{
			FZipArchiveWriter Writer(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path));
			for (const TPair<FString, FString>& Entry : Entries)
			{
				const FTCHARToUTF8 Utf8(*Entry.Value);
				Writer.AddFile(Entry.Key, TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), FDateTime::Now());
			}
		}
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Path);
		return Bytes;
	}

	static void BridgeDlSpecCleanup()
	{
		IFileManager::Get().DeleteDirectory(*BridgeDlSpecRoot(), /*RequireExists*/ false, /*Tree*/ true);
	}

END_DEFINE_SPEC(FUnrealMcpBridgeDownloaderSpec)

void FUnrealMcpBridgeDownloaderSpec::Define()
{
	Describe("BridgeDownloadUrl", [this]()
	{
		It("maps a RID + plugin version to the v-prefixed GitHub release asset", [this]()
		{
			TestEqual(TEXT("asset name"), FUnrealMcpBridgeDownloader::BridgeAssetName(TEXT("osx-arm64")),
				FString(TEXT("unreal-mcp-bridge-osx-arm64.zip")));
			const FString Base = TEXT("https://github.com/IvanMurzak/Unreal-MCP/releases/download");
			for (const TCHAR* Rid : { TEXT("win-x64"), TEXT("osx-arm64"), TEXT("osx-x64"), TEXT("linux-x64") })
			{
				TestEqual(Rid, FUnrealMcpBridgeDownloader::BridgeDownloadUrl(Base, TEXT("0.26.1"), Rid),
					FString::Printf(TEXT("%s/v0.26.1/unreal-mcp-bridge-%s.zip"), *Base, Rid));
			}
			TestEqual(TEXT("a trailing slash on the base is tolerated"),
				FUnrealMcpBridgeDownloader::BridgeDownloadUrl(Base + TEXT("/"), TEXT("0.26.1"), TEXT("win-x64")),
				Base + TEXT("/v0.26.1/unreal-mcp-bridge-win-x64.zip"));
			TestTrue(TEXT("no version -> empty"), FUnrealMcpBridgeDownloader::BridgeDownloadUrl(Base, TEXT(""), TEXT("win-x64")).IsEmpty());
			TestTrue(TEXT("no rid -> empty"), FUnrealMcpBridgeDownloader::BridgeDownloadUrl(Base, TEXT("0.26.1"), TEXT("")).IsEmpty());
		});

		It("defaults to the plugin's own GitHub releases, which pass the github.com-only host check", [this]()
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_BRIDGE_RELEASE_BASE_URL"), TEXT(""));
			const FString Url = FUnrealMcpBridgeDownloader::BridgeDownloadUrl(
				FUnrealMcpBridgeDownloader::ReleaseBaseUrl(), TEXT("0.26.1"), TEXT("win-x64"));
			TestEqual(TEXT("default url"), Url,
				FString(TEXT("https://github.com/IvanMurzak/Unreal-MCP/releases/download/v0.26.1/unreal-mcp-bridge-win-x64.zip")));
			TestTrue(TEXT("trusted"), FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(Url));
		});

		It("rejects every non-github.com or non-https base a release-base override could name", [this]()
		{
			for (const TCHAR* Bad : {
				TEXT("http://github.com/IvanMurzak/Unreal-MCP/releases/download/v1/x.zip"),
				TEXT("https://github.com.evil.io/x.zip"),
				TEXT("https://evil.io/github.com/x.zip"),
				TEXT("https://github.com@evil.io/x.zip"),
				TEXT("https://objects.github.com/x.zip") })
			{
				TestFalse(Bad, FUnrealMcpExtensionInstaller::IsTrustedDownloadUrl(Bad));
			}
		});

		It("composes the shipped manifest path under Source/ThirdParty/UnrealMcpBridge", [this]()
		{
			TestEqual(TEXT("manifest path"), FUnrealMcpBridgeDownloader::ManifestPath(TEXT("C:/fake/UnrealMCP")),
				FPaths::ConvertRelativePathToFull(TEXT("C:/fake/UnrealMCP/Source/ThirdParty/UnrealMcpBridge/SHA256SUMS")));
		});
	});

	Describe("VerifyBridgeZip", [this]()
	{
		const FString Manifest = FString::Printf(
			TEXT("1111111111111111111111111111111111111111111111111111111111111111  unreal-mcp-bridge-linux-x64.zip\n")
			TEXT("%s  unreal-mcp-bridge-win-x64.zip\n"), *BridgeDlSpecAbcDigest());

		It("accepts a zip whose SHA-256 matches the shipped manifest entry for its RID", [this, Manifest]()
		{
			FString Error;
			TestTrue(TEXT("verified"), FUnrealMcpBridgeDownloader::VerifyBridgeZip(Manifest, TEXT("win-x64"), BridgeDlSpecDigest("abc"), Error));
			TestTrue(TEXT("no error"), Error.IsEmpty());
		});

		It("rejects a tampered zip", [this, Manifest]()
		{
			FString Error;
			TestFalse(TEXT("tampered"), FUnrealMcpBridgeDownloader::VerifyBridgeZip(Manifest, TEXT("win-x64"), BridgeDlSpecDigest("abd"), Error));
			TestTrue(TEXT("actionable error"), Error.Contains(TEXT("rejected")) && Error.Contains(TEXT("not installed")));
		});

		It("rejects a RID the manifest has no entry for, and never cross-matches another RID's digest", [this, Manifest]()
		{
			FString Error;
			TestFalse(TEXT("missing entry"), FUnrealMcpBridgeDownloader::VerifyBridgeZip(Manifest, TEXT("osx-arm64"), BridgeDlSpecDigest("abc"), Error));
			TestFalse(TEXT("linux digest is not win's"), FUnrealMcpBridgeDownloader::VerifyBridgeZip(Manifest, TEXT("linux-x64"), BridgeDlSpecDigest("abc"), Error));
		});

		It("rejects everything against an empty or garbage manifest", [this]()
		{
			FString Error;
			TestFalse(TEXT("empty"), FUnrealMcpBridgeDownloader::VerifyBridgeZip(TEXT(""), TEXT("win-x64"), BridgeDlSpecDigest("abc"), Error));
			TestFalse(TEXT("garbage"), FUnrealMcpBridgeDownloader::VerifyBridgeZip(TEXT("not a manifest"), TEXT("win-x64"), BridgeDlSpecDigest("abc"), Error));
		});
	});

	Describe("ExtractBridgeZip", [this]()
	{
		AfterEach([]() { BridgeDlSpecCleanup(); });

		It("publishes the binary + verification marker and writes nothing outside the target dir", [this]()
		{
			const FString Basename = FUnrealMcpSidecarManager::BridgeBinaryBasename();
			const TArray<uint8> Zip = BridgeDlSpecZip({
				{ Basename, TEXT("apphost") },
				{ TEXT("../escaped.txt"), TEXT("x") },
				{ TEXT("nested/inner.txt"), TEXT("x") } });
			const FString Target = BridgeDlSpecRoot() / TEXT("cache/0.0.1/win-x64");
			FString Error;
			TestTrue(TEXT("extracted"), BridgeDlSpecExtract(Zip, Target, Error));
			TestTrue(TEXT("binary"), FPaths::FileExists(Target / Basename));
			TestTrue(TEXT("marker"), FPaths::FileExists(Target / FUnrealMcpSidecarManager::DownloadedBridgeMarkerName));
			TestFalse(TEXT("no zip-slip"), FPaths::FileExists(BridgeDlSpecRoot() / TEXT("cache/0.0.1/escaped.txt")));
			TestFalse(TEXT("no nested entry"), FPaths::DirectoryExists(Target / TEXT("nested")));
			TArray<FString> Leftovers;
			IFileManager::Get().FindFiles(Leftovers, *(BridgeDlSpecRoot() / TEXT("cache/0.0.1/*")), /*Files*/ true, /*Dirs*/ true);
			TestEqual(TEXT("only the published rid dir remains (no staging leftovers)"), Leftovers.Num(), 1);
		});

		It("keeps a concurrent editor's complete dir when it finished first", [this]()
		{
			const FString Basename = FUnrealMcpSidecarManager::BridgeBinaryBasename();
			const FString Target = BridgeDlSpecRoot() / TEXT("cache/0.0.1/win-x64");
			FString Error;
			TestTrue(TEXT("first"), BridgeDlSpecExtract(BridgeDlSpecZip({ { Basename, TEXT("first") } }), Target, Error));
			TestTrue(TEXT("second"), BridgeDlSpecExtract(BridgeDlSpecZip({ { Basename, TEXT("second") } }), Target, Error));
			FString Content;
			FFileHelper::LoadFileToString(Content, *(Target / Basename));
			TestEqual(TEXT("the first complete dir wins"), Content, FString(TEXT("first")));
		});

		It("fails, publishing nothing, when the zip lacks the bridge binary or is not a zip", [this]()
		{
			const FString Target = BridgeDlSpecRoot() / TEXT("cache/0.0.1/win-x64");
			FString Error;
			TestFalse(TEXT("no binary"), BridgeDlSpecExtract(BridgeDlSpecZip({ { TEXT("readme.txt"), TEXT("x") } }), Target, Error));
			TestFalse(TEXT("error set"), Error.IsEmpty());
			TestFalse(TEXT("not a zip"), BridgeDlSpecExtract(BridgeDlSpecBytes("abc"), Target, Error));
			TestFalse(TEXT("nothing published"), FPaths::DirectoryExists(Target));
		});
	});

	Describe("Start", [this]()
	{
		It("fails fast, without any request, when the plugin ships no checksum manifest", [this]()
		{
			TSharedRef<FUnrealMcpBridgeDownloader, ESPMode::ThreadSafe> Downloader = MakeShared<FUnrealMcpBridgeDownloader, ESPMode::ThreadSafe>();
			bool bCalled = false, bResult = true;
			FString Error;
			Downloader->Start(TEXT("0.0.1"), TEXT("win-x64"), BridgeDlSpecRoot() / TEXT("no-plugin"), BridgeDlSpecRoot() / TEXT("cache"),
				[&](bool bOk, const FString& Message) { bCalled = true; bResult = bOk; Error = Message; });
			TestTrue(TEXT("callback invoked synchronously"), bCalled);
			TestFalse(TEXT("failed"), bResult);
			TestTrue(TEXT("names the missing manifest"), Error.Contains(TEXT("SHA256SUMS")));
			TestEqual(TEXT("state"), static_cast<int32>(Downloader->GetState()), static_cast<int32>(FUnrealMcpBridgeDownloader::EState::Failed));
			TestEqual(TEXT("the UI message is the error"), Downloader->GetMessage(), Error);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
