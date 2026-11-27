// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "UnrealMcpFabSignIn.h"
#include "UnrealMcpLog.h"

#if WITH_EOS_SDK
#include "IEOSSDKManager.h"
#include "eos_auth.h"
#include "eos_sdk.h"
#endif

/**
 * Keeps the user's Fab sign-in across editor restarts (DEV-281). The engine's Fab plugin signs in on its own EOS platform,
 * and Epic's SDK remembers that sign-in on this computer (EOS persistent auth), but the plugin restores it only when its
 * panel opens. This family never opens the panel (its web view cannot exist under -nullrhi, and the engine asserts there),
 * so on editor start this module restores the remembered sign-in on that platform itself: no panel, no browser. It reads
 * the tokens from the platform for the Fab tools, since the Fab plugin's own GetAuthToken knows only sign-ins it started.
 *
 * Fab's platform is the one EOS platform that was created from options rather than from a named config: the Fab plugin
 * creates it that way, the engine's online subsystems use named configs. Only those created while the editor starts count
 * (MetaHuman's cloud sign-in creates one later), and the one found is kept. Without an EOS SDK, Epic's sign-in service never
 * started. With no such platform or more than one, this module cannot tell (the tools then behave as without it).
 */
namespace UnrealMcpFabLocal
{
#if WITH_EOS_SDK
	/** A persistent-auth login this module started is in flight. */
	static bool bRestoring = false;
	/**
	 * The restore is tried once per editor session: on editor start, or on first use when Fab's platform came later. A try
	 * that failed only for a passing reason (FabRestoreRetryable) clears it, so a later use tries again (DEV-293).
	 */
	static bool bRestoreTried = false;
	/** No new try before this time (FPlatformTime::Seconds): a passing failure is retried, but not on every frame. */
	static double FabRestoreNotBefore = 0.0;
	static constexpr double FabRestoreRetrySeconds = 15.0;
	/** Passing failures retried so far; after FabRestoreMaxRetries the next one is final. */
	static int32 FabRestoreRetries = 0;
	static constexpr int32 FabRestoreMaxRetries = 3;

	/**
	 * A retry of a passing failure is due later (FabRestoreDone). The tools then wait as for a restore in flight: fab-search
	 * does not report a missing sign-in, and fab-sign-in opens no browser page that the retried login would collide with.
	 */
	static bool FabRestorePending()
	{
		return !bRestoreTried && FPlatformTime::Seconds() < FabRestoreNotBefore;
	}

	/** Where Fab's EOS platform stands: no SDK, no candidate, exactly one (returned) or more than one. */
	enum class EFabPlatform : uint8 { NoSdk, None, Found, Ambiguous };

	/** Fab's platform once known. It is kept: platforms other plugins create later (MetaHuman's cloud sign-in) never hide it. */
	static IEOSPlatformHandleWeakPtr FabKnownPlatform;
	/** The platforms without a config name created while the editor started (the Fab plugin creates its own then). */
	static TArray<IEOSPlatformHandleWeakPtr> FabStartupPlatforms;
	static FDelegateHandle FabPlatformCreatedHandle;

	static void FabOnPlatformCreated(const IEOSPlatformHandlePtr& Platform)
	{
		if (Platform.IsValid() && Platform->GetConfigName().IsEmpty())
			FabStartupPlatforms.Add(Platform);
	}

	/**
	 * Fab's platform: the one platform without a config name (the Fab plugin creates it from options) among those created
	 * while the editor started, else among the active ones. The first lookup runs on editor start, before other plugins
	 * create theirs on demand, and the platform it finds is kept.
	 */
	static EFabPlatform FabFindPlatform(EOS_HPlatform& OutPlatform)
	{
		OutPlatform = nullptr;
		IEOSSDKManager* Sdk = IEOSSDKManager::Get();
		if (!Sdk || !Sdk->IsInitialized())
			return EFabPlatform::NoSdk;
		if (const IEOSPlatformHandlePtr Known = FabKnownPlatform.Pin())
		{
			OutPlatform = *Known;
			return EFabPlatform::Found;
		}
		TArray<IEOSPlatformHandlePtr> Candidates;
		for (const IEOSPlatformHandleWeakPtr& Platform : FabStartupPlatforms)
		{
			if (const IEOSPlatformHandlePtr Pinned = Platform.Pin())
				Candidates.Add(Pinned);
		}
		if (Candidates.IsEmpty()) // loaded after the editor started, or no platform was announced
		{
			for (const IEOSPlatformHandlePtr& Platform : Sdk->GetActivePlatforms())
			{
				if (Platform.IsValid() && Platform->GetConfigName().IsEmpty())
					Candidates.Add(Platform);
			}
		}
		if (Candidates.Num() != 1)
			return Candidates.IsEmpty() ? EFabPlatform::None : EFabPlatform::Ambiguous;
		FabKnownPlatform = Candidates[0];
		OutPlatform = *Candidates[0];
		return EFabPlatform::Found;
	}

	/** The account signed in on @p Auth, or null. */
	static EOS_EpicAccountId FabSignedInAccount(EOS_HAuth Auth)
	{
		const int32 Count = EOS_Auth_GetLoggedInAccountsCount(Auth);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const EOS_EpicAccountId Account = EOS_Auth_GetLoggedInAccountByIndex(Auth, Index);
			if (EOS_Auth_GetLoginStatus(Auth, Account) == EOS_ELoginStatus::EOS_LS_LoggedIn)
				return Account;
		}
		return nullptr;
	}

	/**
	 * A failed restore that may succeed later with the same remembered sign-in: no network yet, a timeout, a busy or failing
	 * service. Epic's SDK keeps the remembered sign-in then. Every other failure is final: none remembered, Epic refused it, or
	 * another login holds Fab's platform (EOS_RequestInProgress: a retry would only collide with it again).
	 */
	static bool FabRestoreRetryable(EOS_EResult Result)
	{
		switch (Result)
		{
		case EOS_EResult::EOS_NoConnection:
		case EOS_EResult::EOS_NetworkDisconnected:
		case EOS_EResult::EOS_TimedOut:
		case EOS_EResult::EOS_TooManyRequests:
		case EOS_EResult::EOS_ServiceFailure:
			return true;
		default:
			return false;
		}
	}

	static void EOS_CALL FabRestoreDone(const EOS_Auth_LoginCallbackInfo* Data)
	{
		if (!EOS_EResult_IsOperationComplete(Data->ResultCode))
			return; // e.g. EOS_OperationWillRetry: the SDK calls again with the final result
		bRestoring = false;
		if (Data->ResultCode == EOS_EResult::EOS_Success)
		{
			UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: restored the Epic sign-in remembered on this computer (no browser)."));
		}
		else if (FabRestoreRetryable(Data->ResultCode) && FabRestoreRetries < FabRestoreMaxRetries)
		{
			++FabRestoreRetries;
			bRestoreTried = false;
			FabRestoreNotBefore = FPlatformTime::Seconds() + FabRestoreRetrySeconds;
			UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: the remembered Epic sign-in was not restored yet (%s); the next Fab tool call "
				"tries again."), UTF8_TO_TCHAR(EOS_EResult_ToString(Data->ResultCode)));
		}
		else
		{
			UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: no remembered Epic sign-in was restored (%s); fab-sign-in signs the user in."),
				UTF8_TO_TCHAR(EOS_EResult_ToString(Data->ResultCode)));
		}
	}

	/** Sign in on Fab's platform from the remembered sign-in, once, unless someone is signed in there already. */
	static void FabRestoreOnce(EOS_HPlatform Platform)
	{
		if (bRestoreTried || FPlatformTime::Seconds() < FabRestoreNotBefore)
			return;
		bRestoreTried = true;
		const EOS_HAuth Auth = EOS_Platform_GetAuthInterface(Platform);
		if (!Auth || FabSignedInAccount(Auth))
			return;
		UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: restoring the Epic sign-in remembered on this computer (no browser)."));
		EOS_Auth_Credentials Credentials = {};
		Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
		Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_PersistentAuth;
		EOS_Auth_LoginOptions Options = {};
		Options.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
		Options.Credentials = &Credentials;
		bRestoring = true;
		EOS_Auth_Login(Auth, &Options, nullptr, &FabRestoreDone);
	}

	/** Fab's platform, or null; the first lookup that finds it starts the restore (FabRestoreOnce). */
	static EOS_HPlatform FabPlatform()
	{
		EOS_HPlatform Platform;
		if (FabFindPlatform(Platform) == EFabPlatform::Found)
			FabRestoreOnce(Platform);
		return Platform;
	}

	/** On editor start: restore now if Fab's platform exists, else on first use (FabPlatform). */
	static void FabRestoreOnStart()
	{
		if (IEOSSDKManager* Sdk = IEOSSDKManager::Get())
			Sdk->OnPlatformCreated.Remove(FabPlatformCreatedHandle);
		EOS_HPlatform Platform;
		const EFabPlatform Found = FabFindPlatform(Platform);
		if (Found == EFabPlatform::Found)
		{
			FabRestoreOnce(Platform);
			return;
		}
		UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: %s; the remembered Fab sign-in is not restored now."),
			Found == EFabPlatform::NoSdk ? TEXT("Epic's sign-in service did not start in this editor (no EOS SDK)")
			: Found == EFabPlatform::None ? TEXT("the Fab plugin started no EOS platform (yet)")
			: TEXT("more than one EOS platform could be the Fab plugin's"));
	}

	static EUnrealMcpFabSignInService FabService()
	{
		EOS_HPlatform Platform;
		switch (FabFindPlatform(Platform))
		{
		case EFabPlatform::NoSdk:
			return EUnrealMcpFabSignInService::NotStarted;
		case EFabPlatform::Found:
			FabRestoreOnce(Platform);
			return bRestoring || FabRestorePending() ? EUnrealMcpFabSignInService::Restoring : EUnrealMcpFabSignInService::Running;
		default:
			// No candidate yet, or several: not known here (fab-sign-in's own check of the Fab plugin's Login decides).
			return EUnrealMcpFabSignInService::Unknown;
		}
	}

	static bool FabTokens(FString& OutAccessToken, FString& OutRefreshToken)
	{
		const EOS_HPlatform Platform = FabPlatform();
		const EOS_HAuth Auth = Platform ? EOS_Platform_GetAuthInterface(Platform) : nullptr;
		const EOS_EpicAccountId Account = Auth ? FabSignedInAccount(Auth) : nullptr;
		if (!Account)
			return false;
		EOS_Auth_CopyUserAuthTokenOptions Options = {};
		Options.ApiVersion = EOS_AUTH_COPYUSERAUTHTOKEN_API_LATEST;
		EOS_Auth_Token* Token = nullptr;
		if (EOS_Auth_CopyUserAuthToken(Auth, &Options, Account, &Token) != EOS_EResult::EOS_Success || !Token)
			return false;
		OutAccessToken = UTF8_TO_TCHAR(Token->AccessToken);
		OutRefreshToken = Token->RefreshToken ? UTF8_TO_TCHAR(Token->RefreshToken) : TEXT("");
		EOS_Auth_Token_Release(Token);
		return !OutAccessToken.IsEmpty();
	}
#endif
}

class FUnrealMcpFabModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if WITH_EOS_SDK
		// The Fab plugin creates its EOS platform only in an interactive editor, and later than this module loads.
		if (!GIsEditor || IsRunningCommandlet())
			return;
		FUnrealMcpFabSignIn& SignIn = UnrealMcpFabSignIn();
		SignIn.Service = &UnrealMcpFabLocal::FabService;
		SignIn.Tokens = &UnrealMcpFabLocal::FabTokens;
		if (GIsRunning)
		{
			UnrealMcpFabLocal::FabRestoreOnStart();
			return;
		}
		// Record the platforms created until the editor has started: the Fab plugin (PostDefault) creates its own after this.
		if (IEOSSDKManager* Sdk = IEOSSDKManager::Get())
		{
			UnrealMcpFabLocal::FabPlatformCreatedHandle = Sdk->OnPlatformCreated.AddStatic(&UnrealMcpFabLocal::FabOnPlatformCreated);
		}
		PostEngineInit = FCoreDelegates::OnPostEngineInit.AddStatic(&UnrealMcpFabLocal::FabRestoreOnStart);
#endif
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInit);
#if WITH_EOS_SDK
		if (IEOSSDKManager* Sdk = IEOSSDKManager::Get())
			Sdk->OnPlatformCreated.Remove(UnrealMcpFabLocal::FabPlatformCreatedHandle);
#endif
		UnrealMcpFabSignIn() = FUnrealMcpFabSignIn();
	}

private:
	FDelegateHandle PostEngineInit;
};

IMPLEMENT_MODULE(FUnrealMcpFabModule, UnrealMcpFab)
