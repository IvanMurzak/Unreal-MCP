// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/CriticalSection.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "UnrealMcpSupervisedProcess.h"   // the shared §1.5 crash-restart watchdog (FUnrealMcpSupervisedProcess Watchdog member below)

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
	 * Launch + supervise the local server on @p Port with the pre-composed @p LaunchArgs. Resolves the binary
	 * (override → cache → download), kills any orphaned `gamedev-mcp-server` already holding @p Port, spawns the
	 * process, then arms the crash-restart watchdog (which detects an early exit and re-launches with backoff).
	 * Returns true when the process was spawned (supervision proceeds asynchronously). Idempotent: a call while
	 * already running/starting is a no-op that returns true. Game-thread only.
	 *
	 * mcp-authorize g5/g6 consolidation: @p LaunchArgs is composed by the .NET sidecar's SHARED
	 * ServerLaunchArguments builder (none/oauth/token) and delivered over IPC — this manager NEVER assembles the
	 * arg string itself. The caller (FUnrealMcpEditorCoordinator) requests it via SendServerLaunchArgsRequest and
	 * calls Start in the async result callback, so the args are always the freshly-resolved auth mode.
	 */
	bool Start(int32 Port, const FString& LaunchArgs);

	/**
	 * Stop the supervised server. Stops the watchdog first (so a relaunch cannot undo the kill), then sends the
	 * platform terminate (KillTree) and, when @p bForce, blocks (bounded) until the child has actually exited —
	 * used on editor shutdown so no orphan survives. Clears the persisted PID. Idempotent. Game-thread only.
	 */
	void Stop(bool bForce = false);

	/** Whether the supervised server process is currently running. Game-thread only. */
	bool IsRunning() const;

	/** Total auto-restarts the watchdog performed since the last Start (for the UI status string). */
	int32 GetRestartCount() const { return Watchdog.GetRestartCount(); }

	/**
	 * Reattach to a server this plugin spawned in a PREVIOUS editor module load (a hot-reload / domain-reload
	 * survivor) whose PID is persisted under {Project}/Intermediate/UnrealMCP/server/server.pid. When the PID
	 * still names a live `gamedev-mcp-server`, adopt it (so a later Stop tears it down) and re-arm the watchdog;
	 * otherwise clear the stale marker. Returns true when an existing process was adopted. Game-thread only.
	 */
	bool ReattachIfRunning(int32 Port, const FString& LaunchArgs);

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

	/**
	 * Fail-closed verify-before-execute gate. Called from DownloadBinaryIfNeeded AFTER the zip bytes are in
	 * hand and BEFORE FZipArchiveReader / CreateProc. Fetches the release's `SHA256SUMS` manifest (a second
	 * blocking HTTP GET mirroring the zip download, with a bounded transient-retry), computes @p ZipBytes'
	 * SHA256 via a self-contained FIPS 180-4 SHA-256 (UE's FPlatformMisc::GetSHA256Signature is a desktop
	 * stub that asserts; no third-party dep), and compares against the manifest entry
	 * for @p AssetZipName via the pure-managed FUnrealMcpServerChecksum::VerifyZipChecksum. Returns true ONLY
	 * when the digest matched the manifest. Every failure path — a manifest we could not fetch after all
	 * retries, an unparsable manifest, a missing entry, or a digest mismatch — returns false with a clear
	 * UE_LOG so the caller skips extraction/launch. Game-thread; blocking. */
	bool VerifyDownloadedZip(const TArray<uint8>& ZipBytes, const FString& Version, const FString& AssetZipName) const;

	/**
	 * Download the `SHA256SUMS` manifest text with a bounded transient-retry (mirrors the zip download's
	 * tick-until-done pattern on the game thread). Writes the manifest body to @p OutText and returns true,
	 * or returns false when every attempt failed (the fail-closed signal). Game-thread; blocking. */
	static bool FetchSha256SumsText(const FString& SumsUrl, FString& OutText);

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
	 * TerminateProcess / crash that never runs FUnrealMcpEditorCoordinator::Shutdown's graceful stop. The job handle is
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

	FThreadSafeBool bStarting = false;

	// The §1.5 crash-restart watchdog (backoff {1,2,5,10,30}s + give-up after 5 crashes / 300s). Shared with
	// FUnrealMcpSidecarManager via FUnrealMcpSupervisedProcess; this manager supplies the IsAlive / Respawn /
	// OnGiveUp closures (the respawn kills an orphan on the port + re-spawns; give-up clears the pid file) in
	// StartWatchdog().
	FUnrealMcpSupervisedProcess Watchdog;
};
