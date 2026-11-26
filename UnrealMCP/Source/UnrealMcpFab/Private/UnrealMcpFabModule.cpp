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
 * creates it that way, the engine's online subsystems use named configs. With none, Epic's sign-in service never started.
 * With more than one, this module cannot tell which is Fab's and does nothing (the tools then behave as without it).
 */
namespace UnrealMcpFabLocal
{
	/** The longest a restore counts as running: a call that waits on it never waits longer. */
	static constexpr double RestoreWaitSeconds = 30.0;
	static double RestoreStartedAt = -1.0e9;

#if WITH_EOS_SDK
	/** The platforms that could be Fab's: those without a config name. */
	static TArray<EOS_HPlatform> FabCandidatePlatforms(IEOSSDKManager& Sdk)
	{
		TArray<EOS_HPlatform> Candidates;
		for (const IEOSPlatformHandlePtr& Platform : Sdk.GetActivePlatforms())
		{
			if (Platform.IsValid() && Platform->GetConfigName().IsEmpty())
				Candidates.Add(*Platform);
		}
		return Candidates;
	}

	/** Fab's EOS platform, or null (no SDK, no platform, or more than one candidate). */
	static EOS_HPlatform FabPlatform()
	{
		IEOSSDKManager* Sdk = IEOSSDKManager::Get();
		if (!Sdk || !Sdk->IsInitialized())
			return nullptr;
		const TArray<EOS_HPlatform> Candidates = FabCandidatePlatforms(*Sdk);
		return Candidates.Num() == 1 ? Candidates[0] : nullptr;
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

	static EUnrealMcpFabSignInService FabService()
	{
		IEOSSDKManager* Sdk = IEOSSDKManager::Get();
		if (!Sdk || !Sdk->IsInitialized())
			return EUnrealMcpFabSignInService::NotStarted;
		const int32 Candidates = FabCandidatePlatforms(*Sdk).Num();
		if (Candidates == 0)
			return EUnrealMcpFabSignInService::NotStarted;
		if (Candidates > 1)
			return EUnrealMcpFabSignInService::Unknown;
		return FPlatformTime::Seconds() < RestoreStartedAt + RestoreWaitSeconds ? EUnrealMcpFabSignInService::Restoring
			: EUnrealMcpFabSignInService::Running;
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

	static void EOS_CALL FabRestoreDone(const EOS_Auth_LoginCallbackInfo* Data)
	{
		RestoreStartedAt = -1.0e9;
		if (Data->ResultCode == EOS_EResult::EOS_Success)
		{
			UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: restored the Epic sign-in remembered on this computer (no browser)."));
		}
		else
		{
			UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: no remembered Epic sign-in was restored (%s); fab-sign-in signs the user in."),
				UTF8_TO_TCHAR(EOS_EResult_ToString(Data->ResultCode)));
		}
	}

	/** On editor start: sign in on Fab's platform from the remembered sign-in, unless someone is signed in there already. */
	static void FabRestoreSignIn()
	{
		IEOSSDKManager* Sdk = IEOSSDKManager::Get();
		if (!Sdk || !Sdk->IsInitialized())
		{
			UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: Epic's sign-in service did not start in this editor (no EOS SDK); fab-sign-in cannot sign in."));
			return;
		}
		const TArray<EOS_HPlatform> Candidates = FabCandidatePlatforms(*Sdk);
		if (Candidates.Num() != 1)
		{
			UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Fab: %s, so the remembered Fab sign-in is not restored."),
				Candidates.IsEmpty() ? TEXT("the Fab plugin started no EOS platform") : TEXT("more than one EOS platform could be the Fab plugin's"));
			return;
		}
		const EOS_HAuth Auth = EOS_Platform_GetAuthInterface(Candidates[0]);
		if (!Auth || FabSignedInAccount(Auth))
			return;
		EOS_Auth_Credentials Credentials = {};
		Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
		Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_PersistentAuth;
		EOS_Auth_LoginOptions Options = {};
		Options.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
		Options.Credentials = &Credentials;
		RestoreStartedAt = FPlatformTime::Seconds();
		EOS_Auth_Login(Auth, &Options, nullptr, &FabRestoreDone);
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
			UnrealMcpFabLocal::FabRestoreSignIn();
		else
			PostEngineInit = FCoreDelegates::OnPostEngineInit.AddStatic(&UnrealMcpFabLocal::FabRestoreSignIn);
#endif
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInit);
		UnrealMcpFabSignIn() = FUnrealMcpFabSignIn();
	}

private:
	FDelegateHandle PostEngineInit;
};

IMPLEMENT_MODULE(FUnrealMcpFabModule, UnrealMcpFab)
