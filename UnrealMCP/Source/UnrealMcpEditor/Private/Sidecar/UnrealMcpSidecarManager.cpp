// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpSidecarManager.h"
#include "UnrealMcpLog.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

#include <random>

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

FString FUnrealMcpSidecarManager::ResolveBridgeBinaryPath()
{
	// §6 dev override (and the only resolution path until the download task lands).
	const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("UNREAL_MCP_BRIDGE_PATH"));
	if (!Override.IsEmpty() && FPaths::FileExists(Override))
		return Override;

	// TODO(§6 download task): download-on-first-run of the BRIDGE from this repo's GitHub Releases
	// into <Project>/Intermediate/UnrealMCP/bridge/<platform>/unreal-mcp-bridge(.exe). Stubbed here.
	// Note: only the bridge is C++-managed. The local MCP server (gamedev-mcp-server, the shared
	// IvanMurzak/GameDev-MCP-Server release) is acquired by the CLI (`unreal-mcp-cli` download-server
	// module, §6) — do NOT implement server download here.
	return FString();
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
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] no sidecar binary resolved (set UNREAL_MCP_BRIDGE_PATH). The bridge is listening on %d; launch the sidecar manually for the live e2e."),
			IpcPort);
		return false;
	}

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
