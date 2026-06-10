// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "GenericPlatform/GenericPlatformProcess.h"

/**
 * Owns the lifecycle of the unreal-mcp-bridge sidecar process (docs/ARCHITECTURE.md §6): resolve the
 * binary, generate the one-shot IPC token, spawn the process delivering the token over stdin (§1.4),
 * watch it (crash → auto-restart with backoff, §1.5), and terminate it on editor shutdown (no orphans,
 * §6 layer 1). The download-on-first-run flow (§6) is intentionally STUBBED in this task behind the
 * UNREAL_MCP_BRIDGE_PATH dev override — the binary path must resolve from that env/.env var (or a
 * previously-downloaded location) until the release/download task lands.
 */
class FUnrealMcpSidecarManager
{
public:
	FUnrealMcpSidecarManager() = default;
	~FUnrealMcpSidecarManager();

	/**
	 * Resolve the bridge binary and spawn it pointed at @p IpcPort, delivering @p InToken over the
	 * child's stdin (§1.4). @p InToken is the SAME token the bridge server validates (generated once by
	 * the coordinator via GenerateToken so both sides agree). Returns true when the sidecar was spawned;
	 * false (and no spawn) when no binary resolves (the bridge server still listens; the operator can
	 * launch the sidecar manually with UNREAL_MCP_BRIDGE_PATH for the live e2e).
	 */
	bool StartForPort(int32 IpcPort, const FString& InToken);

	/** Terminate the sidecar and stop the watchdog (editor shutdown / module teardown). */
	void Stop();

	bool IsRunning() const;
	int32 GetRestartCount() const { return RestartCount.GetValue(); }

	/** Resolve the bridge binary path (UNREAL_MCP_BRIDGE_PATH override, §6). Empty when unresolved. */
	static FString ResolveBridgeBinaryPath();

	/** Generate a 32-byte token via the OS CSPRNG, hex-encoded (§1.4). */
	static FString GenerateToken();

private:
	bool SpawnProcess();
	void StartWatchdog();
	void StopWatchdog();
	void TerminateProcess();

	FString BridgePath;
	FString Token;
	int32 IpcPort = -1;

	FProcHandle ProcHandle;
	uint32 ProcId = 0;

	FThreadSafeBool bStopRequested = false;
	FThreadSafeCounter RestartCount;
	TFuture<void> WatchdogFuture;

	// Crash-rate guard (§1.5): > 5 crashes in 5 minutes → stop restarting.
	double WindowStartSeconds = 0.0;
	int32 CrashesInWindow = 0;
};
