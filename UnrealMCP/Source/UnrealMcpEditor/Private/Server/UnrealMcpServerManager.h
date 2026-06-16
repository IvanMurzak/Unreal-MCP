// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/CriticalSection.h"
#include "GenericPlatform/GenericPlatformProcess.h"

/**
 * Owns the lifecycle of the LOCAL shared `gamedev-mcp-server` process (the engine-agnostic MCP server
 * released from GameDev-MCP-Server, binary `gamedev-mcp-server`), the plugin-side analog of Unity's
 * in-process `McpServerManager.cs`. Adapted to UE/C++ (FPlatformProcess::CreateProc, a watchdog thread
 * for crash-restart, FCoreDelegates editor-shutdown teardown) — distinct from FUnrealMcpSidecarManager,
 * which owns the `unreal-mcp-bridge` sidecar (always required). The local server is ONLY relevant in
 * Custom connection mode with http transport: the AI agent dials `http://localhost:<port>` and this
 * process is what listens there.
 *
 * Lifecycle parity with Unity's manager: ResolveBinaryPath (env override → cached binary → download),
 * Start/Stop(force)/IsRunning, startup verification, crash-restart with backoff, orphan-port kill,
 * PID persistence + reattach across module reloads, graceful-then-force stop on editor shutdown so NO
 * orphaned `gamedev-mcp-server` survives editor close.
 *
 * Threading: Start/Stop/IsRunning and every status read are GAME-THREAD only (the UI calls them there).
 * A background watchdog thread owns ProcHandle mutation between Start and Stop; ProcessMutex guards the
 * handle so the game-thread Stop can race the watchdog safely.
 */
class FUnrealMcpServerManager
{
public:
	/** The pinned shared server version this plugin consumes. Kept in lockstep with cli/src/lib/server-version.ts. */
	static UNREALMCPEDITOR_API const TCHAR* ServerVersion;

	/** Env var that overrides the resolved server binary path (skips cache + download). Mirrors the CLI's UNREAL_MCP_SERVER_PATH. */
	static UNREALMCPEDITOR_API const TCHAR* ServerPathEnvVar;

	UNREALMCPEDITOR_API FUnrealMcpServerManager();
	UNREALMCPEDITOR_API ~FUnrealMcpServerManager();

	/**
	 * Launch + supervise the local server on @p Port with the given launch parameters (auth/token). Resolves
	 * the binary (override → cache → download), kills any orphaned `gamedev-mcp-server` already holding @p Port,
	 * spawns the process, verifies it survives a brief startup window, then arms the crash-restart watchdog.
	 * Returns true when the process was spawned (verification + supervision proceed asynchronously). Idempotent:
	 * a call while already running/starting is a no-op that returns true. Game-thread only.
	 *
	 * @p bAuthRequired / @p Token shape the `authorization` + `token` launch args exactly like Unity's BuildArguments.
	 */
	bool Start(int32 Port, int32 PluginTimeoutMs, bool bAuthRequired, const FString& Token);

	/**
	 * Stop the supervised server. Stops the watchdog first (so a relaunch cannot undo the kill), then sends the
	 * platform terminate (KillTree) and, when @p bForce, blocks (bounded) until the child has actually exited —
	 * used on editor shutdown so no orphan survives. Clears the persisted PID. Idempotent. Game-thread only.
	 */
	void Stop(bool bForce = false);

	/** Whether the supervised server process is currently running. Game-thread only. */
	bool IsRunning() const;

	/** Total auto-restarts the watchdog performed since the last Start (for the UI status string). */
	int32 GetRestartCount() const { return RestartCount.GetValue(); }

	/**
	 * Reattach to a server this plugin spawned in a PREVIOUS editor module load (a hot-reload / domain-reload
	 * survivor) whose PID is persisted under {Project}/Intermediate/UnrealMCP/server/server.pid. When the PID
	 * still names a live `gamedev-mcp-server`, adopt it (so a later Stop tears it down) and re-arm the watchdog;
	 * otherwise clear the stale marker. Returns true when an existing process was adopted. Game-thread only.
	 */
	bool ReattachIfRunning(int32 Port, int32 PluginTimeoutMs, bool bAuthRequired, const FString& Token);

	// --- Pure / static helpers (the spec-friendly heart — no live process, injectable filesystem). ---

	/** The platform server binary basename: "gamedev-mcp-server.exe" on Windows, "gamedev-mcp-server" elsewhere. */
	static UNREALMCPEDITOR_API FString ServerBinaryBasename();

	/**
	 * The .NET RID directory name for the current host (matches FUnrealMcpSidecarManager::ResolveRid and the
	 * CLI's ridForPlatform): "win-x64" / "linux-x64", or on macOS "osx-arm64" vs "osx-x64" from the physical
	 * host CPU. @p bArm64DirExists lets a caller degrade a missing arm64 slice to osx-x64; pure + injectable.
	 */
	static UNREALMCPEDITOR_API FString ResolveRid(bool bArm64DirExists = true);

	/**
	 * The §6 install dir for the local server under a project: <ProjectDir>/Intermediate/UnrealMCP/server/<rid>.
	 * Pure string composition (no FileExists), absolute-ized. Matches the CLI's serverInstallDir layout so the
	 * cli-downloaded binary and a plugin-downloaded binary share ONE cache. @p Rid lets a spec pin the slice.
	 */
	static UNREALMCPEDITOR_API FString ServerInstallDir(const FString& ProjectDir, const FString& Rid);

	/** The absolute path to the installed server binary under ServerInstallDir. Pure. */
	static UNREALMCPEDITOR_API FString ServerBinaryPathFor(const FString& ProjectDir, const FString& Rid);

	/** Read the `version` marker file in @p InstallDir (the version of the cached binary), or empty when absent. */
	static UNREALMCPEDITOR_API FString ReadVersionMarker(const FString& InstallDir);

	/**
	 * Parse the listen port out of a Custom-mode host URL ("http://localhost:8080" → 8080, "https://h" → 443,
	 * "http://h" → 80). Returns @p DefaultPort when the URL has no explicit port and no inferable scheme default,
	 * or when @p HostUrl is empty/unparseable. Pure — the launch port the agent dials must equal the port the
	 * server listens on, so both are derived from the SAME Custom host the UI shows (mirrors Unity's Port).
	 */
	static UNREALMCPEDITOR_API int32 ParsePortFromHost(const FString& HostUrl, int32 DefaultPort = 8080);

	/**
	 * Compose the server launch args (Unity BuildArguments parity, the order the server's CLI parser expects):
	 *   port=<port> plugin-timeout=<ms> client-transport=streamableHttp authorization=<none|required> [token=<t>]
	 * The token arg is appended ONLY when @p bAuthRequired AND @p Token is non-empty. Pure — exercised by a spec.
	 * The transport is ALWAYS streamableHttp for a launched local server (the agent's stdio/http choice is about
	 * how the AGENT reaches the server, not how the server is hosted) — matching Unity's hard-coded launch transport.
	 */
	static UNREALMCPEDITOR_API FString BuildLaunchArgs(int32 Port, int32 PluginTimeoutMs, bool bAuthRequired, const FString& Token);

	/**
	 * Whether the local server may be launched for the given mode + transport. ONLY Custom mode + http transport
	 * targets a locally-hosted server; Cloud (remote) and Custom+stdio never launch one. Pure (no engine access)
	 * so it is unit-testable and so the UI can hide/disable the Start affordance off this single predicate.
	 */
	static UNREALMCPEDITOR_API bool IsLaunchAllowed(bool bIsCustomMode, bool bIsHttpTransport);

	/**
	 * Resolve the server binary path for the LIVE project: ServerPathEnvVar override first (when set + present),
	 * else the cached binary under the §6 install dir when its `version` marker matches ServerVersion, else empty
	 * (caller downloads). Reads real env + filesystem; the project dir / rid come from the engine.
	 */
	UNREALMCPEDITOR_API FString ResolveBinaryPath() const;

private:
	/** macOS/Linux pre-spawn: chmod +x the resolved binary so a freshly-downloaded server is runnable. No-op on Windows. */
	static void PrepareBinaryForSpawn(const FString& Path);

	/** Download + unpack the pinned server zip into the §6 install dir (override/cache miss path). Blocking; game-thread. */
	bool DownloadBinaryIfNeeded(FString& OutBinaryPath);

	/** Spawn the resolved binary with the current launch args; records ProcHandle + ProcId under ProcessMutex. */
	bool SpawnProcess(const FString& BinaryPath);

	/** PID of the `gamedev-mcp-server` listening on @p Port, or -1. Uses netstat (Win) / lsof (Unix) like Unity. */
	static int32 GetPidListeningOnPort(int32 Port);

	/** Kill an ORPHANED `gamedev-mcp-server` holding @p Port that is not our own child (fails safe — never kills a non-server). */
	void KillOrphanedServerOnPort(int32 Port);

	void StartWatchdog();
	void StopWatchdog();
	void TerminateProcess(bool bForce);

	/**
	 * Windows-only: bind the just-spawned process @p ProcessHandle to a kill-on-job-close Job Object so the OS
	 * terminates the local server when THIS editor process dies by ANY means — including a hard
	 * TerminateProcess / crash that never runs FUnrealMcpRuntime::Shutdown's graceful stop. The job handle is
	 * owned by the manager (JobHandle) for the manager's lifetime; the editor process's death closes the last
	 * handle to it, tripping JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE. No-op on non-Windows (the graceful PreExit stop
	 * and KillTree terminate cover those; a hard-kill orphan there is out of scope for this Windows testbed).
	 */
	void BindProcessToKillOnCloseJob(void* ProcessHandle);

	/** {Project}/Intermediate/UnrealMCP/server/server.pid — survives a module reload for ReattachIfRunning. */
	static FString PidFilePath();
	static void WritePidFile(uint32 Pid);
	static void ClearPidFile();
	static int32 ReadPidFile();
	/** Whether @p Pid currently names a live process whose name contains "gamedev-mcp-server". */
	static bool IsServerProcessAlive(int32 Pid);

	FString BinaryPath;
	FString LaunchArgs;
	int32 ListenPort = -1;

	mutable FCriticalSection ProcessMutex;
	FProcHandle ProcHandle;
	uint32 ProcId = 0;

	// Windows-only kill-on-close Job Object handle (HANDLE stored as void*; nullptr off Windows / before first
	// spawn). Created lazily on the first spawn and kept for the manager's lifetime so the OS reaps the server
	// when the editor process dies by ANY means (graceful quit, hard TerminateProcess, or crash). Closed in the
	// destructor. See BindProcessToKillOnCloseJob.
	void* JobHandle = nullptr;

	FThreadSafeBool bStopRequested = false;
	FThreadSafeBool bStarting = false;
	FThreadSafeCounter RestartCount;
	TFuture<void> WatchdogFuture;

	// Crash-rate guard (mirrors the sidecar §1.5): > 5 crashes in 5 minutes → stop restarting.
	double WindowStartSeconds = 0.0;
	int32 CrashesInWindow = 0;
};
