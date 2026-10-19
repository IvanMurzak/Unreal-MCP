// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class FUnrealMcpToolRegistry;

/**
 * The environment a CLI session hands to an editor it starts (`@baizor/gamedev-cli-core` `SESSION_ENV`, Code-Exec
 * plan §4.6): the same names in every engine. Read once at startup.
 */
struct UNREALMCPEDITOR_API FUnrealMcpSessionEnv
{
	/** Seconds without a tool call after which the editor saves and quits. Unset / not a positive integer = off. */
	static constexpr const TCHAR* IdleSecondsVar = TEXT("AIGD_SESSION_IDLE_SECONDS");
	/** `cli` when a CLI session (not the user) started this editor. */
	static constexpr const TCHAR* OwnerVar = TEXT("AIGD_SESSION_OWNER");
	/** The nonce of the session lease (`<Project>/.ai-game-dev/session.json`) the CLI wrote for this editor. */
	static constexpr const TCHAR* NonceVar = TEXT("AIGD_SESSION_NONCE");

	int32 IdleSeconds = 0;
	FString Owner;
	FString Nonce;

	/** Started by a CLI session: only such an editor may be quit over `editor-quit` and owns the lease. */
	bool IsCliOwned() const { return Owner == TEXT("cli") && !Nonce.IsEmpty(); }

	/** Parse the raw values (pure). A malformed idle value turns the idle shutdown off rather than guessing. */
	static FUnrealMcpSessionEnv Parse(const FString& IdleRaw, const FString& OwnerRaw, const FString& NonceRaw);
	static FUnrealMcpSessionEnv FromProcess();
};

/**
 * Idle bookkeeping without engine APIs (the Unity-MCP IdleShutdownTimer port). Activity is any change of the tool
 * activity counter (a standard tool call finished), a busy editor (a call in flight, play-in-editor, asset
 * compilation), a long stalled tick (the game thread was blocked) or user input newer than the last activity.
 */
class UNREALMCPEDITOR_API FUnrealMcpIdleTimer
{
public:
	/**
	 * A gap between two checks longer than this means the game thread was blocked. Generous: a windowless editor
	 * ticks only every few seconds.
	 */
	static constexpr double StallThresholdSeconds = 60.0;

	FUnrealMcpIdleTimer(double InTimeoutSeconds, double Now);

	/** @p LastUserInput: the time of the last user interaction (0 when none); the same clock as @p Now. */
	bool ShouldQuit(double Now, uint64 ToolActivity, bool bBusy, double LastUserInput = 0.0);

private:
	double TimeoutSeconds;
	double LastActivity;
	double LastCheck;
	uint64 LastToolActivity = 0;
};

/**
 * The editor half of a CLI session (Code-Exec plan §4.6): quits an editor the CLI started after
 * AIGD_SESSION_IDLE_SECONDS without tool calls, quits it on request (`editor-quit`, so `close --owned-only` works on a
 * windowless editor that never sees WM_CLOSE), saves dirty packages that have a path before quitting, and removes the
 * CLI's lease on exit when its nonce is ours. Editors not started by a CLI are untouched.
 */
class UNREALMCPEDITOR_API FUnrealMcpEditorSession : public TSharedFromThis<FUnrealMcpEditorSession>
{
public:
	/** The system tool the CLI calls to quit an editor it started. */
	static constexpr const TCHAR* QuitToolName = TEXT("editor-quit");

	FUnrealMcpEditorSession(const FUnrealMcpSessionEnv& InEnv, const FString& InProjectDir);
	~FUnrealMcpEditorSession();

	/**
	 * Arm the idle shutdown when the environment asks for it. @p InToolActivity / @p InInFlightCalls read the bridge
	 * counters (FUnrealMcpBridgeServer::GetToolActivityCount / GetInFlightCallCount). Game thread.
	 */
	void Start(TFunction<uint64()> InToolActivity, TFunction<int32()> InInFlightCalls);

	/** Stop the idle ticker and any pending quit, and refuse new quits. Idempotent. Game thread. */
	void Disarm();

	/** Disarm and, for a CLI-owned editor, remove the lease when it still carries our nonce. Idempotent. Game thread. */
	void Shutdown();

	/** Register the `editor-quit` system tool (held weakly: a call after Shutdown is answered with an error). */
	void RegisterTools(FUnrealMcpToolRegistry& Registry);

	/** Save, then ask the engine to exit, on the next tick (so a tool answer still goes out). Game thread. */
	void RequestQuit(const FString& Reason);

	/** `<ProjectDir>/.ai-game-dev/session.json` (the cli-core lease path). Pure. */
	static FString LeasePath(const FString& ProjectDir);

	/** Whether the lease at @p Path parses and its `nonce` equals @p Nonce (case-sensitive; false for an empty nonce). */
	static bool LeaseCarriesNonce(const FString& Path, const FString& Nonce);

	/** Delete the lease at @p Path only when it parses and its `nonce` equals @p Nonce (compare-and-delete). */
	static bool RemoveLeaseIfOwned(const FString& Path, const FString& Nonce);

	/** Whether a dirty package has a place on disk (not /Temp, /Memory, transient or PIE): the ones idle shutdown saves. */
	static bool HasSavablePath(const FString& PackageName);

private:
	/** Save every dirty map and content package that has a path; an untitled map has nowhere to go. */
	static void SaveDirtyPackagesWithPath();

	bool Tick(float DeltaTime);

	FUnrealMcpSessionEnv Env;
	FString ProjectDir;
	TFunction<uint64()> ToolActivity;
	TFunction<int32()> InFlightCalls;
	TOptional<FUnrealMcpIdleTimer> IdleTimer;
	FTSTicker::FDelegateHandle TickHandle;
	FTSTicker::FDelegateHandle QuitHandle;
	bool bDisarmed = false;
	bool bLeaseReleased = false;
};
