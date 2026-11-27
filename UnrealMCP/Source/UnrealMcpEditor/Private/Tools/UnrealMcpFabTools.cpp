// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpSchema.h"
#include "Config/UnrealMcpConfig.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "UnrealMcpEditorDiagnostics.h"
#include "UnrealMcpFabSignIn.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "JsonObjectConverter.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Safety/UnrealMcpEngineContentGuard.h"
#include "Serialization/CustomVersion.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FeedbackContext.h"
#include "UObject/CoreRedirects.h"
#include "UObject/MetaData.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/ObjectVersion.h"
#include "UObject/UnrealType.h"

/**
 * The Fab tool family: `fab-search` (the public Fab catalog), `fab-library` (what the signed-in user owns), `fab-add`
 * (download a listing and import it into the project, as the Fab panel's "Add to Project" does) and `fab-sign-in` (the
 * user's Epic sign-in, the only tool that opens a browser).
 *
 * Sign-in (DEV-273): fab-library and fab-add never open Epic's sign-in page themselves. On an unattended machine (an
 * autonomous never-ask run) a browser tab nobody sees was the only sign of a missing sign-in, and the agent went on
 * without the asset. Without a sign-in they answer at once with what the agent must do: ask the user in the chat
 * (AskUserQuestion, a blocking card in the App's desktop and CLI chats), call fab-sign-in only when the user agreed,
 * then call again. fab-search says up front whether adding will need a sign-in.
 *
 * The engine's Fab plugin (Engine/Plugins/Fab, enabled by default since UE 5.5) owns sign-in (EOS) and the import
 * workflows (packs, Megascans, glTF/FBX). Its types are private to it, so this family reaches them only through UE
 * reflection: UFabBrowserApi::GetAuthToken / AddToProject (UFUNCTIONs) and UFabLocalAssets::PathsListingID (the config
 * map the workflows write when an import completes). Nothing links against the Fab module, so the editor still loads
 * when the Fab plugin is disabled; the tools then answer with an error.
 *
 * Sign-in across editor restarts (DEV-281): the optional UnrealMcpFab module (loaded only with the Fab plugin) restores
 * the sign-in Epic remembers on this computer when the editor starts and hands its tokens to this family
 * (UnrealMcpFabSignIn), and says when Epic's sign-in service never started, so fab-sign-in fails at once instead of
 * waiting for a sign-in that cannot finish.
 *
 * Handlers never block the game thread: an HTTP request or an import in flight answers RetryLater and the dispatcher
 * runs the handler again on a later frame with the same call (the per-call state is keyed by the call's arguments
 * object, which the dispatcher keeps alive across retries, and dropped once that object is gone).
 */
namespace UnrealMcpFabToolsLocal
{
	static const TCHAR* FabRoot = TEXT("https://www.fab.com");
	/** How long a call waits for the user to finish signing in on Epic's sign-in page. */
	static constexpr double FabPortalWaitSeconds = 150.0;
	/**
	 * Epic's sign-in page is not opened again for this long: a second account-portal login while the first one still
	 * waits for the user fails both (EOS_Auth_PinGrantPending).
	 */
	static constexpr double FabPortalReopenSeconds = 600.0;
	/** One fab-add call waits at most this long for the import; a later call with the same listing keeps waiting. */
	static constexpr double FabImportWaitSeconds = 240.0;
	/** fab-search waits at most this long for the remembered sign-in to be restored before it searches (DEV-293). */
	static constexpr double FabSearchRestoreWaitSeconds = 30.0;
	static constexpr int32 FabMaxCount = 48;
	/** fab-library with a filter reads at most this many pages of 100. */
	static constexpr int32 FabLibraryMaxPages = 20;
	/** Where a listing's files are read: the public listing, then the user's library (private entitlements). */
	static const TCHAR* const FabRoutes[] = { TEXT("listings"), TEXT("library/assets") };
	/** The formats the editor imports, in the order fab-add prefers them. */
	static const TCHAR* const FabImportFormats[] = { TEXT("unreal-engine"), TEXT("glb"), TEXT("gltf"), TEXT("fbx") };

	// ---------------------------------------------------------------- HTTP

	struct FFabHttpResult
	{
		bool bDone = false;
		bool bConnected = false;
		int32 Code = 0;
		FString Body;
		TArray<FString> Headers;
	};

	/** Specs only (UnrealMcpFabTools::SetOfflineForTests): every request answers at once with no response, so none reaches fab.com. */
	static bool& FabOffline()
	{
		static bool bOffline = false;
		return bOffline;
	}

	/** Start a request; its completion (on the game thread) fills the returned result. */
	static TSharedPtr<FFabHttpResult> FabHttpStart(const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers,
		const FString& Body = FString())
	{
		TSharedPtr<FFabHttpResult> Result = MakeShared<FFabHttpResult>();
		if (FabOffline())
		{
			Result->bDone = true; // answered at once as "no response": nothing leaves this computer
			return Result;
		}
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetVerb(Verb);
		Request->SetURL(Url);
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
		Request->SetHeader(TEXT("User-Agent"), FString::Printf(TEXT("UnrealEditor/%s UnrealMCP"), *FEngineVersion::Current().ToString(EVersionComponent::Patch)));
		for (const TPair<FString, FString>& Header : Headers)
			Request->SetHeader(Header.Key, Header.Value);
		if (!Body.IsEmpty())
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetContentAsString(Body);
		}
		Request->SetTimeout(60.f);
		Request->OnProcessRequestComplete().BindLambda([Result](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			Result->bConnected = bConnected && Response.IsValid();
			if (Response.IsValid())
			{
				Result->Code = Response->GetResponseCode();
				Result->Body = Response->GetContentAsString();
				Result->Headers = Response->GetAllHeaders();
			}
			Result->bDone = true;
		});
		Request->ProcessRequest();
		return Result;
	}

	/** @p Field of @p Object as a string, or empty when it is missing or not a string (GetStringField logs an error then). */
	static FString FabString(const FJsonObject& Object, const TCHAR* Field)
	{
		FString Value;
		Object.TryGetStringField(Field, Value);
		return Value;
	}

	static TSharedPtr<FJsonObject> FabParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object))
			return nullptr;
		return Object;
	}

	static bool FabHttpOk(const FFabHttpResult& Http) { return Http.bConnected && Http.Code >= 200 && Http.Code < 300; }

	/** "HTTP 403: <the first characters of the body>" for an error message (a 2xx body is never shown). */
	static FString FabDescribeHttp(const FFabHttpResult& Http)
	{
		if (!Http.bConnected)
			return TEXT("no response (network error or timeout)");
		if (FabHttpOk(Http))
			return FString::Printf(TEXT("HTTP %d with an answer this tool does not understand"), Http.Code);
		FString Body = Http.Body.Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
		if (Body.Len() > 300)
			Body = Body.Left(300) + TEXT("...");
		return FString::Printf(TEXT("HTTP %d: %s"), Http.Code, *Body);
	}


	// ---------------------------------------------------------------- per-call state (survives RetryLater)

	/**
	 * fab-add: what the project held when an import began, so that only what the import made is changed afterwards: the
	 * packages loaded then (the user may edit and save those at any time) and the material instances that existed then.
	 */
	struct FFabImportWindow
	{
		TSet<FName> LoadedBefore;
		TSet<FName> InstancesBefore;
		/** MinValue: no import began (nothing counts as made by one). */
		FDateTime StartedUtc = FDateTime::MinValue();
		/** FPlatformTime::Seconds when it was opened. */
		double OpenedSeconds = 0.0;
	};

	/**
	 * The window of each listing's import, by listing uid: an import may outlive the call that started it (the next
	 * fab-add call for the listing finds it done or still running), so the window outlives the call. Dropped when a
	 * fab-add answer uses it, when the import fails, or after FabImportWindowSeconds.
	 */
	static TMap<FString, FFabImportWindow>& FabImportWindows()
	{
		static TMap<FString, FFabImportWindow> Windows;
		return Windows;
	}
	static constexpr double FabImportWindowSeconds = 2.0 * 3600.0;

	/** One fab-* call in flight: its stage, the request it waits on and what earlier stages found. */
	struct FFabCallState
	{
		/** The call's arguments: a state whose call ended (freed arguments, maybe a new call at the same address) is dropped. */
		TWeakPtr<FJsonObject> Arguments;
		int32 Stage = 0;
		double StageStarted = 0.0;
		double LastTokenCheck = 0.0;
		bool bAskedSignIn = false;
		bool bSessionRetried = false;
		FString Token;
		FString Account;
		// fab-library: the rows found so far and the page to read next (a filter reads every page).
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<FString> Lines;
		FString Cursor;
		int32 Pages = 0;
		TSharedPtr<FFabHttpResult> Http;
		TSharedPtr<FJsonObject> Listing;
		int32 Route = 0; // index into FabRoutes
		FString Format;
		FString FileUid;
		/** fab-add: the engines the downloaded unreal-engine version lists ("5.8"); empty for other formats. */
		TArray<FString> VersionEngines;
		int64 LogSequence = 0;
		/** fab-add: the log sequence when the import started, for what the import logged (Nanite Assembly warnings). */
		int64 ImportLogStart = 0;
	};

	static TMap<const FJsonObject*, TSharedPtr<FFabCallState>>& FabCalls()
	{
		static TMap<const FJsonObject*, TSharedPtr<FFabCallState>> Calls;
		return Calls;
	}

	static FFabCallState& FabState(const FUnrealMcpToolCall& Call)
	{
		TMap<const FJsonObject*, TSharedPtr<FFabCallState>>& Calls = FabCalls();
		for (auto It = Calls.CreateIterator(); It; ++It)
		{
			if (!It.Value()->Arguments.IsValid())
				It.RemoveCurrent(); // a call that ended by its timeout or a cancel
		}
		TSharedPtr<FFabCallState>& State = Calls.FindOrAdd(Call.Arguments.Get());
		if (!State.IsValid())
		{
			State = MakeShared<FFabCallState>();
			State->Arguments = Call.Arguments;
		}
		return *State;
	}

	/** End the call: drop its state and pass the result through. */
	static FUnrealMcpToolResult FabFinish(const FUnrealMcpToolCall& Call, FUnrealMcpToolResult Result)
	{
		FabCalls().Remove(Call.Arguments.Get());
		return Result;
	}

	/**
	 * Wait on the call's request: false while it runs (answer RetryLater), true once @p Out holds the response. Starts
	 * the request on the first pass.
	 */
	static bool FabFetch(FFabCallState& State, const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers,
		FFabHttpResult& Out, const FString& Body = FString())
	{
		if (!State.Http.IsValid())
			State.Http = FabHttpStart(Verb, Url, Headers, Body);
		if (!State.Http->bDone)
			return false;
		Out = MoveTemp(*State.Http);
		State.Http.Reset();
		return true;
	}

	/** Seconds this call may still spend before its timeout, less a margin for the answer. */
	static double FabTimeLeft(const FUnrealMcpToolCall& Call)
	{
		const double Timeout = (Call.TimeoutMs > 0 ? Call.TimeoutMs : FUnrealMcpConfig::DefaultToolTimeoutMs) / 1000.0;
		const double Elapsed = Call.ReceivedSeconds > 0.0 ? FPlatformTime::Seconds() - Call.ReceivedSeconds : 0.0;
		return Timeout - Elapsed - 10.0;
	}

	// ---------------------------------------------------------------- the Fab plugin, by reflection

	static UClass* FabFindClass(const TCHAR* Name)
	{
		return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Fab.%s"), Name));
	}

	static bool FabPluginLoaded(FString& OutError)
	{
		if (FabFindClass(TEXT("FabBrowserApi")))
			return true;
		OutError = TEXT("The Fab plugin is not loaded in this editor. Enable it (Edit > Plugins > 'Fab', on by default since UE 5.5) and "
		                "restart the editor.");
		return false;
	}

	/** The plugin's API object this family calls into, rooted: its import workflows call back into it. */
	static UObject* FabApi()
	{
		static TWeakObjectPtr<UObject> Api;
		if (!Api.IsValid())
		{
			if (UClass* Class = FabFindClass(TEXT("FabBrowserApi")))
			{
				// A module reload resets the static: the rooted object (and the import workflows it owns) is still there.
				UObject* Object = FindObject<UObject>(GetTransientPackage(), TEXT("UnrealMcpFabBrowserApi"));
				if (!Object)
				{
					Object = NewObject<UObject>(GetTransientPackage(), Class, TEXT("UnrealMcpFabBrowserApi"));
					Object->AddToRoot();
				}
				Api = Object;
			}
		}
		return Api.Get();
	}

	/** Call a reflected UFUNCTION of @p Target; @p Fill sets its parameters, @p Read reads them back. */
	static bool FabInvoke(UObject* Target, const TCHAR* FunctionName, TFunctionRef<void(UFunction*, uint8*)> Fill,
		TFunctionRef<void(UFunction*, uint8*)> Read)
	{
		UFunction* Function = Target ? Target->FindFunction(FunctionName) : nullptr;
		if (!Function)
			return false;
		FStructOnScope Frame(Function); // the parameter block, initialized and destroyed with the scope
		uint8* Params = Frame.GetStructMemory();
		Fill(Function, Params);
		Target->ProcessEvent(Function, Params);
		Read(Function, Params);
		return true;
	}

	/** A reflected FString-returning UFUNCTION of the Fab plugin's API object, without arguments. */
	static FString FabApiString(const TCHAR* FunctionName)
	{
		FString Value;
		FabInvoke(FabApi(), FunctionName, [](UFunction*, uint8*) {}, [&Value](UFunction* Function, uint8* Params)
		{
			if (const FStrProperty* Return = CastField<FStrProperty>(Function->GetReturnProperty()))
				Value = Return->GetPropertyValue_InContainer(Params);
		});
		return Value;
	}

	/** What UnrealMcpFab says of Epic's sign-in service (Unknown without that module). */
	static EUnrealMcpFabSignInService FabService()
	{
		const FUnrealMcpFabSignIn& SignIn = UnrealMcpFabSignIn();
		return SignIn.Service ? SignIn.Service() : EUnrealMcpFabSignInService::Unknown;
	}

	/** The tokens of the sign-in UnrealMcpFab reads from Fab's EOS platform (a restored one included); false without one. */
	static bool FabPlatformTokens(FString& OutAccessToken, FString& OutRefreshToken)
	{
		const FUnrealMcpFabSignIn& SignIn = UnrealMcpFabSignIn();
		return SignIn.Tokens && SignIn.Tokens(OutAccessToken, OutRefreshToken);
	}

	/** The access token of a sign-in the Fab plugin itself made (UFabBrowserApi::GetAuthToken), or empty. */
	static FString FabAuthToken()
	{
		// Signed out, the read logs a LogFab and a LogEOSSDK error; the diagnostics leave out exactly those (DEV-273).
		const int64 Before = FUnrealMcpLogCollector::Get().GetLastSequence();
		FString Token = FabApiString(TEXT("GetAuthToken"));
		FUnrealMcpEditorDiagnostics::IgnoreFabTokenReadLog(Before, FUnrealMcpLogCollector::Get().GetLastSequence());
		return Token;
	}

	/** The account id inside an EOS access token ("eg1~<jwt>": the JWT's `sub`). */
	static FString FabAccountIdFromToken(const FString& Token)
	{
		FString Jwt = Token;
		int32 Tilde = INDEX_NONE;
		if (Jwt.FindChar(TEXT('~'), Tilde))
			Jwt = Jwt.Mid(Tilde + 1);
		TArray<FString> Parts;
		Jwt.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() < 2)
			return FString();
		FString Payload = Parts[1].Replace(TEXT("-"), TEXT("+")).Replace(TEXT("_"), TEXT("/"));
		while (Payload.Len() % 4 != 0)
			Payload += TEXT("=");
		FString Decoded;
		if (!FBase64::Decode(Payload, Decoded))
			return FString();
		const TSharedPtr<FJsonObject> Claims = FabParseObject(Decoded);
		FString Sub;
		if (Claims.IsValid())
			Claims->TryGetStringField(TEXT("sub"), Sub);
		return Sub;
	}

	/** The refresh token of the sign-in, for Fab's web session (POST /p/authenticate). */
	static FString FabRefreshToken()
	{
		FString Access, Refresh;
		if (FabPlatformTokens(Access, Refresh))
			return Refresh;
		return FabApiString(TEXT("GetRefreshToken"));
	}

	/** When this editor last opened Epic's sign-in page, so concurrent and repeated calls do not open a browser tab each. */
	static double& FabPortalOpenedAt()
	{
		static double OpenedAt = -1.0e9;
		return OpenedAt;
	}

	/** What fab-library / fab-add answer without a sign-in: the agent asks the user in the chat, it never opens a browser itself. */
	static FString FabSignInRequiredText(const FString& ToolName)
	{
		return FString::Printf(TEXT(
			"Not signed in to Fab in this editor, so %s did not run (no browser was opened). Adding or listing Fab assets needs the "
			"user's own Epic Games sign-in, which only the user can complete. Ask the user now with AskUserQuestion (the chat "
			"waits for the answer), offering 'Sign in to Fab now' and 'Continue without Fab assets'. If they choose to sign in, call "
			"fab-sign-in (it opens Epic's sign-in page in the browser on this computer and waits for them), then call %s again with "
			"the same arguments. If you cannot ask the user, do not call fab-sign-in: continue without the asset, and in your final "
			"answer name the Fab listings you wanted and say the user can sign in ahead of time (ask the agent to run fab-sign-in, "
			"or open Fab from the editor's toolbar) so a later run can add them."), *ToolName, *ToolName);
	}

	/** What every sign-in path answers when Epic's sign-in service never started in this editor: nothing can sign in here. */
	static FString FabServiceDownText(const FString& ToolName)
	{
		return FString::Printf(TEXT(
			"Epic's sign-in service did not start in this editor (the Fab plugin found no EOS SDK, e.g. the editor was started with "
			"-NoEOS), so no Fab sign-in can finish here and %s did not run (no browser was opened). Do not call fab-sign-in again in "
			"this editor session: continue without Fab assets, and tell the user that Fab needs an editor started normally (without "
			"-NoEOS); the editor log's LogFab lines say why the service did not start."), *ToolName);
	}

	/** fab-search's last line: what adding a listing needs, given whether this editor is signed in to Fab and can sign in. */
	static FString FabSearchFooterText(bool bSignedIn, EUnrealMcpFabSignInService Service = EUnrealMcpFabSignInService::Unknown)
	{
		if (!bSignedIn && Service == EUnrealMcpFabSignInService::NotStarted)
			return TEXT("\nEpic's sign-in service did not start in this editor (e.g. it was started with -NoEOS), so fab-add and "
			            "fab-library cannot run in it: plan without Fab assets.");
		if (!bSignedIn && Service == EUnrealMcpFabSignInService::Restoring)
			return TEXT("\nThe Fab sign-in Epic remembers on this computer is still being restored in this editor (no browser is "
			            "needed for it): call fab-search again in a moment, before you ask the user to sign in.");
		if (bSignedIn)
			return TEXT("\nfab-add imports a listing the user owns (fab-library lists them); a free listing must first be added to the "
			            "user's library on fab.com.");
		return TEXT("\nNot signed in to Fab in this editor: fab-add and fab-library need the user's Epic sign-in first, and they do "
		            "not open a browser by themselves. Before you plan on these assets, ask the user with AskUserQuestion whether to "
		            "sign in now (then call fab-sign-in); without a sign-in, plan without Fab assets. fab-add imports only a listing "
		            "the user owns; a free listing must first be added to the user's library on fab.com.");
	}

	/**
	 * The token if this editor is signed in to Fab now, else empty. UnrealMcpFab's read is silent, so it runs on every pass (a
	 * restore that just finished is seen at once); the Fab plugin's own read logs errors when it fails, so at most once a second.
	 */
	static FString FabReadToken(FFabCallState& State)
	{
		FString Refresh;
		if (State.Token.IsEmpty())
			FabPlatformTokens(State.Token, Refresh);
		const double Now = FPlatformTime::Seconds();
		if (State.Token.IsEmpty() && Now - State.LastTokenCheck >= 1.0)
		{
			State.LastTokenCheck = Now;
			State.Token = FabAuthToken();
		}
		return State.Token;
	}

	/** A sign-in page this editor opened (fab-sign-in) may still be waiting for the user. */
	static bool FabSignInPending()
	{
		return FPlatformTime::Seconds() < FabPortalOpenedAt() + FabPortalWaitSeconds;
	}

	/** Epic's sign-in page was opened less than FabPortalReopenSeconds ago: it is not opened again, it may still complete. */
	static bool FabSignInPageOpen()
	{
		return FPlatformTime::Seconds() < FabPortalOpenedAt() + FabPortalReopenSeconds;
	}

	/** What a call says once a sign-in page that is still open (FabSignInPageOpen) was not completed in time. */
	static FString FabSignInStillOpenText(const FString& ToolName)
	{
		const int32 MinutesAgo = FMath::Max(0, FMath::FloorToInt((FPlatformTime::Seconds() - FabPortalOpenedAt()) / 60.0));
		return FString::Printf(TEXT("Not signed in to Fab yet. Epic's sign-in page was opened in the browser on this computer %d "
			"minute(s) ago and is not opened again until %d minutes after that (a second sign-in while the first one waits fails "
			"both). Ask the user to finish signing in in that browser tab (or to open Fab from the editor's toolbar), then call %s "
			"again; do not call fab-sign-in again before they say they are done."), MinutesAgo,
			(int32)(FabPortalReopenSeconds / 60.0), *ToolName);
	}

	/**
	 * The user's token for a fab-library / fab-add call. Without one the call ends at once with FabSignInRequiredText,
	 * unless a sign-in fab-sign-in started is still waiting for the user: then the call waits for it too (RetryLater).
	 * Only fab-sign-in opens Epic's sign-in page. The token is read at most once a second, since every failed read logs a
	 * LogFab error.
	 */
	static bool FabEnsureToken(const FUnrealMcpToolCall& Call, FFabCallState& State, const TCHAR* ToolName, FString& OutToken,
		FUnrealMcpToolResult& OutWait)
	{
		OutToken = FabReadToken(State);
		if (!OutToken.IsEmpty())
			return true;
		const EUnrealMcpFabSignInService Service = FabService();
		if ((Service == EUnrealMcpFabSignInService::Restoring || FabSignInPending()) && FabTimeLeft(Call) > 0.0)
		{
			OutWait = FUnrealMcpToolResult::RetryLater(Service == EUnrealMcpFabSignInService::Restoring
				? TEXT("Waiting for the remembered Fab sign-in to be restored.") : TEXT("Waiting for the Fab sign-in the user started."));
			return false;
		}
		OutWait = FabFinish(Call, FUnrealMcpToolResult::Error(Service == EUnrealMcpFabSignInService::NotStarted ? FabServiceDownText(ToolName)
			: FabSignInPageOpen() ? FabSignInStillOpenText(ToolName) : FabSignInRequiredText(ToolName)));
		return false;
	}

	/** Fab's web session for @p Token (POST /p/authenticate), cached per token: the cookies the /i/ routes need. */
	struct FFabSession
	{
		FString Token;
		FString Cookie;
		FString Csrf;
	};

	static FFabSession& FabSessionCache()
	{
		static FFabSession Session;
		return Session;
	}

	static void FabReadCookies(const TArray<FString>& Headers, FFabSession& Session)
	{
		TMap<FString, FString> Cookies;
		for (const FString& Line : Headers)
		{
			FString Name, Value;
			if (!Line.Split(TEXT(":"), &Name, &Value) || !Name.TrimStartAndEnd().Equals(TEXT("Set-Cookie"), ESearchCase::IgnoreCase))
				continue;
			// A response may fold several Set-Cookie headers into one comma-joined line; cookie values here have no commas
			// but Expires does, so split only on ", <name>=".
			TArray<FString> Items;
			Value.ParseIntoArray(Items, TEXT(";"));
			for (const FString& Item : Items)
			{
				FString Trimmed = Item.TrimStartAndEnd();
				int32 Comma = INDEX_NONE;
				if (Trimmed.FindLastChar(TEXT(','), Comma))
					Trimmed = Trimmed.Mid(Comma + 1).TrimStartAndEnd();
				FString Key, Val;
				if (!Trimmed.Split(TEXT("="), &Key, &Val))
					continue;
				static const TCHAR* Attributes[] = { TEXT("expires"), TEXT("max-age"), TEXT("path"), TEXT("domain"), TEXT("samesite"),
					TEXT("secure"), TEXT("httponly") };
				bool bAttribute = false;
				for (const TCHAR* Attribute : Attributes)
					bAttribute |= Key.Equals(Attribute, ESearchCase::IgnoreCase);
				if (!bAttribute && !Key.IsEmpty())
					Cookies.Add(Key, Val);
			}
		}
		TArray<FString> Pairs;
		for (const TPair<FString, FString>& Cookie : Cookies)
		{
			Pairs.Add(Cookie.Key + TEXT("=") + Cookie.Value);
			if (Cookie.Key.Contains(TEXT("csrf"), ESearchCase::IgnoreCase))
				Session.Csrf = Cookie.Value;
		}
		Session.Cookie = FString::Join(Pairs, TEXT("; "));
	}

	static TMap<FString, FString> FabSessionHeaders()
	{
		TMap<FString, FString> Headers;
		const FFabSession& Session = FabSessionCache();
		if (!Session.Cookie.IsEmpty())
			Headers.Add(TEXT("Cookie"), Session.Cookie);
		if (!Session.Csrf.IsEmpty())
			Headers.Add(TEXT("X-CsrfToken"), Session.Csrf);
		return Headers;
	}

	/**
	 * The import records of the Fab plugin (UFabLocalAssets::PathsListingID: content path -> listing uid), written when an
	 * import completes; null when this engine's Fab plugin has no such map.
	 */
	static TOptional<FScriptMapHelper> FabImportRecords()
	{
		UClass* Class = FabFindClass(TEXT("FabLocalAssets"));
		const FMapProperty* Map = Class ? FindFProperty<FMapProperty>(Class, TEXT("PathsListingID")) : nullptr;
		if (!Map || !CastField<FStrProperty>(Map->KeyProp) || !CastField<FStrProperty>(Map->ValueProp))
			return {};
		return FScriptMapHelper(Map, Map->ContainerPtrToValuePtr<void>(Class->GetDefaultObject()));
	}

	/**
	 * Every listing imported into this project: lower-case uid -> content path. A record whose folder is gone (the user
	 * deleted it; the plugin prunes records only on its next import) does not count.
	 */
	static TMap<FString, FString> FabImportedPaths()
	{
		TMap<FString, FString> Paths;
		TOptional<FScriptMapHelper> Records = FabImportRecords();
		IAssetRegistry* Registry = IAssetRegistry::Get();
		if (!Records || !Registry)
			return Paths;
		for (FScriptMapHelper::FIterator It(*Records); It; ++It)
		{
			const FString& Path = *reinterpret_cast<const FString*>(Records->GetKeyPtr(It));
			if (Registry->PathExists(Path))
				Paths.Add(reinterpret_cast<const FString*>(Records->GetValuePtr(It))->ToLower(), Path);
		}
		return Paths;
	}

	/** Where @p ListingUid (lower case) is imported in this project, or empty. */
	static FString FabImportedPath(const FString& ListingUid)
	{
		return FabImportedPaths().FindRef(ListingUid);
	}

	/**
	 * Drop the import records of @p ListingUid before importing it again (a reimport, or a record whose folder is gone),
	 * so the record the new import writes is what tells the import is done.
	 */
	static void FabForgetImport(const FString& ListingUid)
	{
		TOptional<FScriptMapHelper> Records = FabImportRecords();
		if (!Records)
			return;
		for (int32 Index = Records->GetMaxIndex() - 1; Index >= 0; --Index)
		{
			if (Records->IsValidIndex(Index)
				&& reinterpret_cast<const FString*>(Records->GetValuePtr(Index))->Equals(ListingUid, ESearchCase::IgnoreCase))
				Records->RemoveAt(Index);
		}
	}

	/** A listing uid from a uid or a fab.com listing URL (https://www.fab.com/listings/<uid>). */
	static FString FabListingUid(FString Input)
	{
		Input.TrimStartAndEndInline();
		int32 Index = Input.Find(TEXT("/listings/"));
		if (Index != INDEX_NONE)
			Input = Input.Mid(Index + 10);
		int32 Cut = INDEX_NONE;
		if (Input.FindChar(TEXT('?'), Cut) || Input.FindChar(TEXT('/'), Cut) || Input.FindChar(TEXT('#'), Cut))
			Input = Input.Left(Cut);
		FGuid Guid;
		return FGuid::Parse(Input, Guid) ? Input.ToLower() : FString();
	}

	static FString FabListingUrl(const FString& Uid) { return FString::Printf(TEXT("%s/listings/%s"), FabRoot, *Uid); }

	static bool FabIsQuixel(const FJsonObject& Listing)
	{
		bool bQuixel = false;
		if (Listing.TryGetBoolField(TEXT("isQuixel"), bQuixel) && bQuixel)
			return true;
		const TSharedPtr<FJsonObject>* User = nullptr;
		FString Seller;
		return Listing.TryGetObjectField(TEXT("user"), User) && (*User)->TryGetStringField(TEXT("sellerName"), Seller)
			&& Seller.Contains(TEXT("Quixel"));
	}

	static TArray<FString> FabFormats(const FJsonObject& Listing)
	{
		TArray<FString> Codes;
		const TArray<TSharedPtr<FJsonValue>>* Formats = nullptr;
		if (Listing.TryGetArrayField(TEXT("assetFormats"), Formats))
		{
			for (const TSharedPtr<FJsonValue>& Format : *Formats)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				const TSharedPtr<FJsonObject>* Type = nullptr;
				FString Code;
				if (Format->TryGetObject(Object) && (*Object)->TryGetObjectField(TEXT("assetFormatType"), Type)
					&& (*Type)->TryGetStringField(TEXT("code"), Code))
					Codes.Add(Code);
			}
		}
		return Codes;
	}

	/** "UE_5.4" -> 504; 0 when unparsable. */
	static int32 FabEngineNumber(const FString& Engine)
	{
		FString Major, Minor;
		const FString Digits = Engine.Replace(TEXT("UE_"), TEXT(""));
		if (!Digits.Split(TEXT("."), &Major, &Minor))
			return 0;
		return FCString::Atoi(*Major) * 100 + FCString::Atoi(*Minor);
	}

	/** A Fab engine id as shown ("UE_5.7" -> "5.7"), or empty when unparsable. */
	static FString FabEngineLabel(const FString& Engine)
	{
		return FabEngineNumber(Engine) > 0 ? Engine.Replace(TEXT("UE_"), TEXT("")) : FString();
	}

	static TArray<TSharedPtr<FJsonValue>> FabJsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
			Array.Add(MakeShared<FJsonValueString>(Value));
		return Array;
	}

	/** This editor's engine as Fab writes it after "UE_" ("5.7"). */
	static FString FabCurrentEngine()
	{
		return FString::Printf(TEXT("%d.%d"), FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor());
	}

	/** The engines a listing's unreal-engine format says it supports (technicalSpecs.unrealEngineEngineVersions), oldest first: "5.7". */
	static TArray<FString> FabListingEngines(const FJsonObject& Listing)
	{
		TArray<FString> Engines;
		const TArray<TSharedPtr<FJsonValue>>* Formats = nullptr;
		if (!Listing.TryGetArrayField(TEXT("assetFormats"), Formats))
			return Engines;
		for (const TSharedPtr<FJsonValue>& Format : *Formats)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			const TSharedPtr<FJsonObject>* Specs = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Versions = nullptr;
			if (!Format.IsValid() || !Format->TryGetObject(Object) || !(*Object)->TryGetObjectField(TEXT("technicalSpecs"), Specs)
				|| !(*Specs)->TryGetArrayField(TEXT("unrealEngineEngineVersions"), Versions))
				continue;
			for (const TSharedPtr<FJsonValue>& Version : *Versions)
			{
				FString Engine;
				if (Version.IsValid() && Version->TryGetString(Engine) && !FabEngineLabel(Engine).IsEmpty())
					Engines.AddUnique(FabEngineLabel(Engine));
			}
		}
		Engines.Sort([](const FString& A, const FString& B) { return FabEngineNumber(A) < FabEngineNumber(B); });
		return Engines;
	}

	/** One search / library row for the structured result and the text summary. */
	static TSharedPtr<FJsonObject> FabSearchRow(const FJsonObject& Listing, FString& OutLine)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		const FString Uid = FabString(Listing, TEXT("uid"));
		const FString Title = FabString(Listing, TEXT("title"));
		const FString Type = FabString(Listing, TEXT("listingType"));
		Row->SetStringField(TEXT("uid"), Uid);
		Row->SetStringField(TEXT("title"), Title);
		Row->SetStringField(TEXT("listingType"), Type);
		Row->SetStringField(TEXT("url"), FabListingUrl(Uid));
		FString PriceText = TEXT("price unknown");
		const TSharedPtr<FJsonObject>* Price = nullptr;
		bool bFree = false;
		Listing.TryGetBoolField(TEXT("isFree"), bFree);
		double Value = 0.0;
		if (Listing.TryGetObjectField(TEXT("startingPrice"), Price) && (*Price)->TryGetNumberField(TEXT("price"), Value))
		{
			Row->SetNumberField(TEXT("price"), Value);
			bFree |= Value <= 0.0;
			PriceText = bFree ? TEXT("free") : FString::Printf(TEXT("from %.2f"), Value);
		}
		Row->SetBoolField(TEXT("free"), bFree);
		const TSharedPtr<FJsonObject>* User = nullptr;
		FString Seller;
		if (Listing.TryGetObjectField(TEXT("user"), User) && (*User)->TryGetStringField(TEXT("sellerName"), Seller))
			Row->SetStringField(TEXT("seller"), Seller);
		const TSharedPtr<FJsonObject>* Category = nullptr;
		FString CategoryName;
		if (Listing.TryGetObjectField(TEXT("category"), Category) && (*Category)->TryGetStringField(TEXT("name"), CategoryName))
			Row->SetStringField(TEXT("category"), CategoryName);
		const TArray<FString> Formats = FabFormats(Listing);
		Row->SetArrayField(TEXT("formats"), FabJsonStrings(Formats));
		// DEV-290: a pack built only for newer engines imports, then does not load; say so before the agent picks it.
		const TArray<FString> Engines = FabListingEngines(Listing);
		FString EngineText;
		if (Engines.Num())
		{
			Row->SetArrayField(TEXT("engineVersions"), FabJsonStrings(Engines));
			const bool bThisEngine = Engines.Contains(FabCurrentEngine());
			Row->SetBoolField(TEXT("supportsThisEngine"), bThisEngine);
			EngineText = FString::Printf(TEXT(" | UE %s%s"), *FString::Join(Engines, TEXT(",")),
				bThisEngine ? TEXT("") : *FString::Printf(TEXT(" (not listed for this editor's UE %s)"), *FabCurrentEngine()));
		}
		OutLine = FString::Printf(TEXT("%s | %s | %s, %s%s%s | formats: %s%s"), *Uid, *Title, *Type, *PriceText,
			CategoryName.IsEmpty() ? TEXT("") : *(TEXT(", ") + CategoryName), Seller.IsEmpty() ? TEXT("") : *(TEXT(", by ") + Seller),
			Formats.Num() ? *FString::Join(Formats, TEXT(",")) : TEXT("-"), *EngineText);
		return Row;
	}

	static FString FabQuery(const TArray<TPair<FString, FString>>& Params)
	{
		TArray<FString> Parts;
		for (const TPair<FString, FString>& Param : Params)
		{
			if (!Param.Value.IsEmpty())
				Parts.Add(Param.Key + TEXT("=") + FGenericPlatformHttp::UrlEncode(Param.Value));
		}
		return FString::Join(Parts, TEXT("&"));
	}

	/** A listing page's `results`, or false when the request failed or the answer is not such a page. */
	static bool FabResults(const FFabHttpResult& Http, TSharedPtr<FJsonObject>& OutJson, const TArray<TSharedPtr<FJsonValue>>*& OutResults)
	{
		OutJson = FabHttpOk(Http) ? FabParseObject(Http.Body) : nullptr;
		return OutJson.IsValid() && OutJson->TryGetArrayField(TEXT("results"), OutResults);
	}

	/** The page's next-page cursor (`cursors.next`), or empty on the last page. */
	static FString FabNextCursor(const FJsonObject& Page)
	{
		FString Next;
		const TSharedPtr<FJsonObject>* Cursors = nullptr;
		if (Page.TryGetObjectField(TEXT("cursors"), Cursors))
			(*Cursors)->TryGetStringField(TEXT("next"), Next);
		return Next;
	}

	/** A search / library answer: the rows as structured content, the numbered lines as text, the paging hint and @p Footer. */
	static FUnrealMcpToolResult FabPageResult(const FUnrealMcpToolCall& Call, const TArray<TSharedPtr<FJsonValue>>& Rows,
		const TArray<FString>& Lines, const FString& Next, const FString& Header, const FString& Footer)
	{
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetArrayField(TEXT("results"), Rows);
		S->SetStringField(TEXT("nextCursor"), Next);
		FString Text = Header + TEXT("\n") + FString::Join(Lines, TEXT("\n"));
		if (!Next.IsEmpty())
			Text += FString::Printf(TEXT("\nMore: call again with cursor '%s'."), *Next);
		Text += Footer;
		FUnrealMcpToolResult Result = FUnrealMcpToolResult::Success(Text, S);
		Result.bMessageCoversStructured = true;
		return FabFinish(Call, Result);
	}

	// ---------------------------------------------------------------- fab-search

	static FUnrealMcpToolResult FabSearch(const FUnrealMcpToolCall& Call)
	{
		FFabCallState& State = FabState(Call);
		static const TMap<FString, FString> Sorts = {
			{ TEXT("relevance"), TEXT("-relevance") }, { TEXT("rating"), TEXT("-ratings.averageRating") },
			{ TEXT("newest"), TEXT("-firstPublishedAt") }, { TEXT("price"), TEXT("price") }, { TEXT("-price"), TEXT("-price") } };
		const FString Sort = Call.GetString(TEXT("sort"), TEXT("relevance")).ToLower();
		if (!Sorts.Contains(Sort))
			return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("'sort' is one of: relevance, rating, newest, price, -price.")));
		const int32 Count = (int32)FMath::Clamp<int64>(Call.GetInt(TEXT("count"), 12), 1, FabMaxCount);
		// Right after the editor starts, the remembered sign-in may still be restoring: wait for it rather than report
		// signedIn:false, which makes the agent ask the user to sign in again (DEV-293). Checked before the search starts, and
		// for at most FabSearchRestoreWaitSeconds, so the search itself keeps its time (the footer then says to call again).
		if (!State.Http.IsValid() && FabReadToken(State).IsEmpty() && FabService() == EUnrealMcpFabSignInService::Restoring)
		{
			const double Now = FPlatformTime::Seconds();
			if (State.StageStarted <= 0.0)
				State.StageStarted = Now;
			if (Now - State.StageStarted < FabSearchRestoreWaitSeconds && FabTimeLeft(Call) > 0.0)
				return FUnrealMcpToolResult::RetryLater(TEXT("Waiting for the remembered Fab sign-in to be restored."));
		}
		const FString Url = FString::Printf(TEXT("%s/i/listings/search?%s"), FabRoot, *FabQuery({
			{ TEXT("q"), Call.GetString(TEXT("query")) },
			{ TEXT("listing_types"), Call.GetString(TEXT("listingType")) },
			{ TEXT("is_free"), Call.GetBool(TEXT("freeOnly")) ? TEXT("1") : TEXT("") },
			{ TEXT("sort_by"), Sorts[Sort] },
			{ TEXT("count"), FString::FromInt(Count) },
			{ TEXT("cursor"), Call.GetString(TEXT("cursor")) } }));

		FFabHttpResult Http;
		if (!FabFetch(State, TEXT("GET"), Url, {}, Http))
			return FUnrealMcpToolResult::RetryLater(TEXT("Searching Fab."));
		TSharedPtr<FJsonObject> Json;
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (!FabResults(Http, Json, Results))
			return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("Fab search failed: ") + FabDescribeHttp(Http)));

		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<FString> Lines;
		for (const TSharedPtr<FJsonValue>& Value : *Results)
		{
			const TSharedPtr<FJsonObject>* Listing = nullptr;
			if (!Value->TryGetObject(Listing))
				continue;
			FString Line;
			Rows.Add(MakeShared<FJsonValueObject>(FabSearchRow(**Listing, Line)));
			Lines.Add(FString::Printf(TEXT("%d. %s"), Lines.Num() + 1, *Line));
		}
		// Whether adding will need a sign-in first (DEV-273), said up front so the agent asks the user before it plans on a listing.
		FString PluginError;
		const bool bSignedIn = FabPluginLoaded(PluginError) && !FabReadToken(State).IsEmpty();
		FUnrealMcpToolResult Result = FabPageResult(Call, Rows, Lines, FabNextCursor(*Json),
			FString::Printf(TEXT("%d Fab listing(s) (uid | title | type, price | formats | Unreal Engine versions, when Fab lists them):"), Rows.Num()),
			PluginError.IsEmpty() ? FabSearchFooterText(bSignedIn, bSignedIn ? EUnrealMcpFabSignInService::Unknown : FabService())
			: TEXT("\n") + PluginError);
		Result.Structured->SetBoolField(TEXT("signedIn"), bSignedIn);
		return Result;
	}

	// ---------------------------------------------------------------- fab-library

	static FUnrealMcpToolResult FabLibrary(const FUnrealMcpToolCall& Call)
	{
		FString Error;
		if (!FabPluginLoaded(Error))
			return FabFinish(Call, FUnrealMcpToolResult::Error(Error));
		FFabCallState& State = FabState(Call);
		FString Token;
		FUnrealMcpToolResult Wait;
		if (!FabEnsureToken(Call, State, TEXT("fab-library"), Token, Wait))
			return Wait;
		if (State.Account.IsEmpty())
			State.Account = FabAccountIdFromToken(Token);
		if (State.Account.IsEmpty())
			return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("Could not read the Epic account id from the Fab sign-in token.")));

		// A filter searches the whole library (every page, at most FabLibraryMaxPages of 100); without one, one page.
		const FString Filter = Call.GetString(TEXT("filter")).TrimStartAndEnd();
		const int32 Count = Filter.IsEmpty() ? (int32)FMath::Clamp<int64>(Call.GetInt(TEXT("count"), 24), 1, 100) : 100;
		if (State.Pages == 0)
			State.Cursor = Call.GetString(TEXT("cursor"));
		const FString Url = FString::Printf(TEXT("%s/e/accounts/%s/ue/library?%s"), FabRoot, *State.Account, *FabQuery({
			{ TEXT("count"), FString::FromInt(Count) }, { TEXT("cursor"), State.Cursor } }));
		FFabHttpResult Http;
		if (!FabFetch(State, TEXT("GET"), Url, { { TEXT("Authorization"), TEXT("Bearer ") + Token } }, Http))
			return FUnrealMcpToolResult::RetryLater(TEXT("Reading the Fab library."));
		TSharedPtr<FJsonObject> Json;
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (!FabResults(Http, Json, Results))
			return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("Reading the Fab library failed: ") + FabDescribeHttp(Http)));

		const TMap<FString, FString> ImportedPaths = FabImportedPaths();
		TArray<TSharedPtr<FJsonValue>>& Rows = State.Rows;
		TArray<FString>& Lines = State.Lines;
		for (const TSharedPtr<FJsonValue>& Value : *Results)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Value->TryGetObject(Item))
				continue;
			const FString Title = FabString(**Item, TEXT("title"));
			if (!Filter.IsEmpty() && !Title.Contains(Filter))
				continue;
			FString ItemUrl, ListingType, Distribution, AssetId, Seller;
			(*Item)->TryGetStringField(TEXT("url"), ItemUrl);
			(*Item)->TryGetStringField(TEXT("listingType"), ListingType);
			(*Item)->TryGetStringField(TEXT("distributionMethod"), Distribution);
			(*Item)->TryGetStringField(TEXT("assetId"), AssetId);
			(*Item)->TryGetStringField(TEXT("seller"), Seller);
			FString Uid = FabListingUid(ItemUrl);
			if (Uid.IsEmpty())
				Uid = FabListingUid(AssetId);
			const FString Imported = ImportedPaths.FindRef(Uid);
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("uid"), Uid);
			Row->SetStringField(TEXT("title"), Title);
			Row->SetStringField(TEXT("listingType"), ListingType);
			Row->SetStringField(TEXT("distributionMethod"), Distribution);
			Row->SetStringField(TEXT("seller"), Seller);
			Row->SetStringField(TEXT("url"), ItemUrl);
			if (!Imported.IsEmpty())
				Row->SetStringField(TEXT("importedAt"), Imported);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Lines.Add(FString::Printf(TEXT("%d. %s | %s | %s%s%s"), Lines.Num() + 1, Uid.IsEmpty() ? TEXT("(no listing uid)") : *Uid, *Title,
				ListingType.IsEmpty() ? TEXT("?") : *ListingType, Distribution.IsEmpty() ? TEXT("") : *(TEXT(", ") + Distribution.ToLower()),
				Imported.IsEmpty() ? TEXT("") : *(TEXT(" | already in the project at ") + Imported)));
		}
		FString Next = FabNextCursor(*Json);
		if (!Filter.IsEmpty() && !Next.IsEmpty() && ++State.Pages < FabLibraryMaxPages)
		{
			State.Cursor = Next;
			return FUnrealMcpToolResult::RetryLater(TEXT("Reading the next page of the Fab library."));
		}
		if (!Filter.IsEmpty() && State.Pages < FabLibraryMaxPages)
			Next.Reset(); // the filter read every page

		return FabPageResult(Call, Rows, Lines, Next, FString::Printf(TEXT("%d item(s) of the user's Fab library%s (uid | title | type):"),
			Rows.Num(), Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" with '%s' in the title"), *Filter)),
			TEXT("\nImport one with fab-add {\"listing\": \"<uid>\"}."));
	}

	// ---------------------------------------------------------------- fab-add: will this engine load what it imported (DEV-290)

	/** At most this many names of one kind are listed in a result; the rest are counted. */
	static constexpr int32 FabShownNames = 10;
	/** An import's package files read for their versions: more than this many are left unchecked (and said so). */
	static constexpr int32 FabMaxCheckedPackages = 5000;

	/**
	 * The custom versions of a package this engine cannot load: newer than this engine's (or invalid), as "<name> <saved>
	 * (this engine: <current>)"; @p OutUnknown gets the ones this editor has no registration for (it loads them with a
	 * warning; usually a plugin that registers them is missing).
	 */
	static TArray<FString> FabCustomVersionProblems(const FCustomVersionArray& Versions, const FString& DebugName, TArray<FString>& OutUnknown)
	{
		TArray<FString> TooNew;
		for (const FCustomVersionDifference& Diff : FCurrentCustomVersions::Compare(Versions, *DebugName))
		{
			if (!Diff.Version)
				continue;
			if (Diff.Type == ECustomVersionDifference::Missing)
			{
				OutUnknown.Add(Diff.Version->Key.ToString());
				continue;
			}
			if (Diff.Type != ECustomVersionDifference::Newer && Diff.Type != ECustomVersionDifference::Invalid)
				continue;
			const TOptional<FCustomVersion> Latest = FCurrentCustomVersions::Get(Diff.Version->Key);
			const FString Name = Latest ? Latest->GetFriendlyName().ToString() : Diff.Version->Key.ToString();
			TooNew.Add(Diff.Type == ECustomVersionDifference::Invalid ? FString::Printf(TEXT("%s (invalid version %d)"), *Name, Diff.Version->Version)
				: FString::Printf(TEXT("%s %d (this engine: %d)"), *Name, Diff.Version->Version, Latest ? Latest->Version : 0));
		}
		return TooNew;
	}

	/**
	 * What may keep this engine from loading the package file @p Filename, or empty when nothing does.
	 * @p bOutNeedsNewerEngine: true only when a newer engine saved it (newer file or custom versions: this editor never loads
	 * it). Everything else is a warning with its own wording: a file that cannot be read now (locked, or held by an
	 * antivirus scan), one that is no package, one too old for this editor, custom versions this editor does not know.
	 */
	static FString FabPackageFileProblem(const FString& Filename, bool& bOutNeedsNewerEngine)
	{
		bOutNeedsNewerEngine = false;
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename, FILEREAD_Silent));
		if (!Reader.IsValid())
			return TEXT("the file could not be opened to check it (locked, or held by an antivirus scan?); not checked");
		// The versions first, read raw: the summary resets them when they are out of range, so it cannot say which way.
		int32 Tag = 0, LegacyVersion = 0, LegacyUE3Version = 0, VersionUE4 = 0, VersionUE5 = 0;
		if (Reader->TotalSize() >= 20)
		{
			*Reader << Tag << LegacyVersion;
			if (LegacyVersion != -4)
				*Reader << LegacyUE3Version;
			*Reader << VersionUE4;
			if (LegacyVersion <= -8)
				*Reader << VersionUE5;
		}
		if (Reader->IsError() || Tag != (int32)PACKAGE_FILE_TAG)
			return TEXT("not a package file (no package header; damaged or not an Unreal asset)");
		if (VersionUE4 > GPackageFileUEVersion.FileVersionUE4 || VersionUE5 > GPackageFileUEVersion.FileVersionUE5)
		{
			bOutNeedsNewerEngine = true;
			return FString::Printf(TEXT("saved by a newer engine (package version %d/%d; this engine loads up to %d/%d)"), VersionUE4, VersionUE5,
				GPackageFileUEVersion.FileVersionUE4, GPackageFileUEVersion.FileVersionUE5);
		}
		Reader->Seek(0);
		FPackageFileSummary Summary;
		*Reader << Summary;
		if (Reader->IsError())
			return TEXT("its package summary could not be read (damaged file?)");
		if (Summary.bUnversioned)
		{
			// Loaded as this engine's versions, unless the editor refuses unversioned content (then the versions stay unset).
			return Summary.IsFileVersionTooOld() ? TEXT("an unversioned (cooked) package, which this editor does not load") : FString();
		}
		if (Summary.IsFileVersionTooOld())
		{
			// The summary also resets the versions of a summary layout newer than this engine's, whose raw versions look fine.
			if (VersionUE4 >= (int32)VER_UE4_OLDEST_LOADABLE_PACKAGE)
			{
				bOutNeedsNewerEngine = true;
				return TEXT("saved by a newer engine (a package summary format this engine does not read)");
			}
			return TEXT("saved by an engine too old for this editor to load (resave it in an older engine first)");
		}
		TArray<FString> Unknown;
		const TArray<FString> TooNew = FabCustomVersionProblems(Summary.GetCustomVersionContainer().GetAllVersions(), Filename, Unknown);
		if (TooNew.Num())
		{
			bOutNeedsNewerEngine = true;
			return TEXT("saved with newer custom versions than this engine's: ") + FString::Join(TooNew, TEXT(", "));
		}
		if (Unknown.Num())
			return FString::Printf(TEXT("saved with %d custom version(s) this editor does not know (a plugin that registers them may be missing)"), Unknown.Num());
		return FString();
	}

	/** "a; b; c; and 4 more" of at most FabShownNames names. */
	static FString FabNameList(const TArray<FString>& Names)
	{
		TArray<FString> Shown(Names.GetData(), FMath::Min(Names.Num(), FabShownNames));
		FString Text = FString::Join(Shown, TEXT("; "));
		if (Names.Num() > Shown.Num())
			Text += FString::Printf(TEXT("; and %d more"), Names.Num() - Shown.Num());
		return Text;
	}

	/** Module name -> the discovered plugin that has it (FString keys compare case-insensitively); engine modules are absent. */
	using FFabModulePlugins = TMap<FString, TSharedPtr<IPlugin>>;

	static FFabModulePlugins FabModulePlugins()
	{
		FFabModulePlugins Plugins;
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
		{
			for (const FModuleDescriptor& Descriptor : Plugin->GetDescriptor().Modules)
				Plugins.FindOrAdd(Descriptor.Name.ToString(), Plugin);
		}
		return Plugins;
	}

	/**
	 * The plugin a dependency of an imported package needs: the owner of a /Script/<Module> package (from @p ModulePlugins),
	 * or the plugin whose content root a /<Plugin>/... package lies under. Null for engine and project content.
	 */
	static TSharedPtr<IPlugin> FabPluginOfDependency(const FString& Dependency, const FFabModulePlugins& ModulePlugins)
	{
		if (Dependency.StartsWith(TEXT("/Script/")))
			return ModulePlugins.FindRef(Dependency.Mid(8));
		FString Root = Dependency.Mid(1);
		int32 Slash = INDEX_NONE;
		if (Root.FindChar(TEXT('/'), Slash))
			Root.LeftInline(Slash);
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Root);
		return Plugin.IsValid() && Plugin->CanContainContent() ? Plugin : nullptr;
	}

	/** Whether a loaded static or skeletal mesh is a Nanite Assembly (NaniteSettings.NaniteAssemblyData has parts; UE 5.6+). */
	static bool FabIsNaniteAssembly(const UObject* Mesh)
	{
		const FStructProperty* Settings = Mesh ? FindFProperty<FStructProperty>(Mesh->GetClass(), TEXT("NaniteSettings")) : nullptr;
		const FStructProperty* Assembly = Settings ? FindFProperty<FStructProperty>(Settings->Struct, TEXT("NaniteAssemblyData")) : nullptr;
		const FArrayProperty* Parts = Assembly ? FindFProperty<FArrayProperty>(Assembly->Struct, TEXT("Parts")) : nullptr;
		if (!Parts)
			return false;
		const void* AssemblyData = Assembly->ContainerPtrToValuePtr<void>(Settings->ContainerPtrToValuePtr<void>(Mesh));
		return FScriptArrayHelper(Parts, Parts->ContainerPtrToValuePtr<void>(AssemblyData)).Num() > 0;
	}

	/** Known extra steps of a plugin that imported content needs, beyond enabling it. */
	static FString FabPluginNote(const FString& Plugin)
	{
		if (Plugin == TEXT("DynamicWind"))
			return TEXT("wind animates only meshes placed as an InstancedSkinnedMeshComponent with a DynamicWindData transform provider, and a "
			            "new DynamicWindData starts with Enabled=False (the meshes then stay in their reference pose): set Enabled=True.");
		return FString();
	}

	/** What the post-import checks found: the result's `requirements` object and the lines its text adds. */
	struct FFabImportReport
	{
		TSharedPtr<FJsonObject> Requirements = MakeShared<FJsonObject>();
		TArray<FString> Lines;
		TArray<FString> Unloadable;
	};

	/**
	 * Read the import's package files against this engine (file and custom versions), and from the asset registry the
	 * plugins its packages need (enabled or not), the references that do not resolve, and whether its meshes are Nanite
	 * Assemblies (they need a startup cvar). @p LogStart: the log sequence when this call's import began (0: none).
	 */
	static FFabImportReport FabCheckImport(const FString& Path, const TArray<FString>& VersionEngines, int64 LogStart)
	{
		FFabImportReport Report;
		const TSharedPtr<FJsonObject>& R = Report.Requirements;

		if (VersionEngines.Num() && !VersionEngines.Contains(FabCurrentEngine()))
		{
			R->SetArrayField(TEXT("builtFor"), FabJsonStrings(VersionEngines));
			Report.Lines.Add(FString::Printf(TEXT("Fab has no version of this pack for UE %s; the one downloaded is listed for UE %s."),
				*FabCurrentEngine(), *FString::Join(VersionEngines, TEXT(", "))));
		}

		// 1. Package files: the asset registry skips the ones this engine cannot load, so they are read from disk.
		TArray<FString> Files;
		FString Dir;
		if (FPackageName::TryConvertLongPackageNameToFilename(Path / TEXT(""), Dir))
		{
			IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.uasset"), true, false);
			IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.umap"), true, false, /*bClearFileNames*/ false);
		}
		TArray<FString> Warnings;
		const int32 Checked = FMath::Min(Files.Num(), FabMaxCheckedPackages);
		for (int32 Index = 0; Index < Checked; ++Index)
		{
			bool bNeedsNewerEngine = false;
			const FString Problem = FabPackageFileProblem(Files[Index], bNeedsNewerEngine);
			if (Problem.IsEmpty())
				continue;
			FString Package;
			if (!FPackageName::TryConvertFilenameToLongPackageName(Files[Index], Package))
				Package = FPaths::GetBaseFilename(Files[Index]);
			(bNeedsNewerEngine ? Report.Unloadable : Warnings).Add(Package + TEXT(": ") + Problem);
		}
		R->SetNumberField(TEXT("packagesChecked"), Checked);
		R->SetArrayField(TEXT("unloadablePackages"), FabJsonStrings(Report.Unloadable));
		if (Report.Unloadable.Num())
			Report.Lines.Add(FString::Printf(TEXT("%d of %d package(s) were saved by a newer engine than this editor (UE %s): the editor skips them, "
				"so they are missing from the project, its levels and its cooks: %s."), Report.Unloadable.Num(), Checked, *FabCurrentEngine(),
				*FabNameList(Report.Unloadable)));
		if (Warnings.Num())
		{
			R->SetArrayField(TEXT("packageWarnings"), FabJsonStrings(Warnings));
			Report.Lines.Add(TEXT("Package warnings: ") + FabNameList(Warnings) + TEXT("."));
		}
		if (Files.Num() > Checked)
			Report.Lines.Add(FString::Printf(TEXT("Only the first %d of %d package files were checked."), Checked, Files.Num()));
		if (Checked == 0)
		{
			// Never a silent 0 (DEV-306): an import's packages that are only in memory have no file to read yet.
			const FString Why = FString::Printf(TEXT("no package file (.uasset / .umap) is on disk under %s: packages that are only in memory "
				"(not saved yet) have no file to check"), *Path);
			R->SetStringField(TEXT("packagesNotChecked"), Why);
			Report.Lines.Add(TEXT("No package was checked against this engine: ") + Why + TEXT("."));
		}

		// 2. What the loadable packages reference: plugins (enabled or not) and references that do not resolve.
		IAssetRegistry* Registry = IAssetRegistry::Get();
		TArray<FAssetData> Assets;
		if (Registry)
			Registry->GetAssetsByPath(FName(*Path), Assets, /*bRecursive*/ true);
		TSet<FName> Packages;
		for (const FAssetData& Asset : Assets)
			Packages.Add(Asset.PackageName);
		struct FFabNeededPlugin
		{
			TSharedPtr<IPlugin> Plugin;
			FString NeededBy; // the first package that needs it
		};
		TMap<FString, FFabNeededPlugin> NeededPlugins; // by plugin name
		const FFabModulePlugins ModulePlugins = FabModulePlugins();
		TArray<FString> Unresolved;
		TSet<FName> Seen;
		for (const FName& Package : Packages)
		{
			TArray<FName> Dependencies;
			Registry->GetDependencies(Package, Dependencies);
			for (const FName& DependencyName : Dependencies)
			{
				bool bAlreadySeen = false;
				Seen.Add(DependencyName, &bAlreadySeen);
				if (bAlreadySeen)
					continue;
				const FString Dependency = DependencyName.ToString();
				const TSharedPtr<IPlugin> Plugin = FabPluginOfDependency(Dependency, ModulePlugins);
				if (Plugin.IsValid())
				{
					NeededPlugins.FindOrAdd(Plugin->GetName(), { Plugin, Package.ToString() });
					if (!Plugin->IsEnabled())
						continue; // a disabled plugin's content and classes are reported as that plugin
				}
				if (FPackageName::IsMemoryPackage(Dependency) || FPackageName::IsTempPackage(Dependency))
					continue;
				// A moved or renamed package (a PackageRedirects line) loads under its new name; one marked removed is no gap.
				const FCoreRedirectObjectName Redirected = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Package,
					FCoreRedirectObjectName(NAME_None, NAME_None, DependencyName));
				if (FCoreRedirects::IsKnownMissing(ECoreRedirectFlags::Type_Package, Redirected))
					continue;
				const FString Target = Redirected.PackageName.IsNone() ? Dependency : Redirected.PackageName.ToString();
				const bool bExists = Target.StartsWith(TEXT("/Script/")) ? FindPackage(nullptr, *Target) != nullptr
					: Packages.Contains(FName(*Target)) || FPackageName::DoesPackageExist(Target);
				if (!bExists)
					Unresolved.Add(FString::Printf(TEXT("%s (from %s)"), *Dependency, *Package.ToString()));
			}
		}
		TArray<TSharedPtr<FJsonValue>> PluginValues;
		TArray<FString> Disabled, Notes;
		NeededPlugins.KeySort([](const FString& A, const FString& B) { return A < B; });
		for (const TPair<FString, FFabNeededPlugin>& Needed : NeededPlugins)
		{
			const TSharedPtr<IPlugin>& Plugin = Needed.Value.Plugin;
			const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();
			const TCHAR* Stage = Descriptor.bIsExperimentalVersion ? TEXT("experimental") : Descriptor.bIsBetaVersion ? TEXT("beta") : nullptr;
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Needed.Key);
			Row->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
			Row->SetStringField(TEXT("neededBy"), Needed.Value.NeededBy);
			if (Stage)
				Row->SetStringField(TEXT("stage"), Stage);
			const FString Note = FabPluginNote(Needed.Key);
			if (!Note.IsEmpty())
			{
				Row->SetStringField(TEXT("note"), Note);
				Notes.Add(Needed.Key + TEXT(": ") + Note);
			}
			PluginValues.Add(MakeShared<FJsonValueObject>(Row));
			if (!Plugin->IsEnabled())
				Disabled.Add(FString::Printf(TEXT("%s%s (needed by %s)"), *Needed.Key, Stage ? *(FString(TEXT(", ")) + Stage) : TEXT(""), *Needed.Value.NeededBy));
		}
		R->SetArrayField(TEXT("plugins"), PluginValues);
		if (Disabled.Num())
			Report.Lines.Add(FString::Printf(TEXT("Needs plugin(s) that are not enabled in this project: %s. Without them these assets do not load "
				"or render correctly: ask the user before enabling them (Edit > Plugins, or the .uproject's Plugins list), then restart the "
				"editor."), *FabNameList(Disabled)));
		Report.Lines.Append(Notes);
		R->SetArrayField(TEXT("unresolvedReferences"), FabJsonStrings(Unresolved));
		if (Unresolved.Num())
			Report.Lines.Add(FString::Printf(TEXT("%d reference(s) of the imported packages point at content that is not in this project or "
				"engine (they load as missing, e.g. a blank texture): %s."), Unresolved.Num(), *FabNameList(Unresolved)));

		// 3. Startup cvars: Nanite Assemblies (Megaplants trees) build only with r.Nanite.Foliage or r.Nanite.AllowAssemblies.
		bool bAssemblies = false;
		for (const FAssetData& Asset : Assets)
			bAssemblies = bAssemblies || FabIsNaniteAssembly(Asset.FastGetAsset(/*bLoad*/ false));
		if (LogStart > 0 && !bAssemblies)
		{
			for (const FUnrealMcpLogEntry& Entry : FUnrealMcpLogCollector::Get().SnapshotAfter(LogStart))
				bAssemblies = bAssemblies || Entry.Message.Contains(TEXT("Nanite Assemblies are not enabled"));
		}
		TArray<TSharedPtr<FJsonValue>> CvarValues;
		if (bAssemblies)
		{
			bool bEnabled = false;
			TArray<FString> Values;
			for (const TCHAR* Name : { TEXT("r.Nanite.Foliage"), TEXT("r.Nanite.AllowAssemblies") })
			{
				const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
				const FString Value = Variable ? Variable->GetString() : TEXT("(not in this engine)");
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Name);
				Row->SetStringField(TEXT("value"), Value);
				Row->SetStringField(TEXT("needed"), TEXT("1 (either of the two)"));
				Row->SetStringField(TEXT("where"), TEXT("[/Script/Engine.RendererSettings] in Config/DefaultEngine.ini, read when the editor starts"));
				CvarValues.Add(MakeShared<FJsonValueObject>(Row));
				bEnabled = bEnabled || (Variable && Variable->GetInt() != 0);
				Values.Add(FString::Printf(TEXT("%s=%s"), Name, *Value));
			}
			if (!bEnabled)
				Report.Lines.Add(FString::Printf(TEXT("Its meshes are Nanite Assemblies, which this project does not build (%s): without "
					"r.Nanite.Foliage=1 or r.Nanite.AllowAssemblies=1 under [/Script/Engine.RendererSettings] in Config/DefaultEngine.ini "
					"they render as bare parts. These are startup settings: ask the user, set one, then restart the editor."),
					*FString::Join(Values, TEXT(", "))));
		}
		R->SetArrayField(TEXT("cvars"), CvarValues);
		return Report;
	}

	// ---------------------------------------------------------------- fab-add: base materials out of engine content (DEV-291)

	/** The Fab plugin's material root, and where fab-add mirrors the base materials under it into the project. */
	static const TCHAR* FabPluginMaterials = TEXT("/Fab/Materials/");
	static const TCHAR* FabProjectMaterials = TEXT("/Game/Fab/Materials/");
	/** The package metadata fab-add tags its copies with: "<source material path>|UE <major.minor>" (FabCopyTag). */
	static const TCHAR* FabCopyTagKey = TEXT("UnrealMcp.FabBaseMaterialCopyOf");

	static FFabImportWindow FabOpenImportWindow()
	{
		FFabImportWindow Window;
		// A little before now: file times may be rounded (FAT: 2 s) and the clock may step.
		Window.StartedUtc = FDateTime::UtcNow() - FTimespan::FromSeconds(2.0);
		Window.OpenedSeconds = FPlatformTime::Seconds();
		for (TObjectIterator<UPackage> It; It; ++It)
			Window.LoadedBefore.Add(It->GetFName());
		if (IAssetRegistry* Registry = IAssetRegistry::Get())
		{
			FARFilter Filter;
			Filter.PackagePaths.Add(TEXT("/Game"));
			Filter.bRecursivePaths = true;
			Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			TArray<FAssetData> Assets;
			Registry->GetAssets(Filter, Assets);
			for (const FAssetData& Asset : Assets)
				Window.InstancesBefore.Add(Asset.PackageName);
		}
		return Window;
	}

	/**
	 * Whether the import of @p Window made @p Package: a package that did not exist when the import began (Fab's
	 * Interchange / glTF importers leave theirs unsaved; a pack lands on disk), or one that existed only on disk, not
	 * loaded, and whose file the import rewrote (a pack reimport; it is loaded fresh from that file). Never a package that
	 * was loaded when the import began: the user may have edited or saved it since, and its memory may predate the file.
	 */
	static bool FabChangedByImport(const UPackage* Package, const FFabImportWindow& Window)
	{
		if (!Package || Window.StartedUtc == FDateTime::MinValue() || Window.LoadedBefore.Contains(Package->GetFName()))
			return false;
		if (!Window.InstancesBefore.Contains(Package->GetFName()))
			return true;
		if (Package->IsDirty())
			return false; // changed since it was loaded after the import began: the user's
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
			return false;
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Filename);
		return Stamp != FDateTime::MinValue() && Stamp >= Window.StartedUtc;
	}

	/** What fab-add tags its copy of @p Base with: a copy is reused only for the same material in the same engine version. */
	static FString FabCopyTag(const UMaterial* Base)
	{
		return Base->GetPathName() + TEXT("|UE ") + FabCurrentEngine();
	}

	static FString FabReadCopyTag(UObject* Object)
	{
#if WITH_METADATA
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		UMetaData* MetaData = Object->GetOutermost()->GetMetaData();
		return MetaData ? MetaData->GetValue(Object, FabCopyTagKey) : FString();
#else
		return Object->GetOutermost()->GetMetaData().GetValue(Object, FabCopyTagKey);
#endif
#else
		return FString();
#endif
	}

	static void FabWriteCopyTag(UObject* Object, const FString& Tag)
	{
#if WITH_METADATA
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		if (UMetaData* MetaData = Object->GetOutermost()->GetMetaData())
			MetaData->SetValue(Object, FabCopyTagKey, *Tag);
#else
		Object->GetOutermost()->GetMetaData().SetValue(Object, FabCopyTagKey, *Tag);
#endif
#endif
	}

	/**
	 * Save @p Package to its file without any dialog (no Save As, no source-control checkout prompt: a tool call must not
	 * open a modal window). False with @p OutError when it is not on disk afterwards, e.g. a read-only file that source
	 * control has not checked out.
	 */
	static bool FabSaveQuietly(UPackage* Package, FString& OutError)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(),
			Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
		if (IFileManager::Get().FileExists(*Filename) && IFileManager::Get().IsReadOnly(*Filename))
		{
			OutError = FString::Printf(TEXT("%s (its file is read-only: check it out of source control, then save it)"), *Package->GetName());
			return false;
		}
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.Error = GWarn;
		if (!UPackage::SavePackage(Package, Package->FindAssetInPackage(), *Filename, Args) || !IFileManager::Get().FileExists(*Filename))
		{
			OutError = FString::Printf(TEXT("%s (saving failed: the editor's log says why)"), *Package->GetName());
			return false;
		}
		return true;
	}

	/**
	 * The project path of the copy of @p Base under @p CopyRoot: only for a material of the Fab plugin's own content in an
	 * installed engine (M_MS_Base and its siblings), mirrored by its path under /Fab/Materials/ and named for this engine
	 * version, so two materials or two engines never share a copy ("/Fab/Materials/Standard/M_MS_Base" ->
	 * "/Game/Fab/Materials/Standard/M_MS_Base_UE5_7"). Empty for any other material.
	 */
	static FString FabBaseMaterialCopyPackage(const UMaterial* Base, const FString& CopyRoot)
	{
		const FString Package = Base ? Base->GetOutermost()->GetName() : FString();
		FString Filename;
		if (!Package.StartsWith(FabPluginMaterials) || !FPackageName::TryConvertLongPackageNameToFilename(Package, Filename)
			|| !FUnrealMcpEngineContentGuard::IsInstalledEngineFile(Filename))
			return FString();
		return CopyRoot + Package.Mid(FCString::Strlen(FabPluginMaterials)) + TEXT("_UE") + FabCurrentEngine().Replace(TEXT("."), TEXT("_"));
	}

	/**
	 * The project copy of the Fab plugin material @p Base at @p PackageName, on disk: the copy fab-add made of @p Base for
	 * this engine version (its FabCopyTag), else a new one (Nanite and instanced usages set, tagged, saved). Null with
	 * @p OutWarning when anything else is at that path (never reused, never replaced) or the copy could not be saved.
	 */
	static UMaterial* FabProjectBaseMaterial(UMaterial* Base, const FString& PackageName, FString& OutWarning)
	{
		const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		const bool bOnDisk = FPackageName::DoesPackageExist(PackageName);
		UPackage* Package = FindPackage(nullptr, *PackageName);
		FString Error;
		if (Package || bOnDisk)
		{
			UMaterial* Existing = FindObject<UMaterial>(nullptr, *ObjectPath);
			if (!Existing && !Package)
				Existing = LoadObject<UMaterial>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Existing || FabReadCopyTag(Existing) != FabCopyTag(Base))
			{
				OutWarning = FString::Printf(TEXT("%s already exists and is not the copy fab-add makes of %s for UE %s: it is left as it is, "
					"and the instances keep their engine base material"), *PackageName, *Base->GetPathName(), *FabCurrentEngine());
				return nullptr;
			}
			// Saved only when it never reached disk (an earlier save failed): unsaved changes to a copy on disk stay the user's.
			if (!bOnDisk && !FabSaveQuietly(Existing->GetOutermost(), Error))
			{
				OutWarning = TEXT("the base material copy could not be saved, so no instance was moved to it: ") + Error;
				return nullptr;
			}
			return Existing;
		}
		Package = CreatePackage(*PackageName);
		UMaterial* Copy = DuplicateObject<UMaterial>(Base, Package, *FPackageName::GetShortName(PackageName));
		if (!Copy)
		{
			OutWarning = FString::Printf(TEXT("%s could not be copied to %s"), *Base->GetPathName(), *PackageName);
			return nullptr;
		}
		Copy->SetFlags(RF_Public | RF_Standalone);
		// The usages PCG / Nanite / foliage set at first use, set up front: using the copy never dirties anything read-only.
		Copy->bUsedWithNanite = true;
		Copy->bUsedWithInstancedStaticMeshes = true;
		Copy->PostEditChange();
		FabWriteCopyTag(Copy, FabCopyTag(Base));
		FAssetRegistryModule::AssetCreated(Copy);
		Package->MarkPackageDirty();
		if (!FabSaveQuietly(Package, Error))
		{
			OutWarning = TEXT("the base material copy could not be saved, so no instance was moved to it: ") + Error;
			return nullptr;
		}
		return Copy;
	}

	/**
	 * Reparent the material instances under @p Path that the import of @p Window made (FabChangedByImport: never one
	 * that was loaded when it began, so never the user's edits) whose parent is a material of the Fab plugin's
	 * content in an installed engine (M_MS_Base under Program Files) to a project copy of it under @p CopyRoot with its
	 * Nanite / instanced usages already set: a PCG or Nanite use would otherwise set them on the engine material, which
	 * cannot be saved, and leave it dirty (that blocked an editor restart). An instance the import left unsaved stays
	 * unsaved (it is saved with the rest of the import); one the import wrote to disk is saved again, without a dialog.
	 * Returns "<copy> (<n> instance(s))" per copy used; @p OutWarnings what was left as it was, and why.
	 */
	static TArray<FString> FabLocalizeBaseMaterials(const FString& Path, const FFabImportWindow& Window, const FString& CopyRoot,
		TArray<FString>& OutWarnings)
	{
		TArray<FString> Done;
		IAssetRegistry* Registry = IAssetRegistry::Get();
		if (!Registry)
			return Done;
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Path));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Assets;
		Registry->GetAssets(Filter, Assets);
		TMap<UMaterial*, TArray<UMaterialInstanceConstant*>> ByBase;
		for (const FAssetData& Asset : Assets)
		{
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset.GetAsset());
			UMaterial* Base = Instance ? Cast<UMaterial>(Instance->Parent) : nullptr;
			if (!FabBaseMaterialCopyPackage(Base, CopyRoot).IsEmpty() && FabChangedByImport(Instance->GetOutermost(), Window))
				ByBase.FindOrAdd(Base).Add(Instance);
		}
		TArray<UPackage*> ToSave;
		for (const TPair<UMaterial*, TArray<UMaterialInstanceConstant*>>& Pair : ByBase)
		{
			FString Warning;
			UMaterial* Copy = FabProjectBaseMaterial(Pair.Key, FabBaseMaterialCopyPackage(Pair.Key, CopyRoot), Warning);
			if (!Copy)
			{
				OutWarnings.Add(Warning);
				continue;
			}
			for (UMaterialInstanceConstant* Instance : Pair.Value)
			{
				const bool bWasSaved = !Instance->GetOutermost()->IsDirty();
				Instance->SetParentEditorOnly(Copy);
				Instance->PostEditChange();
				Instance->MarkPackageDirty();
				if (bWasSaved)
					ToSave.Add(Instance->GetOutermost());
			}
			Done.Add(FString::Printf(TEXT("%s (%d instance(s))"), *Copy->GetPathName(), Pair.Value.Num()));
		}
		TArray<FString> NotSaved;
		for (UPackage* Package : ToSave)
		{
			FString Error;
			if (!FabSaveQuietly(Package, Error))
				NotSaved.Add(Error);
		}
		if (NotSaved.Num())
			OutWarnings.Add(TEXT("these reparented instances are not saved yet: ") + FabNameList(NotSaved));
		return Done;
	}

	// ---------------------------------------------------------------- fab-add: save what the import left unsaved (DEV-306)

	/** Whether the loaded asset package @p Package has never been written to disk (only in memory). */
	static bool FabOnlyInMemory(const UPackage* Package)
	{
		return Package->FindAssetInPackage() && !FPackageName::DoesPackageExist(Package->GetName());
	}

	/**
	 * The asset packages under the content folder @p Path loaded in this editor that are not saved: with unsaved changes,
	 * or only in memory (an import may leave a package it created unmarked, yet never written).
	 */
	static TArray<UPackage*> FabUnsavedPackagesUnder(const FString& Path)
	{
		TArray<UPackage*> Unsaved;
		const FString Prefix = Path / TEXT("");
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->GetName().StartsWith(Prefix) && It->FindAssetInPackage() && (It->IsDirty() || FabOnlyInMemory(*It)))
				Unsaved.Add(*It);
		}
		Unsaved.Sort([](const UPackage& A, const UPackage& B) { return A.GetName() < B.GetName(); });
		return Unsaved;
	}

	/**
	 * Save the packages under @p Path that the import of @p Window created and left only in memory (Fab's Interchange /
	 * Megascans glTF importers do, dirty or not: an editor crash or close then loses the whole import), without any dialog.
	 * Only a package that was not loaded when the import began and has never been on disk: never one loaded before it (the
	 * user's, maybe with unsaved edits of their own; FabChangedByImport) and never a file on disk with unsaved changes.
	 * Returns the packages saved; @p OutErrors the ones that could not be.
	 */
	static TArray<FString> FabSaveImport(const FString& Path, const FFabImportWindow& Window, TArray<FString>& OutErrors)
	{
		TArray<FString> Saved;
		if (Window.StartedUtc == FDateTime::MinValue())
			return Saved;
		for (UPackage* Package : FabUnsavedPackagesUnder(Path))
		{
			if (Window.LoadedBefore.Contains(Package->GetFName()) || !FabOnlyInMemory(Package))
				continue;
			FString Error;
			if (FabSaveQuietly(Package, Error))
				Saved.Add(Package->GetName());
			else
				OutErrors.Add(Error);
		}
		return Saved;
	}

	// ---------------------------------------------------------------- fab-add

	enum EFabAddStage : int32
	{
		AddDetails = 0,
		AddSignIn,
		AddSession,
		AddFormat,
		AddDownloadInfo,
		AddImport,
	};

	/** The uid of a file of a Megascans glTF format by quality (names end in _<quality>.zip). */
	static FString FabQuixelFile(const TArray<TSharedPtr<FJsonValue>>& Files, const FString& Quality)
	{
		TArray<FString> Suffixes;
		if (Quality == TEXT("low")) Suffixes = { TEXT("_low.zip") };
		else if (Quality == TEXT("high")) Suffixes = { TEXT("_high.zip") };
		else if (Quality == TEXT("raw")) Suffixes = { TEXT("_raw.zip"), TEXT("_high.zip") };
		else Suffixes = { TEXT("_mid.zip"), TEXT("_medium.zip") };
		for (const FString& Suffix : Suffixes)
		{
			for (const TSharedPtr<FJsonValue>& File : Files)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (File->TryGetObject(Object) && FabString(**Object, TEXT("name")).EndsWith(Suffix, ESearchCase::IgnoreCase))
					return FabString(**Object, TEXT("uid"));
			}
		}
		return FString();
	}

	/** The distribution method of an unreal-engine format ("asset_pack", "complete_project", "code_plugin"), lower case. */
	static FString FabDistributionMethod(const FJsonObject& Format)
	{
		FString Method;
		Format.TryGetStringField(TEXT("distributionMethod"), Method);
		const TSharedPtr<FJsonObject>* Details = nullptr;
		if (Method.IsEmpty() && Format.TryGetObjectField(TEXT("techDetails"), Details))
		{
			// One sub-object per listing category (threeDModel, toolAndPlugin, tutorialsExamples, ...), the others null.
			for (const auto& Category : (*Details)->Values)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Category.Value.IsValid() && Category.Value->TryGetObject(Object) && (*Object)->TryGetStringField(TEXT("distributionMethod"), Method))
					break;
			}
		}
		return Method.ToLower();
	}

	/**
	 * The unreal-engine version to download: one built for this editor's UE, else the newest one built for an older UE
	 * (content opens in newer editors), else the oldest newer one. @p OutEngines: the engines the chosen one lists ("5.8").
	 */
	static FString FabUnrealVersionUid(const TArray<TSharedPtr<FJsonValue>>& Versions, TArray<FString>* OutEngines = nullptr)
	{
		const int32 Mine = FabEngineNumber(FabCurrentEngine());
		FString Best;
		int32 BestScore = MIN_int32;
		for (const TSharedPtr<FJsonValue>& Version : Versions)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Engines = nullptr;
			if (!Version->TryGetObject(Object) || !(*Object)->TryGetArrayField(TEXT("engineVersions"), Engines))
				continue;
			for (const TSharedPtr<FJsonValue>& Engine : *Engines)
			{
				const int32 Number = FabEngineNumber(Engine->AsString());
				// Exact match first, then the closest older engine, then the closest newer one.
				const int32 Score = Number == Mine ? MAX_int32 : Number < Mine ? Number : -Number;
				if (Score > BestScore)
				{
					BestScore = Score;
					Best = FabString(**Object, TEXT("uid"));
					if (OutEngines)
					{
						OutEngines->Reset();
						for (const TSharedPtr<FJsonValue>& Listed : *Engines)
						{
							const FString Label = FabEngineLabel(Listed->AsString());
							if (!Label.IsEmpty())
								OutEngines->Add(Label);
						}
					}
				}
			}
		}
		return Best;
	}

	/**
	 * After every route answered 401 / 403: drop the cached fab.com session (its cookies may have expired while the EOS
	 * token did not) and go back to opening one, once per call. False when this call already did, or the answer was
	 * something else.
	 */
	static bool FabRenewSession(FFabCallState& State, const FFabHttpResult& Http)
	{
		if (State.bSessionRetried || (Http.Code != 401 && Http.Code != 403))
			return false;
		FabSessionCache() = FFabSession();
		State.bSessionRetried = true;
		State.Route = 0;
		State.Stage = AddSession;
		return true;
	}

	/**
	 * fab-add's answer once the listing is in the project at @p Path: the post-import checks (DEV-290: packages this
	 * engine cannot load, plugins, startup cvars, unresolved references) and the base-material copy (DEV-291). Packages
	 * this engine cannot load make it an error: the import landed, but the editor skips them.
	 */
	static FUnrealMcpToolResult FabAddDone(const FUnrealMcpToolCall& Call, const FFabCallState& State, const FString& Uid, const FString& Title,
		const FString& Path, bool bAlready)
	{
		const FString Name = Title.IsEmpty() ? Uid : Title;
		// Only for what a fab-add import made (its window: this call's, or an earlier call's whose import outlived it; a
		// listing found already in the project without one is only located), and not for a pack this editor cannot use.
		// Saved first (DEV-306), so the package check below reads what the import made.
		const FFabImportWindow* Window = FabImportWindows().Find(Uid);
		TArray<FString> SaveErrors;
		const TArray<FString> Saved = Window ? FabSaveImport(Path, *Window, SaveErrors) : TArray<FString>();
		const FFabImportReport Report = FabCheckImport(Path, State.VersionEngines, bAlready ? 0 : State.ImportLogStart);
		TArray<FString> Lines = Report.Lines;
		TArray<FString> BaseMaterialWarnings;
		TArray<FString> BaseMaterials;
		if (Window)
		{
			if (Report.Unloadable.IsEmpty() && Call.GetBool(TEXT("localBaseMaterial"), true))
				BaseMaterials = FabLocalizeBaseMaterials(Path, *Window, FabProjectMaterials, BaseMaterialWarnings);
			FabImportWindows().Remove(Uid);
		}
		Report.Requirements->SetArrayField(TEXT("savedPackages"), FabJsonStrings(Saved));
		if (Saved.Num())
			Lines.Add(FString::Printf(TEXT("Saved the %d package(s) the import created and left unsaved."), Saved.Num()));
		TArray<FString> Unsaved;
		for (const UPackage* Package : FabUnsavedPackagesUnder(Path))
			Unsaved.Add(Package->GetName());
		Report.Requirements->SetArrayField(TEXT("unsavedPackages"), FabJsonStrings(Unsaved));
		if (SaveErrors.Num())
			Lines.Add(TEXT("Could not save these packages of the import: ") + FabNameList(SaveErrors) + TEXT("."));
		if (Unsaved.Num())
		{
			// Never "save all": what fab-add did not save may be the user's own unsaved work (DEV-291's import window).
			Lines.Add(FString::Printf(TEXT("%d package(s) under %s are not saved (lost if the editor closes or crashes): %s. fab-add saves only "
				"the packages its own import created; save the ones to keep, one by one, with rql-mutate `editor | call SaveAsset(\"<package>\")`."),
				Unsaved.Num(), *Path, *FabNameList(Unsaved)));
		}
		if (BaseMaterialWarnings.Num())
		{
			Report.Requirements->SetArrayField(TEXT("baseMaterialWarnings"), FabJsonStrings(BaseMaterialWarnings));
			Lines.Add(TEXT("Base material copy: ") + FabNameList(BaseMaterialWarnings) + TEXT("."));
		}
		if (BaseMaterials.Num())
		{
			Report.Requirements->SetArrayField(TEXT("baseMaterialCopies"), FabJsonStrings(BaseMaterials));
			Lines.Add(FString::Printf(TEXT("Its material instances used a base material of the Fab plugin in the installed engine (read-only), so they "
				"now use a project copy with the Nanite and instanced usages set (PCG / Nanite use then never dirties engine content): %s."),
				*FabNameList(BaseMaterials)));
		}
		const FString Details = Lines.Num() ? TEXT("\n") + FString::Join(Lines, TEXT("\n")) : FString();
		if (Report.Unloadable.Num())
		{
			return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' %s at %s, but this editor cannot use it.%s\nDo not build on "
				"this pack in this editor and do not call fab-add for it again here: it needs a newer engine. Pick another listing whose "
				"versions include UE %s (fab-search shows them), or ask the user whether to delete %s."), *Name,
				bAlready ? TEXT("is in the project") : TEXT("was imported"), *Path, *Details, *FabCurrentEngine(), *Path)));
		}
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("uid"), Uid);
		S->SetStringField(TEXT("title"), Title);
		S->SetStringField(TEXT("path"), Path);
		S->SetBoolField(TEXT("alreadyInProject"), bAlready);
		S->SetObjectField(TEXT("requirements"), Report.Requirements);
		const FString Text = FString::Printf(TEXT("%s '%s' %s at %s. List what it holds: rql-query `assets | where Path startswith \"%s\" | "
			"select Name, Class, Path`.%s"), bAlready ? TEXT("Already in the project:") : TEXT("Imported"), *Name,
			bAlready ? TEXT("is") : TEXT("into the project"), *Path, *Path, *Details);
		return FabFinish(Call, FUnrealMcpToolResult::Success(Text, S));
	}

	/**
	 * The Fab plugin's errors about this import (LogFab lines naming the listing) logged since @p InOutSequence, as one
	 * message; advances @p InOutSequence so the next pass reads only newer lines.
	 */
	static FString FabImportErrors(int64& InOutSequence, const FString& Uid, const FString& Title)
	{
		static const FName LogFab(TEXT("LogFab"));
		TArray<FString> Errors;
		for (const FUnrealMcpLogEntry& Entry : FUnrealMcpLogCollector::Get().SnapshotAfter(InOutSequence))
		{
			InOutSequence = FMath::Max(InOutSequence, Entry.Sequence);
			if (Entry.Category == LogFab && Entry.Verbosity <= ELogVerbosity::Error
				&& (Entry.Message.Contains(Uid) || (!Title.IsEmpty() && Entry.Message.Contains(Title))))
				Errors.Add(Entry.Message.TrimStartAndEnd());
		}
		return FString::Join(Errors, TEXT("; "));
	}

	static FUnrealMcpToolResult FabAdd(const FUnrealMcpToolCall& Call)
	{
		FString Error;
		if (!FabPluginLoaded(Error))
			return FabFinish(Call, FUnrealMcpToolResult::Error(Error));
		const FString Uid = FabListingUid(Call.GetString(TEXT("listing")));
		if (Uid.IsEmpty())
			return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("'listing' must be a Fab listing uid or a https://www.fab.com/listings/<uid> URL.")));
		const FString Quality = Call.GetString(TEXT("quality"), TEXT("medium")).ToLower();
		if (Quality != TEXT("low") && Quality != TEXT("medium") && Quality != TEXT("high") && Quality != TEXT("raw"))
			return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("'quality' is one of: low, medium, high, raw.")));

		FFabCallState& State = FabState(Call);
		FString Title = State.Listing.IsValid() ? FabString(*State.Listing, TEXT("title")) : FString();

		if (State.Stage == AddDetails)
		{
			// Already in the project: no second download. An import still running from an earlier call is not tracked
			// here: AddToProject below ignores a listing whose import workflow is still running, and this call waits for it.
			if (!Call.GetBool(TEXT("reimport")))
			{
				const FString Existing = FabImportedPath(Uid);
				if (!Existing.IsEmpty())
					return FabAddDone(Call, State, Uid, Title, Existing, true);
			}
			{
				FFabHttpResult Http;
				if (!FabFetch(State, TEXT("GET"), FString::Printf(TEXT("%s/i/listings/%s"), FabRoot, *Uid), {}, Http))
					return FUnrealMcpToolResult::RetryLater(TEXT("Reading the Fab listing."));
				State.Listing = FabHttpOk(Http) ? FabParseObject(Http.Body) : nullptr;
				if (!State.Listing.IsValid())
					return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("Fab listing %s not found: %s"), *Uid, *FabDescribeHttp(Http))));
				Title = FabString(*State.Listing, TEXT("title"));
				const TArray<FString> Formats = FabFormats(*State.Listing);
				FString Format = Call.GetString(TEXT("format")).ToLower();
				if (Format.IsEmpty())
				{
					for (const TCHAR* Candidate : FabImportFormats)
					{
						if (Formats.Contains(Candidate))
						{
							Format = Candidate;
							break;
						}
					}
					// Megascans ship their Unreal-ready textures in the glTF zips the Fab plugin's Megascans importer reads.
					if (FabIsQuixel(*State.Listing) && Formats.Contains(TEXT("gltf")) && Format != TEXT("unreal-engine"))
						Format = TEXT("gltf");
				}
				const bool bImportable = MakeArrayView(FabImportFormats).ContainsByPredicate([&Format](const TCHAR* Candidate)
				{
					return Format == Candidate;
				});
				if (!bImportable || !Formats.Contains(Format))
					return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' has no format the editor imports (unreal-engine, glb, gltf, fbx)%s; "
						"its formats: %s."), *Title, Format.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" matching '%s'"), *Format),
						*FString::Join(Formats, TEXT(", ")))));
				State.Format = Format;
				State.Stage = AddSignIn;
			}
		}

		if (State.Stage == AddSignIn)
		{
			FString Token;
			FUnrealMcpToolResult Wait;
			if (!FabEnsureToken(Call, State, TEXT("fab-add"), Token, Wait))
				return Wait;
			FFabSession& Session = FabSessionCache();
			State.Stage = Session.Token == Token && !Session.Cookie.IsEmpty() ? AddFormat : AddSession;
		}

		if (State.Stage == AddSession)
		{
			// The Fab panel's own sign-in to fab.com (window.ue.fab.onLoginSuccessful): the EOS access token in Authorization and
			// the refresh token in the body; the answer sets the session cookies the /i/ routes need.
			FString AuthBody;
			if (!State.Http.IsValid()) // the body is needed only to start the request
			{
				TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
				Body->SetStringField(TEXT("refresh_token"), FabRefreshToken());
				AuthBody = UnrealMcpSerializeCondensed(Body);
			}
			FFabHttpResult Http;
			if (!FabFetch(State, TEXT("POST"), FString::Printf(TEXT("%s/p/authenticate"), FabRoot), { { TEXT("Authorization"), State.Token } }, Http,
				AuthBody))
				return FUnrealMcpToolResult::RetryLater(TEXT("Opening a Fab session."));
			if (!FabHttpOk(Http))
				return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("Fab refused the editor's sign-in: ") + FabDescribeHttp(Http)));
			FFabSession& Session = FabSessionCache();
			Session = FFabSession();
			Session.Token = State.Token;
			FabReadCookies(Http.Headers, Session);
			State.Stage = AddFormat;
		}

		if (State.Stage == AddFormat)
		{
			FFabHttpResult Http;
			const FString Url = FString::Printf(TEXT("%s/i/%s/%s/asset-formats/%s"), FabRoot, FabRoutes[State.Route], *Uid, *State.Format);
			if (!FabFetch(State, TEXT("GET"), Url, FabSessionHeaders(), Http))
				return FUnrealMcpToolResult::RetryLater(TEXT("Reading the listing's files."));
			const TSharedPtr<FJsonObject> Json = FabHttpOk(Http) ? FabParseObject(Http.Body) : nullptr;
			if (!Json.IsValid())
			{
				if (++State.Route < UE_ARRAY_COUNT(FabRoutes))
					return FUnrealMcpToolResult::RetryLater(TEXT("Reading the listing's files (library route)."));
				if (FabRenewSession(State, Http))
					return FUnrealMcpToolResult::RetryLater(TEXT("Renewing the Fab session."));
				return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("Could not read the %s files of '%s': %s"), *State.Format, *Title,
					*FabDescribeHttp(Http))));
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (State.Format == TEXT("unreal-engine"))
			{
				const FString Distribution = FabDistributionMethod(*Json);
				if (Distribution == TEXT("code_plugin"))
					return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is an engine code plugin: the user installs it from the "
						"Epic Games Launcher (Fab Library > Install to Engine); it is not added to a project."), *Title)));
				if (Distribution == TEXT("complete_project"))
					return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is a complete project, not content to add: the user "
						"creates it as a new project from the Epic Games Launcher (Fab Library > Create Project)."), *Title)));
				if (Json->TryGetArrayField(TEXT("versions"), Items))
					State.FileUid = FabUnrealVersionUid(*Items, &State.VersionEngines);
			}
			else if (Json->TryGetArrayField(TEXT("files"), Items))
			{
				State.FileUid = FabIsQuixel(*State.Listing) ? FabQuixelFile(*Items, Quality) : FString();
				const TSharedPtr<FJsonObject>* First = nullptr;
				if (State.FileUid.IsEmpty() && Items->Num() > 0 && (*Items)[0]->TryGetObject(First))
					State.FileUid = FabString(**First, TEXT("uid"));
			}
			if (State.FileUid.IsEmpty())
				return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' has no downloadable %s file."), *Title, *State.Format)));
			State.Stage = AddDownloadInfo;
		}

		if (State.Stage == AddDownloadInfo)
		{
			FFabHttpResult Http;
			const FString Url = FString::Printf(TEXT("%s/i/%s/%s/asset-formats/%s/files/%s/download-info"), FabRoot, FabRoutes[State.Route],
				*Uid, *State.Format, *State.FileUid);
			if (!FabFetch(State, TEXT("GET"), Url, FabSessionHeaders(), Http))
				return FUnrealMcpToolResult::RetryLater(TEXT("Requesting the download."));
			const TSharedPtr<FJsonObject> Json = FabHttpOk(Http) ? FabParseObject(Http.Body) : nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Infos = nullptr;
			const TSharedPtr<FJsonObject>* InfoPtr = nullptr;
			FString DownloadUrl;
			if (!Json.IsValid() || !Json->TryGetArrayField(TEXT("downloadInfo"), Infos) || Infos->IsEmpty() || !(*Infos)[0]->TryGetObject(InfoPtr)
				|| !(*InfoPtr)->TryGetStringField(TEXT("downloadUrl"), DownloadUrl) || DownloadUrl.IsEmpty())
			{
				if (++State.Route < UE_ARRAY_COUNT(FabRoutes))
					return FUnrealMcpToolResult::RetryLater(TEXT("Requesting the download (library route)."));
				if (FabRenewSession(State, Http))
					return FUnrealMcpToolResult::RetryLater(TEXT("Renewing the Fab session."));
				return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("Fab did not give a download for '%s' (%s). The user must own it: "
					"buy it or, for a free listing, click 'Add to My Library' at %s, then call fab-add again."), *Title, *FabDescribeHttp(Http),
					*FabListingUrl(Uid))));
			}
			const FJsonObject& Info = **InfoPtr;

			// The Fab panel's own "Add to Project": UFabBrowserApi::AddToProject picks the pack / Megascans / Interchange workflow.
			TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
			Metadata->SetStringField(TEXT("AssetId"), Uid);
			Metadata->SetStringField(TEXT("AssetName"), Title);
			Metadata->SetStringField(TEXT("AssetType"), State.Format);
			Metadata->SetStringField(TEXT("ListingType"), FabString(*State.Listing, TEXT("listingType")));
			Metadata->SetStringField(TEXT("AssetNamespace"), TEXT("fab"));
			Metadata->SetBoolField(TEXT("IsQuixel"), FabIsQuixel(*State.Listing) && State.Format != TEXT("unreal-engine"));
			const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
			Metadata->SetArrayField(TEXT("DistributionPointBaseUrls"),
				Info.TryGetArrayField(TEXT("distributionPointBaseUrls"), Points) ? *Points : TArray<TSharedPtr<FJsonValue>>());

			FabForgetImport(Uid); // a reimport's old record, or one whose folder is gone, must not count as this import done
			State.LogSequence = FUnrealMcpLogCollector::Get().GetLastSequence();
			State.ImportLogStart = State.LogSequence;
			// An import of this listing an earlier call started may still run (AddToProject then starts none): keep its window.
			const FFabImportWindow* Open = FabImportWindows().Find(Uid);
			if (!Open || FPlatformTime::Seconds() - Open->OpenedSeconds > FabImportWindowSeconds)
				FabImportWindows().Add(Uid, FabOpenImportWindow());
			bool bFilled = false;
			const bool bCalled = FabInvoke(FabApi(), TEXT("AddToProject"), [&](UFunction* Function, uint8* Params)
			{
				const FStrProperty* UrlParam = CastField<FStrProperty>(Function->FindPropertyByName(TEXT("DownloadUrl")));
				const FStructProperty* MetaParam = CastField<FStructProperty>(Function->FindPropertyByName(TEXT("AssetMetadata")));
				if (!UrlParam || !MetaParam)
					return;
				UrlParam->SetPropertyValue_InContainer(Params, DownloadUrl);
				bFilled = FJsonObjectConverter::JsonObjectToUStruct(Metadata.ToSharedRef(), MetaParam->Struct,
					MetaParam->ContainerPtrToValuePtr<void>(Params));
			}, [](UFunction*, uint8*) {});
			if (!bCalled || !bFilled)
				return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("This engine's Fab plugin has no UFabBrowserApi::AddToProject(DownloadUrl, "
					"AssetMetadata) this tool can call.")));
			UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] fab-add: importing '%s' (%s, %s)"), *Title, *Uid, *State.Format);
			State.Stage = AddImport;
			State.StageStarted = FPlatformTime::Seconds();
		}

		// AddImport: the workflow records the listing in UFabLocalAssets when it is done.
		const FString Path = FabImportedPath(Uid);
		if (!Path.IsEmpty())
			return FabAddDone(Call, State, Uid, Title, Path, false);
		const FString Errors = FabImportErrors(State.LogSequence, Uid, Title);
		if (!Errors.IsEmpty())
		{
			FabImportWindows().Remove(Uid); // a new try opens a new window
			return FabFinish(Call, FUnrealMcpToolResult::Error(FString::Printf(TEXT("Importing '%s' failed: %s"), *(Title.IsEmpty() ? Uid : Title), *Errors)));
		}
		const double Waited = FPlatformTime::Seconds() - State.StageStarted;
		if (Waited < FMath::Min(FabImportWaitSeconds, FabTimeLeft(Call)))
			return FUnrealMcpToolResult::RetryLater(TEXT("Waiting for the Fab import."));
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("uid"), Uid);
		S->SetBoolField(TEXT("inProgress"), true);
		return FabFinish(Call, FUnrealMcpToolResult::Success(FString::Printf(TEXT("Still downloading/importing '%s' after %.0f s (large packs take "
			"minutes; progress shows in the editor's notifications). Call fab-add with the same listing again to keep waiting (it "
			"does not start a second download while this one runs, and starts over if this one failed)."),
			*(Title.IsEmpty() ? Uid : Title), Waited), S));
	}

	// ---------------------------------------------------------------- fab-sign-in

	/**
	 * The one tool that opens Epic's sign-in page (UFabBrowserApi::Login, the Fab plugin's own account-portal login; at
	 * most once per FabPortalReopenSeconds in this editor), then waits for the user (RetryLater) until the page's window or
	 * the call's time runs out. Opening the Fab panel in the editor signs in silently from the remembered Epic login too;
	 * this family never opens the panel itself: the panel's web view cannot exist in a -nullrhi editor, and the engine
	 * asserts there on a layout that saved it.
	 *
	 * When Epic's sign-in service never started (UnrealMcpFab says so, or the plugin's Login logs that it has no EOS
	 * platform), no page opens and nothing could complete: the call fails at once with FabServiceDownText (DEV-281).
	 */
	static FUnrealMcpToolResult FabSignIn(const FUnrealMcpToolCall& Call)
	{
		FString Error;
		if (!FabPluginLoaded(Error))
			return FabFinish(Call, FUnrealMcpToolResult::Error(Error));
		FFabCallState& State = FabState(Call);
		const bool bAskedBefore = State.bAskedSignIn;
		if (!FabReadToken(State).IsEmpty())
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetBoolField(TEXT("signedIn"), true);
			FUnrealMcpToolResult Result = FUnrealMcpToolResult::Success(FString::Printf(TEXT("%s fab-library and fab-add can run now. "
				"Epic remembers the sign-in on this computer, so a later editor session signs in again by itself, without a browser."),
				bAskedBefore ? TEXT("Signed in to Fab.") : TEXT("Already signed in to Fab in this editor.")), S);
			Result.bMessageCoversStructured = true;
			return FabFinish(Call, Result);
		}
		const EUnrealMcpFabSignInService Service = FabService();
		if (Service == EUnrealMcpFabSignInService::NotStarted)
			return FabFinish(Call, FUnrealMcpToolResult::Error(FabServiceDownText(TEXT("fab-sign-in"))));
		if (Service == EUnrealMcpFabSignInService::Restoring && !State.bAskedSignIn)
		{
			// Never open a page while the remembered sign-in is still being restored: a second login would race it.
			if (FabTimeLeft(Call) > 0.0)
				return FUnrealMcpToolResult::RetryLater(TEXT("Restoring the Fab sign-in Epic remembers on this computer."));
			return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("The Fab sign-in Epic remembers on this computer is still being "
				"restored (no browser was opened). Call fab-sign-in again in a moment.")));
		}
		if (!State.bAskedSignIn)
		{
			// Once per call: open the page unless one this editor opened may still be waiting for the user.
			State.bAskedSignIn = true;
			State.StageStarted = FPlatformTime::Seconds();
			if (!FabSignInPageOpen())
			{
				const int64 Before = FUnrealMcpLogCollector::Get().GetLastSequence();
				if (!FabInvoke(FabApi(), TEXT("Login"), [](UFunction*, uint8*) {}, [](UFunction*, uint8*) {}))
					return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("This engine's Fab plugin has no UFabBrowserApi::Login this tool can "
						"call. The user signs in by opening Fab from the editor's toolbar.")));
				// Login returns nothing; without an EOS platform it only logs so (FabAuthentication::LoginUsingAccountPortal).
				static const FName LogFab(TEXT("LogFab"));
				for (const FUnrealMcpLogEntry& Entry : FUnrealMcpLogCollector::Get().SnapshotAfter(Before))
				{
					if (Entry.Category == LogFab && Entry.Message.Contains(TEXT("Invalid EOS platform handle")))
						return FabFinish(Call, FUnrealMcpToolResult::Error(FabServiceDownText(TEXT("fab-sign-in"))));
				}
				FabPortalOpenedAt() = State.StageStarted;
				UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] fab-sign-in: opened Epic's sign-in page in the browser."));
			}
		}
		// The page stays usable until the reopen guard ends: wait for it up to FabPortalWaitSeconds from this call's start.
		if (FabSignInPageOpen() && FPlatformTime::Seconds() - State.StageStarted < FabPortalWaitSeconds && FabTimeLeft(Call) > 0.0)
			return FUnrealMcpToolResult::RetryLater(TEXT("Waiting for the user to sign in on Epic's page."));
		if (!FabSignInPageOpen())
			return FabFinish(Call, FUnrealMcpToolResult::Error(TEXT("The Fab sign-in did not finish, and Epic's sign-in page has expired. "
				"When the user is ready, call fab-sign-in again: it opens a new page.")));
		return FabFinish(Call, FUnrealMcpToolResult::Error(FabSignInStillOpenText(TEXT("fab-sign-in"))));
	}
}

UNREALMCPEDITOR_API FUnrealMcpFabSignIn& UnrealMcpFabSignIn()
{
	static FUnrealMcpFabSignIn SignIn;
	return SignIn;
}

namespace UnrealMcpFabTools
{
	UNREALMCPEDITOR_API FString ParseListingUid(const FString& Input) { return UnrealMcpFabToolsLocal::FabListingUid(Input); }
	UNREALMCPEDITOR_API FString AccountIdFromToken(const FString& Token) { return UnrealMcpFabToolsLocal::FabAccountIdFromToken(Token); }
	UNREALMCPEDITOR_API FString DistributionMethod(const FJsonObject& Format) { return UnrealMcpFabToolsLocal::FabDistributionMethod(Format); }
	UNREALMCPEDITOR_API FString PickUnrealVersion(const TArray<TSharedPtr<FJsonValue>>& Versions) { return UnrealMcpFabToolsLocal::FabUnrealVersionUid(Versions); }
	UNREALMCPEDITOR_API FString SignInRequiredText(const FString& ToolName) { return UnrealMcpFabToolsLocal::FabSignInRequiredText(ToolName); }
	UNREALMCPEDITOR_API FString SearchFooter(bool bSignedIn, EUnrealMcpFabSignInService Service)
	{
		return UnrealMcpFabToolsLocal::FabSearchFooterText(bSignedIn, Service);
	}
	UNREALMCPEDITOR_API TArray<FString> ListingEngines(const FJsonObject& Listing) { return UnrealMcpFabToolsLocal::FabListingEngines(Listing); }
	UNREALMCPEDITOR_API FString PackageFileProblem(const FString& Filename, bool& bOutNeedsNewerEngine)
	{
		return UnrealMcpFabToolsLocal::FabPackageFileProblem(Filename, bOutNeedsNewerEngine);
	}
	UNREALMCPEDITOR_API TArray<FString> CustomVersionProblems(const FCustomVersionArray& Versions, TArray<FString>& OutUnknown)
	{
		return UnrealMcpFabToolsLocal::FabCustomVersionProblems(Versions, TEXT("UnrealMcpFabTools"), OutUnknown);
	}
	UNREALMCPEDITOR_API TSharedPtr<FJsonObject> CheckImport(const FString& Path, TArray<FString>& OutLines)
	{
		const UnrealMcpFabToolsLocal::FFabImportReport Report = UnrealMcpFabToolsLocal::FabCheckImport(Path, {}, 0);
		OutLines = Report.Lines;
		return Report.Requirements;
	}
	UNREALMCPEDITOR_API void OpenImportWindow(const FString& ListingUid)
	{
		UnrealMcpFabToolsLocal::FabImportWindows().Add(ListingUid, UnrealMcpFabToolsLocal::FabOpenImportWindow());
	}
	UNREALMCPEDITOR_API void CloseImportWindow(const FString& ListingUid) { UnrealMcpFabToolsLocal::FabImportWindows().Remove(ListingUid); }
	UNREALMCPEDITOR_API TArray<FString> LocalizeBaseMaterials(const FString& Path, const FString& ListingUid, const FString& CopyRoot,
		TArray<FString>& OutWarnings)
	{
		const UnrealMcpFabToolsLocal::FFabImportWindow* Window = UnrealMcpFabToolsLocal::FabImportWindows().Find(ListingUid);
		return Window ? UnrealMcpFabToolsLocal::FabLocalizeBaseMaterials(Path, *Window, CopyRoot, OutWarnings) : TArray<FString>();
	}
	UNREALMCPEDITOR_API TArray<FString> SaveImport(const FString& Path, const FString& ListingUid, TArray<FString>& OutErrors)
	{
		const UnrealMcpFabToolsLocal::FFabImportWindow* Window = UnrealMcpFabToolsLocal::FabImportWindows().Find(ListingUid);
		return Window ? UnrealMcpFabToolsLocal::FabSaveImport(Path, *Window, OutErrors) : TArray<FString>();
	}
	UNREALMCPEDITOR_API void SetOfflineForTests(bool bOffline) { UnrealMcpFabToolsLocal::FabOffline() = bOffline; }
	UNREALMCPEDITOR_API FString PluginOfDependency(const FString& Dependency)
	{
		const TSharedPtr<IPlugin> Plugin = UnrealMcpFabToolsLocal::FabPluginOfDependency(Dependency, UnrealMcpFabToolsLocal::FabModulePlugins());
		return Plugin.IsValid() ? Plugin->GetName() : FString();
	}

	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		using namespace UnrealMcpFabToolsLocal;

		Registry.Tool(TEXT("fab-search"))
			.Title(TEXT("Search Fab"))
			.Description(TEXT("Search the Fab marketplace (fab.com: 3D models, materials, Megascans, Unreal Engine packs, ...). Returns each "
			                  "listing's uid, title, type, price, file formats and the Unreal Engine versions Fab lists for it (a pack not listed for "
			                  "this editor's version may not load here), and whether this editor is signed in to Fab. Import "
			                  "one the user owns with fab-add, which needs the user's Fab sign-in (fab-sign-in)."))
			.ParamString(TEXT("query"), TEXT("Search text, e.g. 'medieval barrel' or 'mossy rock'."))
			.ParamString(TEXT("listingType"), TEXT("Only this listing type, e.g. 3d-model, material, decal, environment, sprite, audio, "
			                                       "game-template, tool-and-plugin, vfx, animation."))
			.ParamBool(TEXT("freeOnly"), TEXT("Only free listings (default false)."))
			.ParamString(TEXT("sort"), TEXT("relevance (default), rating, newest, price, -price."))
			.ParamInt(TEXT("count"), TEXT("Results per page, 1-48 (default 12)."))
			.ParamString(TEXT("cursor"), TEXT("The next-page cursor a previous call returned."))
			.ReadOnlyHint(true).DestructiveHint(false).IdempotentHint(true).OpenWorldHint(true)
			.Handle(&FabSearch);

		Registry.Tool(TEXT("fab-library"))
			.Title(TEXT("List Fab Library"))
			.Description(TEXT("List the Fab assets the signed-in user owns (their Fab library), with each listing's uid and where it is "
			                  "already imported in this project. Needs the user's Fab sign-in in this editor: without it the call says "
			                  "so at once (it never opens a browser; fab-sign-in does, when the user agreed)."))
			.ParamString(TEXT("filter"), TEXT("Only titles containing this text (case-insensitive), searched across the whole library."))
			.ParamInt(TEXT("count"), TEXT("Items per page without a filter, 1-100 (default 24)."))
			.ParamString(TEXT("cursor"), TEXT("The next-page cursor a previous call returned."))
			.ReadOnlyHint(true).DestructiveHint(false).IdempotentHint(true).OpenWorldHint(true)
			.Handle(&FabLibrary);

		Registry.Tool(TEXT("fab-add"))
			.Title(TEXT("Add Fab Asset to Project"))
			.Description(TEXT("Download a Fab listing the user owns and import it into the project, like the Fab panel's 'Add to Project': "
			                  "Unreal Engine packs go to /Game/<Pack>, Megascans to /Game/Fab/Megascans/..., glTF/FBX models to /Game/Fab/... "
			                  "Returns the content folder it landed in and the pack's 'requirements' in this editor: packages it cannot "
			                  "load (saved by a newer engine; the call is then an error), plugins its content needs and whether they are "
			                  "enabled, startup cvars it needs (Nanite Assemblies) with their values, and references that do not resolve. "
			                  "The packages the import created and left unsaved (Megascans, glTF) are saved before they are checked; "
			                  "any still unsaved under the folder are listed (`unsavedPackages`) with the call that saves one. "
			                  "A listing already in the project is not downloaded again. Large "
			                  "packs may take minutes: when the call ends before the import does, call it again with the same listing. "
			                  "Complete projects and engine code plugins are not added to a project (the Epic Games Launcher installs them). "
			                  "Needs the user's Fab sign-in in this editor: without it the call says so at once and never opens a browser."))
			.ParamString(TEXT("listing"), TEXT("The listing uid (from fab-search / fab-library) or its https://www.fab.com/listings/<uid> URL."),
				EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("format"), TEXT("unreal-engine, glb, gltf or fbx (default: the best one the listing has)."))
			.ParamString(TEXT("quality"), TEXT("Megascans only: low, medium (default), high or raw."))
			.ParamBool(TEXT("reimport"), TEXT("Download and import again even when the listing is already in the project, replacing its "
			                                   "assets (default false)."))
			.ParamBool(TEXT("localBaseMaterial"), TEXT("Reparent the material instances this call imports whose base material is read-only engine "
			                                            "content (Megascans' M_MS_Base) to fab-add's copy of it for this engine version in "
			                                            "/Game/Fab/Materials with the Nanite and instanced usages set, so PCG / Nanite use never "
			                                            "dirties engine content (default true). Never touches a listing already in the project, "
			                                            "instances with unsaved changes from before the import, or a material there fab-add did not make."))
			.ReadOnlyHint(false).DestructiveHint(true).IdempotentHint(false).OpenWorldHint(true)
			.Handle(&FabAdd);

		Registry.Tool(TEXT("fab-sign-in"))
			.Title(TEXT("Sign In to Fab"))
			.Description(TEXT("Sign the user in to Fab in this editor, which fab-add and fab-library need: opens Epic's sign-in page in "
			                  "the browser on this computer and waits for the user to finish (answers at once when already signed in). "
			                  "Only the user can complete it: call it only when the user is present and agreed (ask with "
			                  "AskUserQuestion first), or when the user asked to sign in ahead of a run. Never call it in an unattended run."))
			.ReadOnlyHint(false).DestructiveHint(false).IdempotentHint(true).OpenWorldHint(true)
			.Handle(&FabSignIn);
	}
}
