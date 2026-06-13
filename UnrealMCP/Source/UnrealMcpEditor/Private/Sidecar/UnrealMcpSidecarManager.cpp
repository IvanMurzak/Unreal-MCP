// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpSidecarManager.h"
#include "UnrealMcpLog.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"

#include <random>

#if PLATFORM_MAC || PLATFORM_LINUX
#include <cerrno>
#include <sys/stat.h>
#endif
#if PLATFORM_MAC
#include <sys/xattr.h>
#include <sys/sysctl.h>
#endif

namespace
{
	// §1.5 restart backoff schedule (seconds).
	const int32 BackoffSeconds[] = { 1, 2, 5, 10, 30 };
	constexpr int32 MaxCrashesPerWindow = 5;
	constexpr double CrashWindowSeconds = 300.0;
}

FUnrealMcpSidecarManager::~FUnrealMcpSidecarManager()
{
	Stop();
}

FString FUnrealMcpSidecarManager::BridgeBinaryBasename()
{
#if PLATFORM_WINDOWS
	return TEXT("unreal-mcp-bridge.exe");
#else
	return TEXT("unreal-mcp-bridge");
#endif
}

FString FUnrealMcpSidecarManager::ResolveRid(bool bArm64DirExists)
{
#if PLATFORM_WINDOWS
	(void)bArm64DirExists;
	return TEXT("win-x64");
#elif PLATFORM_MAC
	// §6.2: select from the PHYSICAL host CPU, not the (possibly Rosetta-translated) editor arch — the
	// bridge is a separate child and should match the hardware. `hw.optional.arm64 == 1` on Apple
	// Silicon reports the hardware even under a translated process (verified per design R5). Fall back to
	// osx-x64 (runs under Rosetta 2) when the arm64 slice is absent, so a missing build is never fatal.
	int32 IsArm64 = 0;
	size_t Size = sizeof(IsArm64);
	if (sysctlbyname("hw.optional.arm64", &IsArm64, &Size, nullptr, 0) != 0)
		IsArm64 = 0; // probe unavailable → treat as Intel
	if (IsArm64 == 1 && bArm64DirExists)
		return TEXT("osx-arm64");
	return TEXT("osx-x64");
#elif PLATFORM_LINUX
	(void)bArm64DirExists;
	return TEXT("linux-x64");
#else
	(void)bArm64DirExists;
	return FString();
#endif
}

FString FUnrealMcpSidecarManager::ComposeBundledBridgePath(const FString& PluginBaseDir, bool bArm64DirExists)
{
	if (PluginBaseDir.IsEmpty())
		return FString();
	const FString Rid = ResolveRid(bArm64DirExists);
	if (Rid.IsEmpty())
		return FString();
	const FString Path = PluginBaseDir
		/ TEXT("Binaries") / TEXT("ThirdParty") / TEXT("UnrealMcpBridge") / Rid / BridgeBinaryBasename();
	return FPaths::ConvertRelativePathToFull(Path);
}

bool FUnrealMcpSidecarManager::BundledArm64SliceExists()
{
	// On Apple Silicon, prefer osx-arm64 only when that dir is actually present (defensive — a build that
	// shipped only osx-x64 must not resolve to a missing arm64 path). On non-macOS hosts the arm64 flag is
	// irrelevant to ResolveRid, so report true (the cheap default).
#if PLATFORM_MAC
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
	if (!Plugin.IsValid())
		return false;
	const FString Arm64Candidate = Plugin->GetBaseDir()
		/ TEXT("Binaries") / TEXT("ThirdParty") / TEXT("UnrealMcpBridge") / TEXT("osx-arm64") / BridgeBinaryBasename();
	return FPaths::FileExists(Arm64Candidate);
#else
	return true;
#endif
}

FString FUnrealMcpSidecarManager::ResolveEffectiveRid()
{
	return ResolveRid(BundledArm64SliceExists());
}

FString FUnrealMcpSidecarManager::ResolveBridgeBinaryPath()
{
	// §6.3 step 1 — dev/CI override: UNREAL_MCP_BRIDGE_PATH wins when set and present (bridge inner
	// dev loop + live e2e). The bundled binary is NEVER preferred over an explicit override.
	const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("UNREAL_MCP_BRIDGE_PATH"));
	if (!Override.IsEmpty() && FPaths::FileExists(Override))
		return Override;

	// §6.3 step 2 — bundled path inside the plugin (the production path for every end user). The
	// self-contained binary ships under Binaries/ThirdParty/UnrealMcpBridge/<rid>/ (§6.1), staged into
	// the packaged plugin by the release job (T4); a dev source checkout has none, so this returns
	// empty and the override above is the dev path (unchanged behavior on a fresh source tree).
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
	if (!Plugin.IsValid())
		return FString(); // §6.3 step 3 — neither resolves; caller logs the actionable error.

	// Share the single §6.1 layout composition with the unit-tested pure helper (no inline drift); pass the
	// arm64-probe result so a host with only the osx-x64 slice degrades to that rid.
	const FString Path = ComposeBundledBridgePath(Plugin->GetBaseDir(), BundledArm64SliceExists());
	if (Path.IsEmpty())
		return FString();
	return FPaths::FileExists(Path) ? Path : FString();
}

bool FUnrealMcpSidecarManager::PrepareBundledBinaryForSpawn(const FString& Path)
{
	if (Path.IsEmpty())
		return false;

#if PLATFORM_MAC || PLATFORM_LINUX
	// §6.6: ensure the apphost is executable. UE has no portable chmod; call the syscall directly (no
	// shell, no PATH dependency). 0755 = rwxr-xr-x.
	const auto Utf8Path = StringCast<ANSICHAR>(*Path);
	if (chmod(Utf8Path.Get(), 0755) != 0)
	{
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] could not set +x on the bundled sidecar '%s' (errno=%d); spawn may fail."),
			*Path, errno);
		// Not fatal — the bit may already be set by the packager; let the spawn attempt surface a real failure.
	}
#endif
#if PLATFORM_MAC
	// §6.6 / design R3: strip com.apple.quarantine so Gatekeeper's first-exec check is bypassed even
	// offline (a binary with no quarantine xattr is not Gatekeeper-checked on exec). Belt-and-suspenders
	// alongside notarization (T3). ENOATTR (no such attribute) is the expected happy case for an
	// already-clean or never-quarantined binary — tolerate it.
	if (removexattr(Utf8Path.Get(), "com.apple.quarantine", 0) != 0 && errno != ENOATTR)
	{
		UE_LOG(LogUnrealMcp, Verbose,
			TEXT("[Unreal-MCP] could not strip com.apple.quarantine from '%s' (errno=%d); relying on notarization."),
			*Path, errno);
	}
#endif
	(void)Path;
	return true;
}

FString FUnrealMcpSidecarManager::GenerateToken()
{
	// §1.4: 32 cryptographically-random bytes. std::random_device is backed by the OS CSPRNG on the
	// supported targets (RtlGenRandom on Windows-MSVC, /dev/urandom on Unix), satisfying the §1.4 intent
	// without a platform-specific bcrypt link in the MVP. (Deviation noted in design_notes: the design
	// names BCryptGenRandom/dev-urandom explicitly; std::random_device delegates to exactly those.)
	std::random_device Rd;
	uint8 Bytes[32];
	for (int32 i = 0; i < 32; i += 4)
	{
		const uint32 R = Rd();
		Bytes[i + 0] = static_cast<uint8>(R & 0xFF);
		Bytes[i + 1] = static_cast<uint8>((R >> 8) & 0xFF);
		Bytes[i + 2] = static_cast<uint8>((R >> 16) & 0xFF);
		Bytes[i + 3] = static_cast<uint8>((R >> 24) & 0xFF);
	}
	return BytesToHex(Bytes, 32).ToLower();
}

bool FUnrealMcpSidecarManager::StartForPort(int32 InIpcPort, const FString& InToken)
{
	IpcPort = InIpcPort;
	Token = InToken;
	bStopRequested = false;

	BridgePath = ResolveBridgeBinaryPath();
	if (BridgePath.IsEmpty())
	{
		// §6.3 step 3 graceful-degrade: a packaged plugin missing the <rid> bridge, OR a dev source
		// checkout with no (or a stale) UNREAL_MCP_BRIDGE_PATH. The bridge server still listens; surface
		// an actionable error (the bundled binary is the production path, the env var is the dev path).
		// Report the EFFECTIVE rid (arm64-probed, matching ResolveBridgeBinaryPath) so the reinstall hint
		// names the slice that was actually looked for — not osx-arm64 on a host that degraded to osx-x64.
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] no sidecar binary resolved for rid '%s' (bundled path absent and UNREAL_MCP_BRIDGE_PATH unset or pointing at a missing file). ")
			TEXT("The bridge is listening on %d. End users: reinstall the plugin for your platform; developers: set UNREAL_MCP_BRIDGE_PATH or run `unreal-mcp-cli bootstrap-local`."),
			*ResolveEffectiveRid(), IpcPort);
		return false;
	}

	// §6.6 pre-spawn prep (macOS/Linux): +x and (macOS) quarantine strip on the resolved bundled binary.
	// The dev override is assumed already-runnable (the dev built it), so prep only the bundled path.
	const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("UNREAL_MCP_BRIDGE_PATH"));
	const bool bUsingOverride = !Override.IsEmpty() && FPaths::FileExists(Override)
		&& (BridgePath == Override || BridgePath == FPaths::ConvertRelativePathToFull(Override));
	if (!bUsingOverride)
		PrepareBundledBinaryForSpawn(BridgePath);

	if (!SpawnProcess())
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] failed to spawn the sidecar from '%s'."), *BridgePath);
		return false;
	}

	WindowStartSeconds = FPlatformTime::Seconds();
	CrashesInWindow = 0;
	StartWatchdog();
	return true;
}

bool FUnrealMcpSidecarManager::SpawnProcess()
{
	// §1.4: the token is delivered over the child's STDIN, never argv. argv carries only non-secrets.
	void* ReadPipe = nullptr;   // child's stdin (inherited)
	void* WritePipe = nullptr;  // parent writes the token here
	if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe, /*bWritePipeLocal*/ true))
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] could not create the sidecar stdin pipe."));
		return false;
	}

	const uint32 EditorPid = FPlatformProcess::GetCurrentProcessId();
	const FString Parms = FString::Printf(TEXT("--ipc-port=%d --parent-pid=%u"), IpcPort, EditorPid);

	ProcHandle = FPlatformProcess::CreateProc(
		*BridgePath,
		*Parms,
		/*bLaunchDetached*/ false,
		/*bLaunchHidden*/ true,
		/*bLaunchReallyHidden*/ true,
		&ProcId,
		/*PriorityModifier*/ 0,
		/*OptionalWorkingDirectory*/ nullptr,
		/*PipeWriteChild (child stdout)*/ nullptr,
		/*PipeReadChild  (child stdin) */ ReadPipe);

	if (!ProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		return false;
	}

	// Write exactly one token line; the sidecar reads one line from stdin before dialing (§1.4).
	// FPlatformProcess::WritePipe(const FString&) appends its own trailing '\n', so we pass the bare token
	// (adding another would send a spurious blank second line).
	FPlatformProcess::WritePipe(WritePipe, Token);

	// Close the parent's pipe copies. The child keeps its inherited stdin handle; closing the write end
	// sends EOF after the single token line (the sidecar only reads one line).
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] spawned sidecar pid=%u for ipc-port=%d."), ProcId, IpcPort);
	return true;
}

void FUnrealMcpSidecarManager::StartWatchdog()
{
	if (WatchdogFuture.IsValid())
		return;

	WatchdogFuture = Async(EAsyncExecution::Thread, [this]()
	{
		int32 BackoffIndex = 0;
		while (!bStopRequested)
		{
			FPlatformProcess::Sleep(1.0f);
			if (bStopRequested)
				break;

			if (ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcHandle))
			{
				BackoffIndex = 0; // healthy
				continue;
			}

			// The process died unexpectedly (§1.5 auto-restart).
			const double Now = FPlatformTime::Seconds();
			if (Now - WindowStartSeconds > CrashWindowSeconds)
			{
				WindowStartSeconds = Now;
				CrashesInWindow = 0;
			}
			if (++CrashesInWindow > MaxCrashesPerWindow)
			{
				UE_LOG(LogUnrealMcp, Error,
					TEXT("[Unreal-MCP] sidecar crashed > %d times in %.0fs; giving up auto-restart."),
					MaxCrashesPerWindow, CrashWindowSeconds);
				return;
			}

			const int32 DelaySeconds = BackoffSeconds[FMath::Min(BackoffIndex, (int32)UE_ARRAY_COUNT(BackoffSeconds) - 1)];
			++BackoffIndex;
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] sidecar exited; restarting in %ds (restart #%d)."),
				DelaySeconds, RestartCount.GetValue() + 1);

			// Chunk the backoff on a short tick (not one uninterruptible Sleep of up to 30 s): StopWatchdog()
			// joins this thread on the game thread at editor shutdown, so a single long sleep would stall the
			// editor's quit for the remainder of the backoff. Poll bStopRequested like the bridge heartbeat.
			const double BackoffUntil = FPlatformTime::Seconds() + DelaySeconds;
			while (!bStopRequested && FPlatformTime::Seconds() < BackoffUntil)
				FPlatformProcess::Sleep(0.5f);
			if (bStopRequested)
				break;

			if (ProcHandle.IsValid())
			{
				FPlatformProcess::CloseProc(ProcHandle);
				ProcHandle.Reset();
			}
			// Fresh token each relaunch (§1.4 — token rotates every relaunch). The bridge server reads
			// the new token on the next handshake; the server's Start() token is the authority, so a
			// relaunched sidecar with a stale token would be rejected. NOTE: rotating the server-side
			// token on relaunch lands with the full lifecycle wiring; the MVP keeps the launch token.
			if (SpawnProcess())
				RestartCount.Increment();
		}
	});
}

void FUnrealMcpSidecarManager::StopWatchdog()
{
	bStopRequested = true;
	if (WatchdogFuture.IsValid())
	{
		WatchdogFuture.Wait();
		WatchdogFuture = TFuture<void>();
	}
}

void FUnrealMcpSidecarManager::TerminateProcess()
{
	if (ProcHandle.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(ProcHandle))
			FPlatformProcess::TerminateProc(ProcHandle, /*KillTree*/ true);
		FPlatformProcess::CloseProc(ProcHandle);
		ProcHandle.Reset();
	}
}

bool FUnrealMcpSidecarManager::IsRunning() const
{
	return ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(const_cast<FProcHandle&>(ProcHandle));
}

void FUnrealMcpSidecarManager::StopRestarts()
{
	StopWatchdog();
}

bool FUnrealMcpSidecarManager::WaitForExit(FTimespan Grace)
{
	const double Deadline = FPlatformTime::Seconds() + Grace.GetTotalSeconds();
	while (ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcHandle) && FPlatformTime::Seconds() < Deadline)
		FPlatformProcess::Sleep(0.05f);
	return !(ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcHandle));
}

void FUnrealMcpSidecarManager::Stop()
{
	StopWatchdog();   // §6 layer 1: stop restarting first, then terminate
	TerminateProcess();
}
