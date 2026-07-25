// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Server/UnrealMcpServerManager.h"

/**
 * Local gamedev-mcp-server manager specs (docs/ARCHITECTURE.md §7, issue #95). Cover the pure,
 * host-deterministic helpers — RID mapping (shared with the sidecar + cli), the §6 install-dir layout
 * (one cache shared with the cli-downloaded binary), the version marker, the Custom-host → listen-port
 * parse, the Unity-parity launch-arg composition, and the Custom+http launch gate — plus the
 * UNREAL_MCP_SERVER_PATH override-wins resolution. The live HTTP download + real process spawn /
 * supervision / orphan-port kill are exercised by the operator dev-control E2E (server_smoke.py /
 * window_smoke.py), not unit-asserted here (they need a network + a real port + a GUI editor).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpServerManagerSpec, "UnrealMcp.Server",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Spec-unique helper (unity-build ODR rule): write a throwaway file that looks like a server binary so the
	// override branch's FPaths::FileExists check passes, and return its absolute path.
	static FString ServerSpecMakeFakeBinary(const FString& Leaf)
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpServerSpec"));
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, Leaf));
		FFileHelper::SaveStringToFile(TEXT("not-a-real-binary"), *Path);
		return Path;
	}

	static void ServerSpecCleanup()
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpServerSpec"));
		IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_SERVER_PATH"), TEXT(""));
	}

END_DEFINE_SPEC(FUnrealMcpServerManagerSpec)

void FUnrealMcpServerManagerSpec::Define()
{
	Describe("ResolveRid", [this]()
	{
		It("maps the host to the documented .NET RID (shared with the sidecar + cli)", [this]()
		{
			const FString Rid = FUnrealMcpServerManager::ResolveRid(/*bArm64DirExists*/ true);
#if PLATFORM_WINDOWS
			TestEqual(TEXT("Windows -> win-x64"), Rid, FString(TEXT("win-x64")));
#elif PLATFORM_LINUX
			TestEqual(TEXT("Linux -> linux-x64"), Rid, FString(TEXT("linux-x64")));
#elif PLATFORM_MAC
			TestTrue(TEXT("Mac -> an osx-* rid"), Rid == TEXT("osx-arm64") || Rid == TEXT("osx-x64"));
#endif
		});
	});

	Describe("ServerBinaryBasename", [this]()
	{
		It("is platform-correct (.exe on Windows only)", [this]()
		{
			const FString Base = FUnrealMcpServerManager::ServerBinaryBasename();
#if PLATFORM_WINDOWS
			TestEqual(TEXT("Windows basename"), Base, FString(TEXT("gamedev-mcp-server.exe")));
#else
			TestEqual(TEXT("posix basename"), Base, FString(TEXT("gamedev-mcp-server")));
#endif
		});
	});

	Describe("ServerInstallDir / ServerBinaryPathFor", [this]()
	{
		It("composes <project>/Intermediate/UnrealMCP/server/<rid> (the §6 cache shared with the cli)", [this]()
		{
			const FString Project = TEXT("C:/fake/project");
			const FString Dir = FUnrealMcpServerManager::ServerInstallDir(Project, TEXT("win-x64"));
			const FString Expected = FPaths::ConvertRelativePathToFull(
				FString(Project) / TEXT("Intermediate") / TEXT("UnrealMCP") / TEXT("server") / TEXT("win-x64"));
			TestEqual(TEXT("install dir matches the §6 layout"), Dir, Expected);

			const FString Bin = FUnrealMcpServerManager::ServerBinaryPathFor(Project, TEXT("win-x64"));
			TestEqual(TEXT("binary path = installdir/basename"), Bin, Dir / FUnrealMcpServerManager::ServerBinaryBasename());
		});

		It("returns empty for an empty project dir or rid", [this]()
		{
			TestTrue(TEXT("empty project -> empty"), FUnrealMcpServerManager::ServerInstallDir(FString(), TEXT("win-x64")).IsEmpty());
			TestTrue(TEXT("empty rid -> empty"), FUnrealMcpServerManager::ServerInstallDir(TEXT("C:/p"), FString()).IsEmpty());
		});
	});

	Describe("ReadVersionMarker", [this]()
	{
		It("reads a trimmed version marker and returns empty when absent", [this]()
		{
			const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpServerSpec"));
			IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
			TestTrue(TEXT("missing marker -> empty"), FUnrealMcpServerManager::ReadVersionMarker(Dir).IsEmpty());
			FFileHelper::SaveStringToFile(TEXT("8.0.0\n"), *(Dir / TEXT("version")));
			TestEqual(TEXT("marker is trimmed"), FUnrealMcpServerManager::ReadVersionMarker(Dir), FString(TEXT("8.0.0")));
			ServerSpecCleanup();
		});
	});

	Describe("ParsePortFromHost", [this]()
	{
		It("extracts an explicit port from the Custom host URL", [this]()
		{
			TestEqual(TEXT("http with port"), FUnrealMcpServerManager::ParsePortFromHost(TEXT("http://localhost:8080")), 8080);
			TestEqual(TEXT("https with port"), FUnrealMcpServerManager::ParsePortFromHost(TEXT("https://127.0.0.1:5244")), 5244);
			TestEqual(TEXT("trailing path ignored"), FUnrealMcpServerManager::ParsePortFromHost(TEXT("http://localhost:9001/mcp")), 9001);
			TestEqual(TEXT("bare host:port (no scheme)"), FUnrealMcpServerManager::ParsePortFromHost(TEXT("localhost:7777")), 7777);
		});

		It("infers the scheme default port when none is given", [this]()
		{
			TestEqual(TEXT("http -> 80"), FUnrealMcpServerManager::ParsePortFromHost(TEXT("http://example.com")), 80);
			TestEqual(TEXT("https -> 443"), FUnrealMcpServerManager::ParsePortFromHost(TEXT("https://example.com")), 443);
		});

		It("falls back to the supplied default for empty / portless-schemeless input", [this]()
		{
			TestEqual(TEXT("empty -> default"), FUnrealMcpServerManager::ParsePortFromHost(TEXT(""), 8080), 8080);
			TestEqual(TEXT("schemeless host -> default"), FUnrealMcpServerManager::ParsePortFromHost(TEXT("just-a-host"), 1234), 1234);
		});
	});

	// NOTE (mcp-authorize g5/g6 consolidation): the launch-arg composition no longer lives in C++. The former
	// FUnrealMcpServerManager::BuildLaunchArgs was DELETED — FUnrealMcpServerManager::Start now takes a pre-composed
	// arg string, and the sidecar's SHARED com.IvanMurzak.McpPlugin.ServerLaunch.ServerLaunchArguments builder
	// (none/oauth/token) composes it (covered by the bridge xUnit SidecarHostServerLaunchArgsTests). There is no C++
	// arg-building logic left to spec here.

	Describe("IsLaunchAllowed", [this]()
	{
		It("permits ONLY Custom mode + http transport", [this]()
		{
			TestTrue(TEXT("Custom+http -> allowed"), FUnrealMcpServerManager::IsLaunchAllowed(/*custom*/ true, /*http*/ true));
			TestFalse(TEXT("Custom+stdio -> denied"), FUnrealMcpServerManager::IsLaunchAllowed(true, false));
			TestFalse(TEXT("Cloud+http -> denied"), FUnrealMcpServerManager::IsLaunchAllowed(false, true));
			TestFalse(TEXT("Cloud+stdio -> denied"), FUnrealMcpServerManager::IsLaunchAllowed(false, false));
		});
	});

	Describe("ServerVersion", [this]()
	{
		It("is pinned to the lockstep value (kept in sync with cli/src/lib/server-version.ts)", [this]()
		{
			TestEqual(TEXT("pinned server version"), FString(FUnrealMcpServerManager::ServerVersion), FString(TEXT("9.2.2")));
		});
	});

	Describe("ResolveBinaryPath", [this]()
	{
		It("returns the UNREAL_MCP_SERVER_PATH override when it points at an existing file", [this]()
		{
			const FString Fake = ServerSpecMakeFakeBinary(TEXT("override-server.bin"));
			FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_SERVER_PATH"), *Fake);
			FUnrealMcpServerManager Manager;
			TestEqual(TEXT("override wins over cache/download"), Manager.ResolveBinaryPath(), Fake);
			ServerSpecCleanup();
		});

		It("ignores a non-existent override and returns empty when no cached binary exists", [this]()
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("UNREAL_MCP_SERVER_PATH"), TEXT("Z:/does/not/exist/gamedev-mcp-server.bin"));
			FUnrealMcpServerManager Manager;
			// A clean test project has no cached server, so a bogus override degrades to empty (caller downloads).
			TestTrue(TEXT("bogus override + no cache -> empty"), Manager.ResolveBinaryPath().IsEmpty());
			ServerSpecCleanup();
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
