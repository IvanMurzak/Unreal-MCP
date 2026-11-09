// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Server/UnrealMcpServerManager.h"
#include "Server/UnrealMcpServerChecksum.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpProcessUtil.h"   // shared ResolveDotNetRid + MakeExecutable

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"



#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// Single source of the consumed shared-server version. Kept in lockstep with cli/src/lib/server-version.ts
// (SERVER_VERSION) and Unity's McpServerManager.ServerVersion — the three plugins consume the SAME server.
const TCHAR* FUnrealMcpServerManager::ServerVersion = TEXT("9.4.4");
const TCHAR* FUnrealMcpServerManager::ServerPathEnvVar = TEXT("UNREAL_MCP_SERVER_PATH");
// GameDev-MCP-Server 9.2.12 is the last release on McpPlugin.Server < 8.11.0; the next one carries 8.11.0.
const TCHAR* FUnrealMcpServerManager::MinServerVersionWithToolTimeout = TEXT("9.2.13");

namespace
{
	// The §1.5 backoff schedule + crash-rate constants now live in FUnrealMcpSupervisedProcess.
	const TCHAR* ServerProcessNameNeedle = TEXT("gamedev-mcp-server");


	// The UNREAL_MCP_SERVER_PATH dev/CI override when it is set and points at an existing file, else empty.
	FString ExistingServerPathOverride()
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(FUnrealMcpServerManager::ServerPathEnvVar);
		return !Override.IsEmpty() && FPaths::FileExists(Override) ? Override : FString();
	}
}

FUnrealMcpServerManager::FUnrealMcpServerManager() = default;

FUnrealMcpServerManager::~FUnrealMcpServerManager()
{
	Stop(/*bForce*/ true);
#if PLATFORM_WINDOWS
	// Close the kill-on-job-close Job Object handle. If any server process is still assigned to it, closing the
	// LAST handle here trips JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE and reaps it — a final backstop on top of Stop().
	if (JobHandle != nullptr)
	{
		::CloseHandle(static_cast<HANDLE>(JobHandle));
		JobHandle = nullptr;
	}
#endif
}

FString FUnrealMcpServerManager::ServerBinaryBasename()
{
#if PLATFORM_WINDOWS
	return TEXT("gamedev-mcp-server.exe");
#else
	return TEXT("gamedev-mcp-server");
#endif
}

FString FUnrealMcpServerManager::ResolveRid(bool bArm64DirExists)
{
	// The RID resolver is shared with FUnrealMcpSidecarManager — see FUnrealMcpProcessUtil::ResolveDotNetRid
	// (§6.2). This thin forwarder keeps the tested static-member entry point.
	return FUnrealMcpProcessUtil::ResolveDotNetRid(bArm64DirExists);
}

FString FUnrealMcpServerManager::ServerInstallDir(const FString& ProjectDir, const FString& Rid)
{
	if (ProjectDir.IsEmpty() || Rid.IsEmpty())
		return FString();
	const FString Path = ProjectDir / TEXT("Intermediate") / TEXT("UnrealMCP") / TEXT("server") / Rid;
	return FPaths::ConvertRelativePathToFull(Path);
}

FString FUnrealMcpServerManager::ServerBinaryPathFor(const FString& ProjectDir, const FString& Rid)
{
	const FString Dir = ServerInstallDir(ProjectDir, Rid);
	if (Dir.IsEmpty())
		return FString();
	return Dir / ServerBinaryBasename();
}

FString FUnrealMcpServerManager::ReadVersionMarker(const FString& InstallDir)
{
	if (InstallDir.IsEmpty())
		return FString();
	const FString Marker = InstallDir / TEXT("version");
	FString Contents;
	if (FFileHelper::LoadFileToString(Contents, *Marker))
		return Contents.TrimStartAndEnd();
	return FString();
}

int32 FUnrealMcpServerManager::ParsePortFromHost(const FString& HostUrl, int32 DefaultPort)
{
	const FString Trimmed = HostUrl.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
		return DefaultPort;

	// Strip the scheme, remember it for the default-port inference.
	FString Rest = Trimmed;
	bool bHttps = false;
	bool bHadScheme = false;
	if (Rest.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)) { Rest.RightChopInline(8); bHttps = true; bHadScheme = true; }
	else if (Rest.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)) { Rest.RightChopInline(7); bHadScheme = true; }

	// Drop any path / query after the authority.
	int32 SlashIdx;
	if (Rest.FindChar(TEXT('/'), SlashIdx))
		Rest.LeftInline(SlashIdx);

	// authority is host[:port]. An IPv6 literal (in []) has colons inside the brackets — guard against it.
	int32 ColonIdx;
	if (Rest.StartsWith(TEXT("[")))
	{
		int32 CloseIdx;
		if (Rest.FindChar(TEXT(']'), CloseIdx) && CloseIdx + 1 < Rest.Len() && Rest[CloseIdx + 1] == TEXT(':'))
		{
			const FString PortStr = Rest.RightChop(CloseIdx + 2);
			const int32 Port = FCString::Atoi(*PortStr);
			if (Port > 0 && Port <= 65535)
				return Port;
		}
	}
	else if (Rest.FindLastChar(TEXT(':'), ColonIdx))
	{
		const FString PortStr = Rest.RightChop(ColonIdx + 1);
		const int32 Port = FCString::Atoi(*PortStr);
		if (Port > 0 && Port <= 65535)
			return Port;
	}

	// No explicit port: infer from the scheme, else fall back to the supplied default.
	if (bHadScheme)
		return bHttps ? 443 : 80;
	return DefaultPort;
}

bool FUnrealMcpServerManager::IsLaunchAllowed(bool bIsCustomMode, bool bIsHttpTransport)
{
	return bIsCustomMode && bIsHttpTransport;
}

void FUnrealMcpServerManager::PrepareBinaryForSpawn(const FString& Path)
{
	// §6.6: ensure the apphost is executable (shared +x helper — no-op on Windows). Not fatal on failure.
	FUnrealMcpProcessUtil::MakeExecutable(Path);
}

bool FUnrealMcpServerManager::IsServerVersionAtLeast(const FString& Version, const FString& Minimum)
{
	TArray<FString> Have, Need;
	Version.ParseIntoArray(Have, TEXT("."));
	Minimum.ParseIntoArray(Need, TEXT("."));
	for (int32 Index = 0; Index < FMath::Max(Have.Num(), Need.Num()); ++Index)
	{
		const int32 A = Have.IsValidIndex(Index) ? FCString::Atoi(*Have[Index]) : 0;
		const int32 B = Need.IsValidIndex(Index) ? FCString::Atoi(*Need[Index]) : 0;
		if (A != B)
			return A > B;
	}
	return true;
}

bool FUnrealMcpServerManager::ServerHasToolTimeout()
{
	static const bool bPinnedHasToolTimeout = IsServerVersionAtLeast(ServerVersion, MinServerVersionWithToolTimeout);
	if (!ExistingServerPathOverride().IsEmpty())
		return false; // a dev/CI binary of unknown version (ResolveBinaryPath prefers it)
	return bPinnedHasToolTimeout;
}

FString FUnrealMcpServerManager::ResolveBinaryPath() const
{
	// 1. Dev/CI override — wins when set + present (mirrors the sidecar's UNREAL_MCP_BRIDGE_PATH and the CLI's
	//    UNREAL_MCP_SERVER_PATH). Skips the version check, exactly like download-server.ts's override branch.
	const FString Override = ExistingServerPathOverride();
	if (!Override.IsEmpty())
		return Override;

	// 2. Cached binary under the §6 install dir whose `version` marker matches the pin → reuse (no network).
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString Rid = ResolveRid();
	const FString InstallDir = ServerInstallDir(ProjectDir, Rid);
	const FString BinPath = ServerBinaryPathFor(ProjectDir, Rid);
	// §6.8: in the Fab build the user installed the file by hand; only a plugin download writes the `version` marker.
	if (!BinPath.IsEmpty() && FPaths::FileExists(BinPath) && (UNREAL_MCP_FAB_DISTRIBUTION || ReadVersionMarker(InstallDir) == ServerVersion))
		return BinPath;

	// 3. Neither resolves — caller downloads (or surfaces the CLI hint).
	return FString();
}

bool FUnrealMcpServerManager::SpawnProcess(const FString& InBinaryPath)
{
	uint32 NewProcId = 0;
	FProcHandle NewHandle = FPlatformProcess::CreateProc(
		*InBinaryPath,
		*LaunchArgs,
		/*bLaunchDetached*/ false,
		/*bLaunchHidden*/ true,
		/*bLaunchReallyHidden*/ true,
		&NewProcId,
		/*PriorityModifier*/ 0,
		/*OptionalWorkingDirectory*/ *FPaths::GetPath(InBinaryPath),
		/*PipeWriteChild*/ nullptr,
		/*PipeReadChild*/ nullptr);

	if (!NewHandle.IsValid())
		return false;

	{
		FScopeLock Lock(&ProcessMutex);
		ProcHandle = NewHandle;
		ProcId = NewProcId;
	}
	// Bind the child to the kill-on-job-close job so it dies with the editor by ANY exit path (graceful, hard
	// TerminateProcess, or crash) — the load-bearing guarantee for "Start then close editor → no orphan" when
	// the engine's graceful OnEnginePreExit teardown never runs. The handle (HANDLE on Windows) is ProcHandle.Get().
#if PLATFORM_WINDOWS
	BindProcessToKillOnCloseJob(NewHandle.Get());
#endif
	WritePidFile(NewProcId);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] spawned local server pid=%u on port=%d."), NewProcId, ListenPort);
	return true;
}

#if PLATFORM_WINDOWS
void FUnrealMcpServerManager::BindProcessToKillOnCloseJob(void* ProcessHandle)
{
	if (ProcessHandle == nullptr)
		return;

	// Create the job lazily on the first spawn; reuse it for restarts so one editor session = one job.
	if (JobHandle == nullptr)
	{
		HANDLE NewJob = ::CreateJobObject(nullptr, nullptr);
		if (NewJob == nullptr)
		{
			UE_LOG(LogUnrealMcp, Warning,
				TEXT("[Unreal-MCP] CreateJobObject failed (err=%lu); relying on the graceful shutdown stop only."),
				::GetLastError());
			return;
		}
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info = {};
		Info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!::SetInformationJobObject(NewJob, JobObjectExtendedLimitInformation, &Info, sizeof(Info)))
		{
			UE_LOG(LogUnrealMcp, Warning,
				TEXT("[Unreal-MCP] SetInformationJobObject failed (err=%lu); job will not kill-on-close."),
				::GetLastError());
			::CloseHandle(NewJob);
			return;
		}
		JobHandle = NewJob;
	}

	if (!::AssignProcessToJobObject(static_cast<HANDLE>(JobHandle), static_cast<HANDLE>(ProcessHandle)))
	{
		// A common benign cause: the process was created in a job that disallows nesting/breakaway. Log and move
		// on — the graceful OnEnginePreExit stop still covers a clean editor close.
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] AssignProcessToJobObject failed (err=%lu); kill-on-editor-exit not guaranteed for this server."),
			::GetLastError());
	}
	else
	{
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] local server bound to kill-on-close job."));
	}
}
#endif

bool FUnrealMcpServerManager::Start(int32 Port, const FString& InLaunchArgs)
{
	if (IsRunning() || bStarting)
	{
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] local server already running/starting; Start is a no-op."));
		return true;
	}

	bStarting = true;
	ListenPort = Port;
	// mcp-authorize g5/g6: LaunchArgs are pre-composed by the .NET sidecar's shared ServerLaunchArguments builder
	// and passed in — this manager never assembles the none/oauth/token arg string.
	LaunchArgs = InLaunchArgs;

	// Resolve the binary (override → cache); download on a miss.
	BinaryPath = ResolveBinaryPath();
	if (BinaryPath.IsEmpty())
	{
#if UNREAL_MCP_FAB_DISTRIBUTION
		// §6.8: the Fab build never downloads an executable. The user installs the server by hand.
		bStarting = false;
		UE_LOG(LogUnrealMcp, Error,
			TEXT("[Unreal-MCP] the local gamedev-mcp-server is not installed. Download %s from ")
			TEXT("https://github.com/IvanMurzak/GameDev-MCP-Server/releases/tag/v%s and extract it into %s, or set %s."),
			*FUnrealMcpServerChecksum::ServerZipAssetName(ResolveRid()), ServerVersion, *ServerInstallDir(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), ResolveRid()), ServerPathEnvVar);
		return false;
#else
		FString Downloaded;
		if (!DownloadBinaryIfNeeded(Downloaded))
		{
			bStarting = false;
			UE_LOG(LogUnrealMcp, Error,
				TEXT("[Unreal-MCP] could not resolve or download the local gamedev-mcp-server. ")
				TEXT("Set %s to a local build, or run `unreal-mcp-cli` to populate the server cache."),
				ServerPathEnvVar);
			return false;
		}
		BinaryPath = Downloaded;
#endif
	}

	PrepareBinaryForSpawn(BinaryPath);

	// Free the port if an orphaned server from a previous session is squatting on it.
	KillOrphanedServerOnPort(Port);

	if (!SpawnProcess(BinaryPath))
	{
		bStarting = false;
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] failed to spawn the local server from '%s'."), *BinaryPath);
		return false;
	}

	Watchdog.ResetRestartCount();   // fresh Start zeroes the restart counter (Watchdog.Start resets the crash window).
	StartWatchdog();
	bStarting = false;
	return true;
}

bool FUnrealMcpServerManager::ReattachIfRunning(int32 Port, const FString& InLaunchArgs)
{
	const int32 SavedPid = ReadPidFile();
	if (SavedPid <= 0 || !IsServerProcessAlive(SavedPid))
	{
		ClearPidFile();
		return false;
	}

	// Adopt the survivor: open a handle so a later Stop can terminate it, and re-arm supervision.
	FProcHandle Adopted = FPlatformProcess::OpenProcess(static_cast<uint32>(SavedPid));
	if (!Adopted.IsValid())
	{
		ClearPidFile();
		return false;
	}

	ListenPort = Port;
	LaunchArgs = InLaunchArgs; // pre-composed by the sidecar (g5/g6) — used by the watchdog's respawn closure.
	BinaryPath = ResolveBinaryPath();
	{
		FScopeLock Lock(&ProcessMutex);
		ProcHandle = Adopted;
		ProcId = static_cast<uint32>(SavedPid);
	}
	// Re-bind the adopted survivor to a kill-on-close job so it too dies with THIS editor session by any exit
	// path (the original spawning session's job died with that session's editor).
#if PLATFORM_WINDOWS
	BindProcessToKillOnCloseJob(Adopted.Get());
#endif
	// Reattach does NOT reset the restart counter (only a fresh Start does); Watchdog.Start resets the crash window.
	StartWatchdog();
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] reattached to existing local server pid=%d on port=%d."), SavedPid, Port);
	return true;
}

void FUnrealMcpServerManager::StartWatchdog()
{
	// The §1.5 crash-restart loop lives in FUnrealMcpSupervisedProcess; supply the server-specific liveness probe
	// + respawn + give-up. The liveness read and the dead-handle close both take ProcessMutex (the watchdog thread
	// otherwise owns ProcHandle mutation); the respawn first frees any orphan squatting on the listen port, then
	// re-spawns; give-up clears the pid file. All run ON the watchdog thread.
	Watchdog.Start(
		/*IsAlive*/ [this]() -> bool
		{
			FScopeLock Lock(&ProcessMutex);
			return ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcHandle);
		},
		/*Respawn*/ [this]() -> bool
		{
			{
				FScopeLock Lock(&ProcessMutex);
				if (ProcHandle.IsValid())
				{
					FPlatformProcess::CloseProc(ProcHandle);
					ProcHandle.Reset();
				}
			}
			if (ListenPort > 0)
				KillOrphanedServerOnPort(ListenPort);
			return SpawnProcess(BinaryPath);
		},
		/*OnGiveUp*/ [this]() { ClearPidFile(); },
		TEXT("local server"));
}

void FUnrealMcpServerManager::StopWatchdog()
{
	Watchdog.Stop();
}

void FUnrealMcpServerManager::TerminateProcess(bool bForce)
{
	FProcHandle Handle;
	{
		FScopeLock Lock(&ProcessMutex);
		Handle = ProcHandle;
	}
	if (Handle.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(Handle))
			FPlatformProcess::TerminateProc(Handle, /*KillTree*/ true);
		if (bForce)
		{
			// Bounded wait so editor shutdown does not orphan the child if TerminateProc is still propagating.
			const double Deadline = FPlatformTime::Seconds() + 5.0;
			while (FPlatformProcess::IsProcRunning(Handle) && FPlatformTime::Seconds() < Deadline)
				FPlatformProcess::Sleep(0.05f);
		}
		FPlatformProcess::CloseProc(Handle);
	}
	{
		FScopeLock Lock(&ProcessMutex);
		ProcHandle.Reset();
		ProcId = 0;
	}
}

void FUnrealMcpServerManager::Stop(bool bForce)
{
	StopWatchdog();        // stop restarting BEFORE terminating, so a relaunch cannot undo the kill
	TerminateProcess(bForce);
	ClearPidFile();
}

bool FUnrealMcpServerManager::IsRunning() const
{
	FScopeLock Lock(&ProcessMutex);
	return ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(const_cast<FProcHandle&>(ProcHandle));
}

FString FUnrealMcpServerManager::PidFilePath()
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	return ProjectDir / TEXT("Intermediate") / TEXT("UnrealMCP") / TEXT("server") / TEXT("server.pid");
}

void FUnrealMcpServerManager::WritePidFile(uint32 Pid)
{
	const FString Path = PidFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	FFileHelper::SaveStringToFile(FString::FromInt(static_cast<int32>(Pid)), *Path);
}

void FUnrealMcpServerManager::ClearPidFile()
{
	IFileManager::Get().Delete(*PidFilePath(), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
}

int32 FUnrealMcpServerManager::ReadPidFile()
{
	FString Contents;
	if (FFileHelper::LoadFileToString(Contents, *PidFilePath()))
	{
		const int32 Pid = FCString::Atoi(*Contents.TrimStartAndEnd());
		return Pid > 0 ? Pid : -1;
	}
	return -1;
}

bool FUnrealMcpServerManager::IsServerProcessAlive(int32 Pid)
{
	if (Pid <= 0)
		return false;
	const FString Name = FPlatformProcess::GetApplicationName(static_cast<uint32>(Pid));
	if (Name.IsEmpty())
		return false;
	return Name.Contains(ServerProcessNameNeedle);
}

int32 FUnrealMcpServerManager::GetPidListeningOnPort(int32 Port)
{
	int32 ReturnCode = -1;
	FString StdOut;
	FString StdErr;

#if PLATFORM_WINDOWS
	const FString Cmd = TEXT("netstat.exe");
	const FString Params = TEXT("-ano -p tcp");
#else
	const FString Cmd = TEXT("lsof");
	const FString Params = FString::Printf(TEXT("-ti tcp:%d -sTCP:LISTEN"), Port);
#endif

	if (!FPlatformProcess::ExecProcess(*Cmd, *Params, &ReturnCode, &StdOut, &StdErr))
		return -1;

#if PLATFORM_WINDOWS
	const FString PortSuffix = FString::Printf(TEXT(":%d"), Port);
	TArray<FString> Lines;
	StdOut.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (!Trimmed.Contains(TEXT("LISTENING")))
			continue;
		TArray<FString> Parts;
		Trimmed.ParseIntoArray(Parts, TEXT(" "), /*CullEmpty*/ true);
		if (Parts.Num() < 5)
			continue;
		// Parts: Proto LocalAddr ForeignAddr State PID
		if (Parts[1].EndsWith(PortSuffix))
		{
			const int32 Pid = FCString::Atoi(*Parts.Last());
			if (Pid > 0)
				return Pid;
		}
	}
#else
	const FString Trimmed = StdOut.TrimStartAndEnd();
	if (!Trimmed.IsEmpty())
	{
		TArray<FString> Lines;
		Trimmed.ParseIntoArrayLines(Lines);
		if (Lines.Num() > 0)
		{
			const int32 Pid = FCString::Atoi(*Lines[0].TrimStartAndEnd());
			if (Pid > 0)
				return Pid;
		}
	}
#endif
	return -1;
}

void FUnrealMcpServerManager::KillOrphanedServerOnPort(int32 Port)
{
	const int32 ListeningPid = GetPidListeningOnPort(Port);
	if (ListeningPid <= 0)
		return;

	uint32 OwnPid;
	{
		FScopeLock Lock(&ProcessMutex);
		OwnPid = ProcId;
	}
	if (static_cast<uint32>(ListeningPid) == OwnPid)
		return; // our own child

	// Fail safe: only kill it when it is actually a gamedev-mcp-server (never a random port owner).
	if (!IsServerProcessAlive(ListeningPid))
	{
		UE_LOG(LogUnrealMcp, Warning,
			TEXT("[Unreal-MCP] port %d is held by a non-server process (pid=%d); not killing it. ")
			TEXT("Free the port or change the Custom host."), Port, ListeningPid);
		return;
	}

	UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] killing orphaned local server pid=%d holding port %d."), ListeningPid, Port);
	FProcHandle Orphan = FPlatformProcess::OpenProcess(static_cast<uint32>(ListeningPid));
	if (Orphan.IsValid())
	{
		FPlatformProcess::TerminateProc(Orphan, /*KillTree*/ true);
		const double Deadline = FPlatformTime::Seconds() + 3.0;
		while (FPlatformProcess::IsProcRunning(Orphan) && FPlatformTime::Seconds() < Deadline)
			FPlatformProcess::Sleep(0.05f);
		FPlatformProcess::CloseProc(Orphan);
	}
}
