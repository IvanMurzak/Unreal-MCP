// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Session/UnrealMcpEditorSession.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpToolRegistry.h"

#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

const TCHAR* FUnrealMcpSessionEnv::IdleSecondsVar = TEXT("AIGD_SESSION_IDLE_SECONDS");
const TCHAR* FUnrealMcpSessionEnv::OwnerVar = TEXT("AIGD_SESSION_OWNER");
const TCHAR* FUnrealMcpSessionEnv::NonceVar = TEXT("AIGD_SESSION_NONCE");
const TCHAR* FUnrealMcpEditorSession::QuitToolName = TEXT("editor-quit");

// --- FUnrealMcpSessionEnv -------------------------------------------------------------------------

FUnrealMcpSessionEnv FUnrealMcpSessionEnv::Parse(const FString& IdleRaw, const FString& OwnerRaw, const FString& NonceRaw)
{
	FUnrealMcpSessionEnv Result;
	const FString Idle = IdleRaw.TrimStartAndEnd();
	// Digits only: "300s", "-5" or "1e3" turn the shutdown off instead of quitting at a guessed time.
	if (!Idle.IsEmpty() && Idle.Len() <= 9)
	{
		bool bDigits = true;
		for (const TCHAR Ch : Idle)
			bDigits &= FChar::IsDigit(Ch);
		if (bDigits)
			Result.IdleSeconds = FCString::Atoi(*Idle);
	}
	Result.Owner = OwnerRaw.TrimStartAndEnd();
	Result.Nonce = NonceRaw.TrimStartAndEnd();
	return Result;
}

FUnrealMcpSessionEnv FUnrealMcpSessionEnv::FromProcess()
{
	return Parse(FPlatformMisc::GetEnvironmentVariable(IdleSecondsVar), FPlatformMisc::GetEnvironmentVariable(OwnerVar),
		FPlatformMisc::GetEnvironmentVariable(NonceVar));
}

// --- FUnrealMcpIdleTimer --------------------------------------------------------------------------

FUnrealMcpIdleTimer::FUnrealMcpIdleTimer(double InTimeoutSeconds, double Now)
	: TimeoutSeconds(InTimeoutSeconds)
	, LastActivity(Now)
	, LastCheck(Now)
{
}

bool FUnrealMcpIdleTimer::ShouldQuit(double Now, uint64 ToolActivity, bool bBusy, double LastUserInput)
{
	const bool bStalled = Now - LastCheck > StallThresholdSeconds;
	LastCheck = Now;
	if (bBusy || bStalled || ToolActivity != LastToolActivity)
	{
		LastToolActivity = ToolActivity;
		LastActivity = Now;
		return false;
	}
	LastActivity = FMath::Max(LastActivity, FMath::Min(LastUserInput, Now));
	return Now - LastActivity >= TimeoutSeconds;
}

// --- FUnrealMcpEditorSession ----------------------------------------------------------------------

FUnrealMcpEditorSession::FUnrealMcpEditorSession(const FUnrealMcpSessionEnv& InEnv, const FString& InProjectDir)
	: Env(InEnv)
	, ProjectDir(InProjectDir)
{
}

FUnrealMcpEditorSession::~FUnrealMcpEditorSession()
{
	Shutdown();
}

void FUnrealMcpEditorSession::Start(TFunction<uint64()> InToolActivity, TFunction<int32()> InInFlightCalls)
{
	ToolActivity = MoveTemp(InToolActivity);
	InFlightCalls = MoveTemp(InInFlightCalls);
	if (Env.IdleSeconds <= 0 || IsRunningCommandlet())
		return;

	IdleTimer.Emplace(static_cast<double>(Env.IdleSeconds), FPlatformTime::Seconds());
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &FUnrealMcpEditorSession::Tick), /*InDelay*/ 1.0f);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] idle shutdown armed: the editor saves and quits after %ds without tool calls (owner=%s)."),
		Env.IdleSeconds, Env.Owner.IsEmpty() ? TEXT("-") : *Env.Owner);
}

bool FUnrealMcpEditorSession::Tick(float /*DeltaTime*/)
{
	if (!IdleTimer.IsSet())
		return false;

	const bool bBusy = (InFlightCalls && InFlightCalls() > 0)
		|| (GEditor != nullptr && GEditor->IsPlaySessionInProgress())
		|| FAssetCompilingManager::Get().GetNumRemainingAssets() > 0;
	// Someone working in a CLI-started window keeps it open (Slate times use FPlatformTime::Seconds too).
	const double LastUserInput = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetLastUserInteractionTime() : 0.0;
	if (!IdleTimer->ShouldQuit(FPlatformTime::Seconds(), ToolActivity ? ToolActivity() : 0, bBusy, LastUserInput))
		return true;

	IdleTimer.Reset();
	RequestQuit(FString::Printf(TEXT("no tool calls for %ds"), Env.IdleSeconds));
	TickHandle.Reset(); // returning false removes the ticker
	return false;
}

void FUnrealMcpEditorSession::RequestQuit(const FString& Reason)
{
	if (bDisarmed || QuitHandle.IsValid())
		return;
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] quitting the editor (%s): saving dirty packages that have a path."), *Reason);
	QuitHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		SaveDirtyPackagesWithPath();
		// Not forced: the normal exit path runs (OnEnginePreExit → the coordinator stops the sidecar and the local server).
		FPlatformMisc::RequestExit(/*Force*/ false, TEXT("UnrealMcp.EditorSession"));
		return false;
	}), /*InDelay*/ 0.25f);
}

void FUnrealMcpEditorSession::Disarm()
{
	bDisarmed = true;
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (QuitHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(QuitHandle);
		QuitHandle.Reset();
	}
	IdleTimer.Reset();
	ToolActivity = nullptr;
	InFlightCalls = nullptr;
}

void FUnrealMcpEditorSession::Shutdown()
{
	Disarm();
	if (bLeaseReleased || !Env.IsCliOwned())
		return;
	bLeaseReleased = true;
	if (RemoveLeaseIfOwned(LeasePath(ProjectDir), Env.Nonce))
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] removed the CLI session lease."));
}

void FUnrealMcpEditorSession::RegisterTools(FUnrealMcpToolRegistry& Registry)
{
	TWeakPtr<FUnrealMcpEditorSession> WeakSession = AsShared();
	Registry.Tool(QuitToolName)
		.Title(TEXT("Quit editor session"))
		.Description(TEXT("Save dirty packages that have a path and quit an editor started by a CLI session "
		                  "(`unreal-mcp-cli close --owned-only`). Refuses unless 'nonce' matches the session lease."))
		.ParamString(TEXT("nonce"), TEXT("The nonce of the CLI session lease (.ai-game-dev/session.json)."), EUnrealMcpParamRequirement::Required)
		.ToolType(EUnrealMcpToolType::System)
		.DestructiveHint(true)
		.Handle([WeakSession](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
		{
			const TSharedPtr<FUnrealMcpEditorSession> Session = WeakSession.Pin();
			if (!Session.IsValid())
				return FUnrealMcpToolResult::Error(TEXT("The editor is already shutting down."));
			if (!Session->Env.IsCliOwned())
				return FUnrealMcpToolResult::Error(TEXT("NotOwned: this editor was not started by a CLI session; close it yourself."));
			if (Call.GetString(TEXT("nonce")) != Session->Env.Nonce)
				return FUnrealMcpToolResult::Error(TEXT("NotOwned: the nonce does not match this editor's session."));
			if (Session->bDisarmed)
				return FUnrealMcpToolResult::Error(TEXT("The editor is already shutting down."));
			Session->RequestQuit(TEXT("requested by the CLI"));
			return FUnrealMcpToolResult::Success(TEXT("Quitting: saving dirty packages that have a path, then exiting."));
		});
}

FString FUnrealMcpEditorSession::LeasePath(const FString& InProjectDir)
{
	return FPaths::Combine(InProjectDir, TEXT(".ai-game-dev"), TEXT("session.json"));
}

bool FUnrealMcpEditorSession::RemoveLeaseIfOwned(const FString& Path, const FString& Nonce)
{
	if (Nonce.IsEmpty())
		return false;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
		return false;
	TSharedPtr<FJsonObject> Lease;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Lease) || !Lease.IsValid())
		return false;
	FString LeaseNonce;
	if (!Lease->TryGetStringField(TEXT("nonce"), LeaseNonce) || LeaseNonce != Nonce)
		return false; // another session's lease (a newer editor) — never ours to remove
	return IFileManager::Get().Delete(*Path, /*RequireExists*/ true, /*EvenReadOnly*/ false, /*Quiet*/ true);
}

bool FUnrealMcpEditorSession::HasSavablePath(const FString& PackageName)
{
	return FPackageName::IsValidLongPackageName(PackageName)
		&& !FPackageName::IsTempPackage(PackageName)
		&& !FPackageName::IsMemoryPackage(PackageName)
		&& !PackageName.StartsWith(TEXT("/Engine/Transient"));
}

void FUnrealMcpEditorSession::SaveDirtyPackagesWithPath()
{
	TArray<UPackage*> Dirty;
	FEditorFileUtils::GetDirtyWorldPackages(Dirty);
	FEditorFileUtils::GetDirtyContentPackages(Dirty);

	TArray<UPackage*> ToSave;
	for (UPackage* Package : Dirty)
	{
		if (Package != nullptr && !Package->HasAnyPackageFlags(PKG_PlayInEditor | PKG_CompiledIn)
			&& !Package->HasAnyFlags(RF_Transient) && HasSavablePath(Package->GetName()))
			ToSave.AddUnique(Package);
	}
	if (ToSave.Num() == 0)
		return;

	// The no-prompt path (what unattended scripts use): never a Save As dialog, which would block a windowless quit.
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty*/ true);
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] saved %d dirty package(s) before quitting%s."), ToSave.Num(),
		bSaved ? TEXT("") : TEXT(" (some failed: see the log above)"));
}
