// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"   // FProcHandle is a by-value member below; the platform header defines the complete type (the generic header only forward-declares it)
#include "UnrealMcpSupervisedProcess.h"   // the shared §1.5 crash-restart watchdog (FUnrealMcpSupervisedProcess Watchdog member below)

/**
 * Owns the lifecycle of the unreal-mcp-bridge sidecar process (docs/ARCHITECTURE.md §6): resolve the
 * binary, generate the one-shot IPC token, spawn the process delivering the token over stdin (§1.4),
 * watch it (crash → auto-restart with backoff, §1.5), and terminate it on editor shutdown (no orphans,
 * §6 layer 1). Binary resolution follows the §6 BUNDLE model: (1) the UNREAL_MCP_BRIDGE_PATH dev/CI
 * override wins when set, then (2) the prebuilt self-contained binary bundled inside the plugin — checked
 * at the STAGED path Binaries/ThirdParty/UnrealMcpBridge/<rid>/ first, then the FAB-SURVIVING source
 * Source/ThirdParty/UnrealMcpBridge/<rid>/ (#139/#187: Fab strips Binaries/ from a source submission, so
 * the surviving folder is what Epic's recompile stages from). A fresh GUI install resolves the bundled path and auto-spawns at
 * StartupModule with zero user action; a dev source checkout (no staged binary) falls back to the
 * override, exactly as before.
 */
class UNREALMCPRUNTIME_API FUnrealMcpSidecarManager
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

	/**
	 * Graceful-shutdown step 1 (§1.5): stop the auto-restart watchdog WITHOUT terminating the process, so
	 * the bridge can send the sidecar a graceful `shutdown` and the watchdog will not relaunch it. Pair with
	 * WaitForExit() then Stop() (terminate backstop). Idempotent.
	 */
	void StopRestarts();

	/**
	 * Wait (bounded) for the sidecar to exit on its own after a graceful `shutdown`. Returns true if it has
	 * exited (or was never spawned), false if it is still running at the deadline (caller then terminates).
	 */
	bool WaitForExit(FTimespan Grace);

	/**
	 * Whether the sidecar process is currently running. Threading: call only from the game thread / the
	 * Stop path. The watchdog thread owns ProcHandle's mutation (spawn/CloseProc) otherwise, and this read
	 * is intentionally unsynchronized — a concurrent call racing a watchdog restart is not a supported use.
	 */
	bool IsRunning() const;
	int32 GetRestartCount() const { return Watchdog.GetRestartCount(); }

	/**
	 * Resolve the bridge binary path (§6 BUNDLE model): UNREAL_MCP_BRIDGE_PATH dev/CI override first,
	 * then the bundled path inside the plugin. Empty when neither resolves (caller logs the actionable
	 * packaged-without-<rid> error). The returned bundled path is absolute (ConvertRelativePathToFull).
	 */
	static FString ResolveBridgeBinaryPath();

	/**
	 * The .NET RID directory name for the current host platform (§6.2): "win-x64" / "linux-x64", or
	 * on macOS "osx-arm64" vs "osx-x64" chosen from the PHYSICAL host CPU (not the possibly-Rosetta-
	 * translated editor arch). @p bArm64DirExists lets the caller fall back to osx-x64 when the
	 * arm64 slice is absent; pure + injectable so it is unit-testable without touching the filesystem.
	 */
	static FString ResolveRid(bool bArm64DirExists = true);

	/**
	 * Compose the STAGED bundled bridge path under @p PluginBaseDir for the current host (§6.1):
	 * <PluginBaseDir>/Binaries/ThirdParty/UnrealMcpBridge/<rid>/unreal-mcp-bridge[.exe]. Pure string
	 * composition (no FileExists), absolute-ized — testable independent of a staged binary. @p
	 * bArm64DirExists is forwarded to ResolveRid so the production resolver can pass its arm64-probe
	 * result (a missing osx-arm64 slice degrades the composed path to osx-x64). This is the packaged-game /
	 * non-Fab path; the Fab-surviving source location is ComposeSurvivingBridgePath (#139).
	 */
	static FString ComposeBundledBridgePath(const FString& PluginBaseDir, bool bArm64DirExists = true);

	/**
	 * Compose the FAB-SURVIVING bridge source path under @p PluginBaseDir for the current host (#139/#187):
	 * <PluginBaseDir>/Source/ThirdParty/UnrealMcpBridge/<rid>/unreal-mcp-bridge[.exe]. Fab strips Binaries/
	 * from a submitted SOURCE plugin, so the prebuilt sidecar lives under the engine-canonical
	 * Source/ThirdParty/ layout (declared in Config/FilterPlugin.ini) to survive the strip; UBT stages it
	 * into Binaries/ThirdParty at Epic's compile time. Pure string composition, absolute-ized.
	 */
	static FString ComposeSurvivingBridgePath(const FString& PluginBaseDir, bool bArm64DirExists = true);

	/**
	 * The ordered bridge-path candidates the production resolver walks (§6.3 step 2): the STAGED
	 * Binaries/ThirdParty path first, then the FAB-SURVIVING Source/ThirdParty/UnrealMcpBridge/<rid>/ source
	 * (#139/#187). Empty entries
	 * (empty base dir / non-desktop rid) are omitted so callers can FileExists-walk a clean list.
	 */
	static TArray<FString> ComposeBundledBridgeCandidates(const FString& PluginBaseDir, bool bArm64DirExists = true);

	/** The platform bridge binary basename: "unreal-mcp-bridge.exe" on Windows, "unreal-mcp-bridge" elsewhere. */
	static FString BridgeBinaryBasename();

	/** Generate a 32-byte token via the OS CSPRNG, hex-encoded (§1.4). */
	static FString GenerateToken();

	/**
	 * macOS/Linux pre-spawn prep on @p Path (§6.6): ensure the executable bit (+x) and, on macOS, strip
	 * the com.apple.quarantine xattr so Gatekeeper's first-exec check is bypassed offline. No-op on
	 * Windows. Static + path-taking so a spec can exercise it on a fixture binary; tolerant of a
	 * missing xattr. Returns true if no fatal error occurred (a missing quarantine attr is not fatal).
	 */
	static bool PrepareBundledBinaryForSpawn(const FString& Path);

private:
	/** Whether the osx-arm64 bridge slice is present in the bundle (always true off macOS). §6.2 defensive. */
	static bool BundledArm64SliceExists();

	/** The rid the resolver would actually use for the current host (arm64-probed); for log/error messages. */
	static FString ResolveEffectiveRid();

	bool SpawnProcess();
	void StartWatchdog();
	void StopWatchdog();
	void TerminateProcess();

	FString BridgePath;
	FString Token;
	int32 IpcPort = -1;

	FProcHandle ProcHandle;
	uint32 ProcId = 0;

	// The §1.5 crash-restart watchdog (backoff {1,2,5,10,30}s + give-up after 5 crashes / 300s). Shared with
	// FUnrealMcpServerManager via FUnrealMcpSupervisedProcess; this manager supplies the IsAlive / Respawn
	// closures in StartWatchdog().
	FUnrealMcpSupervisedProcess Watchdog;
};
