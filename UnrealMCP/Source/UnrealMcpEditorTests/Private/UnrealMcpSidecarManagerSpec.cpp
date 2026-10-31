// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Sidecar/UnrealMcpSidecarManager.h"
#include "Interfaces/IPluginManager.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

/**
 * Sidecar binary-resolution specs (docs/ARCHITECTURE.md §6 BUNDLE model). Cover the pure, host-
 * deterministic resolver helpers — RID mapping, basename, bundled-path composition — plus the
 * UNREAL_MCP_BRIDGE_PATH override-wins behavior and the graceful-degrade (empty) path. The full
 * IPluginManager-backed bundled resolution and the macOS/Linux spawn-prep syscalls are exercised by
 * the live e2e / packaging dry-run (no staged binary exists in a dev checkout), not unit-asserted here.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpSidecarManagerSpec, "UnrealMcp.Sidecar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Spec-unique helper (unity-build ODR rule): write a throwaway file that looks like a bridge binary
	// so the override branch's FPaths::FileExists check passes, and return its absolute path.
	static FString SidecarSpecMakeFakeBinary(const FString& Leaf)
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpSidecarSpec"));
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, Leaf));
		FFileHelper::SaveStringToFile(TEXT("not-a-real-binary"), *Path);
		return Path;
	}

	static void SidecarSpecCleanup()
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpSidecarSpec"));
		IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_BRIDGE_PATH"), TEXT(""));
		FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_BRIDGE_CACHE_DIR"), TEXT(""));
	}

	// Point the §6.8 download cache at an empty spec dir so a real per-user cache on this machine cannot leak in.
	static FString SidecarSpecUseEmptyCache()
	{
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpSidecarSpec"), TEXT("cache")));
		FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_BRIDGE_CACHE_DIR"), *Root);
		return Root;
	}

END_DEFINE_SPEC(FUnrealMcpSidecarManagerSpec)

void FUnrealMcpSidecarManagerSpec::Define()
{
	Describe("ResolveRid", [this]()
	{
		It("maps the host platform to the documented .NET RID directory (§6.2)", [this]()
		{
			const FString Rid = FUnrealMcpSidecarManager::ResolveRid(/*bArm64DirExists*/ true);
#if PLATFORM_WINDOWS
			TestEqual(TEXT("Windows host -> win-x64"), Rid, FString(TEXT("win-x64")));
#elif PLATFORM_LINUX
			TestEqual(TEXT("Linux host -> linux-x64"), Rid, FString(TEXT("linux-x64")));
#elif PLATFORM_MAC
			// Apple Silicon -> osx-arm64, Intel -> osx-x64; either is a valid macOS RID.
			TestTrue(TEXT("Mac host -> an osx-* rid"), Rid == TEXT("osx-arm64") || Rid == TEXT("osx-x64"));
#endif
		});

		It("falls back to osx-x64 when the arm64 slice is absent (§6.2 defensive)", [this]()
		{
			const FString Rid = FUnrealMcpSidecarManager::ResolveRid(/*bArm64DirExists*/ false);
#if PLATFORM_MAC
			TestEqual(TEXT("arm64 dir missing -> osx-x64 even on Apple Silicon"), Rid, FString(TEXT("osx-x64")));
#elif PLATFORM_WINDOWS
			TestEqual(TEXT("non-mac unaffected by the arm64 flag"), Rid, FString(TEXT("win-x64")));
#elif PLATFORM_LINUX
			TestEqual(TEXT("non-mac unaffected by the arm64 flag"), Rid, FString(TEXT("linux-x64")));
#endif
		});
	});

	Describe("ShouldAutoStart", [this]()
	{
		It("starts the sidecar in the editor, never in a commandlet (a packaging cook)", [this]()
		{
			TestTrue(TEXT("editor"), FUnrealMcpSidecarManager::ShouldAutoStart(/*bIsCommandlet*/ false));
			TestFalse(TEXT("commandlet"), FUnrealMcpSidecarManager::ShouldAutoStart(/*bIsCommandlet*/ true));
		});
	});

	Describe("BridgeBinaryBasename", [this]()
	{
		It("is platform-correct (.exe on Windows only)", [this]()
		{
			const FString Base = FUnrealMcpSidecarManager::BridgeBinaryBasename();
#if PLATFORM_WINDOWS
			TestEqual(TEXT("Windows basename"), Base, FString(TEXT("unreal-mcp-bridge.exe")));
#else
			TestEqual(TEXT("posix basename"), Base, FString(TEXT("unreal-mcp-bridge")));
#endif
		});
	});

	Describe("ComposeBundledBridgePath", [this]()
	{
		It("composes <base>/Binaries/ThirdParty/UnrealMcpBridge/<rid>/<basename> (§6.1)", [this]()
		{
			const FString Base = TEXT("C:/fake/plugin/UnrealMCP");
			const FString Path = FUnrealMcpSidecarManager::ComposeBundledBridgePath(Base);
			const FString Expected = FPaths::ConvertRelativePathToFull(
				FString(Base) / TEXT("Binaries") / TEXT("ThirdParty") / TEXT("UnrealMcpBridge")
				/ FUnrealMcpSidecarManager::ResolveRid() / FUnrealMcpSidecarManager::BridgeBinaryBasename());
			TestEqual(TEXT("composed path matches the §6.1 layout"), Path, Expected);
			TestTrue(TEXT("path is under the ThirdParty bundle dir"),
				Path.Contains(TEXT("Binaries/ThirdParty/UnrealMcpBridge")) || Path.Contains(TEXT("Binaries\\ThirdParty\\UnrealMcpBridge")));
		});

		It("returns empty for an empty plugin base dir", [this]()
		{
			TestTrue(TEXT("empty base -> empty path"), FUnrealMcpSidecarManager::ComposeBundledBridgePath(FString()).IsEmpty());
		});
	});

	Describe("ComposeSurvivingBridgePath (#139/#187 Fab-surviving source)", [this]()
	{
		It("composes <base>/Source/ThirdParty/UnrealMcpBridge/<rid>/<basename> — the engine-canonical Fab-surviving layout", [this]()
		{
			const FString Base = TEXT("C:/fake/plugin/UnrealMCP");
			const FString Path = FUnrealMcpSidecarManager::ComposeSurvivingBridgePath(Base);
			const FString Expected = FPaths::ConvertRelativePathToFull(
				FString(Base) / TEXT("Source") / TEXT("ThirdParty") / TEXT("UnrealMcpBridge")
				/ FUnrealMcpSidecarManager::ResolveRid() / FUnrealMcpSidecarManager::BridgeBinaryBasename());
			TestEqual(TEXT("composed surviving path matches the #187 Source/ThirdParty/UnrealMcpBridge/<rid>/ layout"), Path, Expected);
			TestTrue(TEXT("surviving path is under Source/ThirdParty/UnrealMcpBridge, NOT Binaries/"),
				(Path.Contains(TEXT("/Source/ThirdParty/UnrealMcpBridge/")) || Path.Contains(TEXT("\\Source\\ThirdParty\\UnrealMcpBridge\\")))
				&& !Path.Contains(TEXT("Binaries")));
		});

		It("returns empty for an empty plugin base dir", [this]()
		{
			TestTrue(TEXT("empty base -> empty path"), FUnrealMcpSidecarManager::ComposeSurvivingBridgePath(FString()).IsEmpty());
		});
	});

	Describe("ComposeBundledBridgeCandidates (#139/#187 resolution order)", [this]()
	{
		It("lists the STAGED Binaries/ThirdParty path first, then the FAB-SURVIVING Source/ThirdParty source", [this]()
		{
			const FString Base = TEXT("C:/fake/plugin/UnrealMCP");
			const TArray<FString> Candidates = FUnrealMcpSidecarManager::ComposeBundledBridgeCandidates(Base);
			TestEqual(TEXT("two candidates on a desktop host"), Candidates.Num(), 2);
			TestEqual(TEXT("first candidate is the staged Binaries/ThirdParty path"),
				Candidates[0], FUnrealMcpSidecarManager::ComposeBundledBridgePath(Base));
			TestEqual(TEXT("second candidate is the Fab-surviving Source/ThirdParty source"),
				Candidates[1], FUnrealMcpSidecarManager::ComposeSurvivingBridgePath(Base));
		});

		It("returns an empty list for an empty plugin base dir", [this]()
		{
			TestEqual(TEXT("empty base -> no candidates"),
				FUnrealMcpSidecarManager::ComposeBundledBridgeCandidates(FString()).Num(), 0);
		});
	});

	Describe("ResolveBridgeBinaryPath", [this]()
	{
		It("returns the UNREAL_MCP_BRIDGE_PATH override when it points at an existing file (§6.3 step 1)", [this]()
		{
			const FString Fake = SidecarSpecMakeFakeBinary(TEXT("override-bridge.bin"));
			FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_BRIDGE_PATH"), *Fake);
			const FString Resolved = FUnrealMcpSidecarManager::ResolveBridgeBinaryPath();
			TestEqual(TEXT("override wins over the bundled path"), Resolved, Fake);
			SidecarSpecCleanup();
		});

		It("ignores a non-existent override and degrades when no bundled binary exists (§6.3 step 3)", [this]()
		{
			// A dev checkout never has a staged bundled binary, so with a bogus override the resolver
			// must return empty rather than a phantom path — this is the graceful-degrade precondition.
			FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_BRIDGE_PATH"), TEXT("Z:/does/not/exist/unreal-mcp-bridge.bin"));
			SidecarSpecUseEmptyCache();
			const FString Resolved = FUnrealMcpSidecarManager::ResolveBridgeBinaryPath();
			TestTrue(TEXT("bogus override + no bundle -> empty"), Resolved.IsEmpty());
			SidecarSpecCleanup();
		});
	});

	Describe("PickBridgeBinaryPath (6.3 precedence)", [this]()
	{
		const FString Override = TEXT("C:/o/bridge.exe");
		const TArray<FString> Bundled = { TEXT("C:/p/Binaries/bridge.exe"), TEXT("C:/p/Source/bridge.exe") };
		const FString Downloaded = TEXT("C:/cache/0.26.1/win-x64/bridge.exe");
		auto Pick = [Override, Bundled, Downloaded](TSet<FString> Present)
		{
			return FUnrealMcpSidecarManager::PickBridgeBinaryPath(Override, Bundled, Downloaded,
				[&Present](const FString& Path) { return Present.Contains(Path); });
		};

		It("prefers the override, then the staged bundle, then the surviving bundle, then the download cache", [this, Pick, Override, Bundled, Downloaded]()
		{
			TestEqual(TEXT("override wins"), Pick({ Override, Bundled[0], Bundled[1], Downloaded }), Override);
			TestEqual(TEXT("bundled beats the download (existing installs unchanged)"), Pick({ Bundled[0], Bundled[1], Downloaded }), Bundled[0]);
			TestEqual(TEXT("surviving bundle beats the download"), Pick({ Bundled[1], Downloaded }), Bundled[1]);
			TestEqual(TEXT("download cache last"), Pick({ Downloaded }), Downloaded);
			TestTrue(TEXT("nothing present -> empty"), Pick({}).IsEmpty());
		});

		It("never matches empty entries", [this]()
		{
			TestTrue(TEXT("empty inputs"), FUnrealMcpSidecarManager::PickBridgeBinaryPath(FString(), { FString() }, FString(),
				[](const FString&) { return true; }).IsEmpty());
		});
	});

	Describe("Download cache (6.8)", [this]()
	{
		It("composes <root>/<version>/<rid> and nothing for a missing input", [this]()
		{
			TestEqual(TEXT("dir"), FUnrealMcpSidecarManager::ComposeDownloadedBridgeDir(TEXT("C:/cache"), TEXT("0.26.1"), TEXT("win-x64")),
				FPaths::ConvertRelativePathToFull(TEXT("C:/cache/0.26.1/win-x64")));
			TestTrue(TEXT("no version"), FUnrealMcpSidecarManager::ComposeDownloadedBridgeDir(TEXT("C:/cache"), TEXT(""), TEXT("win-x64")).IsEmpty());
		});

		It("trusts a cache dir only when its marker records the digest the plugin ships", [this]()
		{
			const FString Root = SidecarSpecUseEmptyCache();
			TestEqual(TEXT("cache root honours UNREAL_MCP_BRIDGE_CACHE_DIR"), FUnrealMcpSidecarManager::BridgeCacheRoot(), Root);
			const FString Dir = FUnrealMcpSidecarManager::ComposeDownloadedBridgeDir(Root, TEXT("0.0.1"), TEXT("win-x64"));
			const FString Digest = TEXT("0a936975cbf07c285c5e10e8ab444547b55c6f3f404b51daa6890c2b96e034c1");
			FFileHelper::SaveStringToFile(TEXT("apphost"), *(Dir / FUnrealMcpSidecarManager::BridgeBinaryBasename()));
			TestFalse(TEXT("no marker"), FUnrealMcpSidecarManager::IsCompleteDownloadedBridgeDir(Dir, Digest));
			FFileHelper::SaveStringToFile(Digest.ToUpper(), *(Dir / FUnrealMcpSidecarManager::DownloadedBridgeMarkerName));
			TestTrue(TEXT("marker == shipped digest (case-insensitive)"), FUnrealMcpSidecarManager::IsCompleteDownloadedBridgeDir(Dir, Digest));
			TestFalse(TEXT("another build's digest"), FUnrealMcpSidecarManager::IsCompleteDownloadedBridgeDir(Dir, FString::ChrN(64, TEXT('1'))));
			TestFalse(TEXT("no shipped digest (no manifest) never matches"), FUnrealMcpSidecarManager::IsCompleteDownloadedBridgeDir(Dir, FString()));
			SidecarSpecCleanup();
		});

		It("reads the shipped digest per RID from the SHA256SUMS text, exact name only", [this]()
		{
			const FString Manifest = FString(TEXT("1111111111111111111111111111111111111111111111111111111111111111  unreal-mcp-bridge-linux-x64.zip\r\n"))
				+ TEXT("0A936975CBF07C285C5E10E8AB444547B55C6F3F404B51DAA6890C2B96E034C1 *unreal-mcp-bridge-win-x64.zip\n");
			TestEqual(TEXT("win (binary-mode marker, upper-case)"), FUnrealMcpSidecarManager::ShippedBridgeZipDigest(Manifest, TEXT("win-x64")),
				FString(TEXT("0a936975cbf07c285c5e10e8ab444547b55c6f3f404b51daa6890c2b96e034c1")));
			TestEqual(TEXT("linux"), FUnrealMcpSidecarManager::ShippedBridgeZipDigest(Manifest, TEXT("linux-x64")), FString::ChrN(64, TEXT('1')));
			TestTrue(TEXT("absent rid"), FUnrealMcpSidecarManager::ShippedBridgeZipDigest(Manifest, TEXT("osx-arm64")).IsEmpty());
			TestTrue(TEXT("no prefix match"), FUnrealMcpSidecarManager::ShippedBridgeZipDigest(Manifest, TEXT("win")).IsEmpty());
		});

		It("never resolves the cache for a plugin that ships no SHA256SUMS (a dev checkout)", [this]()
		{
			TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
			if (!Plugin.IsValid() || FPaths::FileExists(FUnrealMcpSidecarManager::BridgeManifestPath(Plugin->GetBaseDir())))
				return; // a release-installed plugin ships the manifest; this case is about dev checkouts
			const FString Root = SidecarSpecUseEmptyCache();
			const FString Dir = FUnrealMcpSidecarManager::ComposeDownloadedBridgeDir(Root,
				FUnrealMcpSidecarManager::PluginVersionName(), FUnrealMcpSidecarManager::ResolveDownloadRid());
			FFileHelper::SaveStringToFile(TEXT("apphost"), *(Dir / FUnrealMcpSidecarManager::BridgeBinaryBasename()));
			FFileHelper::SaveStringToFile(TEXT("anything"), *(Dir / FUnrealMcpSidecarManager::DownloadedBridgeMarkerName));
			TestTrue(TEXT("not resolved"), FUnrealMcpSidecarManager::ResolveDownloadedBridgePath().IsEmpty());
			SidecarSpecCleanup();
		});
	});

	Describe("PrepareBundledBinaryForSpawn", [this]()
	{
		It("is tolerant of a real file on every platform (no-op on Windows, +x/xattr on posix)", [this]()
		{
			const FString Fake = SidecarSpecMakeFakeBinary(TEXT("prep-bridge.bin"));
			const bool bOk = FUnrealMcpSidecarManager::PrepareBundledBinaryForSpawn(Fake);
			TestTrue(TEXT("prep on an existing file does not report a fatal error"), bOk);
#if PLATFORM_MAC || PLATFORM_LINUX
			// §6.6 load-bearing behavior: the apphost must end up executable (chmod 0755). Verify the
			// mode bits, not just the non-fatal return — a missing +x would make the real spawn fail.
			const auto Utf8 = StringCast<ANSICHAR>(*Fake);
			struct stat St;
			if (TestTrue(TEXT("stat the prepped fixture"), stat(Utf8.Get(), &St) == 0))
			{
				TestEqual(TEXT("prepped file is mode 0755 (rwxr-xr-x)"),
					static_cast<int32>(St.st_mode & 0777), static_cast<int32>(0755));
			}
#endif
			SidecarSpecCleanup();
		});

		It("returns false for an empty path", [this]()
		{
			TestFalse(TEXT("empty path -> false"), FUnrealMcpSidecarManager::PrepareBundledBinaryForSpawn(FString()));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
