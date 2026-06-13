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
			const FString Resolved = FUnrealMcpSidecarManager::ResolveBridgeBinaryPath();
			TestTrue(TEXT("bogus override + no bundle -> empty"), Resolved.IsEmpty());
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
