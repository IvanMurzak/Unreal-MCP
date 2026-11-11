// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpSchema.h"
#include "Config/UnrealMcpConfig.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"

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
#include "UObject/UnrealType.h"

/**
 * The Fab tool family: `fab-search` (the public Fab catalog), `fab-library` (what the signed-in user owns) and `fab-add`
 * (download a listing and import it into the project, as the Fab panel's "Add to Project" does).
 *
 * The engine's Fab plugin (Engine/Plugins/Fab, enabled by default since UE 5.5) owns sign-in (EOS) and the import
 * workflows (packs, Megascans, glTF/FBX). Its types are private to it, so this family reaches them only through UE
 * reflection: UFabBrowserApi::GetAuthToken / AddToProject (UFUNCTIONs) and UFabLocalAssets::PathsListingID (the config
 * map the workflows write when an import completes). Nothing links against the Fab module, so the editor still loads
 * when the Fab plugin is disabled; the tools then answer with an error.
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

	/** Start a request; its completion (on the game thread) fills the returned result. */
	static TSharedPtr<FFabHttpResult> FabHttpStart(const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers,
		const FString& Body = FString())
	{
		TSharedPtr<FFabHttpResult> Result = MakeShared<FFabHttpResult>();
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
		int64 LogSequence = 0;
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

	/** The signed-in user's EOS access token, or empty when nobody is signed in to Fab in this editor. */
	static FString FabAuthToken() { return FabApiString(TEXT("GetAuthToken")); }

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

	/** When this editor last opened Epic's sign-in page, so concurrent and repeated calls do not open a browser tab each. */
	static double& FabPortalOpenedAt()
	{
		static double OpenedAt = -1.0e9;
		return OpenedAt;
	}

	/**
	 * The user's token for this call. Without one, Epic's sign-in page opens in the browser (UFabBrowserApi::Login, the
	 * Fab plugin's own account-portal login; at most once per FabPortalReopenSeconds in this editor) and the call waits for
	 * the user (RetryLater) until the page's window or the call's time runs out. Opening the Fab panel in the editor signs
	 * in silently from the remembered Epic login too; this family never opens the panel itself: the panel's web view
	 * cannot exist in a -nullrhi editor, and the engine asserts there on a layout that saved it. The token is read at
	 * most once a second, since every failed read logs a LogFab error.
	 */
	static bool FabEnsureToken(const FUnrealMcpToolCall& Call, FFabCallState& State, FString& OutToken, FUnrealMcpToolResult& OutWait)
	{
		if (!State.Token.IsEmpty())
		{
			OutToken = State.Token;
			return true;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - State.LastTokenCheck >= 1.0)
		{
			State.LastTokenCheck = Now;
			OutToken = FabAuthToken();
			if (!OutToken.IsEmpty())
			{
				State.Token = OutToken;
				return true;
			}
		}
		if (!State.bAskedSignIn)
		{
			// Once per call: open the page unless one this editor opened is still waiting for the user.
			State.bAskedSignIn = true;
			if (Now - FabPortalOpenedAt() >= FabPortalReopenSeconds)
			{
				FabPortalOpenedAt() = Now;
				FabInvoke(FabApi(), TEXT("Login"), [](UFunction*, uint8*) {}, [](UFunction*, uint8*) {});
				UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] fab: not signed in to Fab; opened Epic's sign-in page in the browser."));
			}
		}
		if (Now < FabPortalOpenedAt() + FabPortalWaitSeconds && FabTimeLeft(Call) > 0.0)
		{
			OutWait = FUnrealMcpToolResult::RetryLater(TEXT("Waiting for the Fab sign-in."));
			return false;
		}
		OutWait = FabFinish(Call, FUnrealMcpToolResult::Error(TEXT(
			"Not signed in to Fab. Epic's sign-in page was opened in the browser (it is not opened again for 10 minutes): the user "
			"signs in there with their Epic Games account, or opens Fab in the editor's toolbar, which signs in from a remembered "
			"login; then call this tool again.")));
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
		TArray<TSharedPtr<FJsonValue>> FormatValues;
		for (const FString& Format : Formats)
			FormatValues.Add(MakeShared<FJsonValueString>(Format));
		Row->SetArrayField(TEXT("formats"), FormatValues);
		OutLine = FString::Printf(TEXT("%s | %s | %s, %s%s%s | formats: %s"), *Uid, *Title, *Type, *PriceText,
			CategoryName.IsEmpty() ? TEXT("") : *(TEXT(", ") + CategoryName), Seller.IsEmpty() ? TEXT("") : *(TEXT(", by ") + Seller),
			Formats.Num() ? *FString::Join(Formats, TEXT(",")) : TEXT("-"));
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
		const TArray<FString>& Lines, const FString& Next, const FString& Header, const TCHAR* Footer)
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
		return FabPageResult(Call, Rows, Lines, FabNextCursor(*Json),
			FString::Printf(TEXT("%d Fab listing(s) (uid | title | type, price | formats):"), Rows.Num()),
			TEXT("\nfab-add imports a listing the user owns (fab-library lists them); a free listing must first be added to the "
			     "user's library on fab.com."));
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
		if (!FabEnsureToken(Call, State, Token, Wait))
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
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Category : (*Details)->Values)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Category.Value.IsValid() && Category.Value->TryGetObject(Object) && (*Object)->TryGetStringField(TEXT("distributionMethod"), Method))
					break;
			}
		}
		return Method.ToLower();
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

	/**
	 * The unreal-engine version to download: one built for this editor's UE, else the newest one built for an older UE
	 * (content opens in newer editors), else the oldest newer one.
	 */
	static FString FabUnrealVersionUid(const TArray<TSharedPtr<FJsonValue>>& Versions)
	{
		const int32 Mine = FEngineVersion::Current().GetMajor() * 100 + FEngineVersion::Current().GetMinor();
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

	static FUnrealMcpToolResult FabAddDone(const FUnrealMcpToolCall& Call, const FString& Uid, const FString& Title, const FString& Path,
		bool bAlready)
	{
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("uid"), Uid);
		S->SetStringField(TEXT("title"), Title);
		S->SetStringField(TEXT("path"), Path);
		S->SetBoolField(TEXT("alreadyInProject"), bAlready);
		const FString Text = FString::Printf(TEXT("%s '%s' %s at %s. List what it holds: rql-query `assets | where Path startswith \"%s\" | "
			"select Name, Class, Path`."), bAlready ? TEXT("Already in the project:") : TEXT("Imported"), *(Title.IsEmpty() ? Uid : Title),
			bAlready ? TEXT("is") : TEXT("into the project"), *Path, *Path);
		FUnrealMcpToolResult Result = FUnrealMcpToolResult::Success(Text, S);
		Result.bMessageCoversStructured = true;
		return FabFinish(Call, Result);
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
					return FabAddDone(Call, Uid, Title, Existing, true);
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
			if (!FabEnsureToken(Call, State, Token, Wait))
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
				Body->SetStringField(TEXT("refresh_token"), FabApiString(TEXT("GetRefreshToken")));
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
					State.FileUid = FabUnrealVersionUid(*Items);
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
			return FabAddDone(Call, Uid, Title, Path, false);
		const FString Errors = FabImportErrors(State.LogSequence, Uid, Title);
		if (!Errors.IsEmpty())
		{
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
}

namespace UnrealMcpFabTools
{
	UNREALMCPEDITOR_API FString ParseListingUid(const FString& Input) { return UnrealMcpFabToolsLocal::FabListingUid(Input); }
	UNREALMCPEDITOR_API FString AccountIdFromToken(const FString& Token) { return UnrealMcpFabToolsLocal::FabAccountIdFromToken(Token); }
	UNREALMCPEDITOR_API FString DistributionMethod(const FJsonObject& Format) { return UnrealMcpFabToolsLocal::FabDistributionMethod(Format); }
	UNREALMCPEDITOR_API FString PickUnrealVersion(const TArray<TSharedPtr<FJsonValue>>& Versions) { return UnrealMcpFabToolsLocal::FabUnrealVersionUid(Versions); }

	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		using namespace UnrealMcpFabToolsLocal;

		Registry.Tool(TEXT("fab-search"))
			.Title(TEXT("Search Fab"))
			.Description(TEXT("Search the Fab marketplace (fab.com: 3D models, materials, Megascans, Unreal Engine packs, ...). Returns each "
			                  "listing's uid, title, type, price and file formats. Import one the user owns with fab-add."))
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
			                  "already imported in this project. Needs the user's Fab sign-in in this editor: when missing, the tool "
			                  "opens Epic's sign-in page in the browser and waits for the user (opening Fab in the editor's toolbar also "
			                  "signs in)."))
			.ParamString(TEXT("filter"), TEXT("Only titles containing this text (case-insensitive), searched across the whole library."))
			.ParamInt(TEXT("count"), TEXT("Items per page without a filter, 1-100 (default 24)."))
			.ParamString(TEXT("cursor"), TEXT("The next-page cursor a previous call returned."))
			.ReadOnlyHint(true).DestructiveHint(false).IdempotentHint(true).OpenWorldHint(true)
			.Handle(&FabLibrary);

		Registry.Tool(TEXT("fab-add"))
			.Title(TEXT("Add Fab Asset to Project"))
			.Description(TEXT("Download a Fab listing the user owns and import it into the project, like the Fab panel's 'Add to Project': "
			                  "Unreal Engine packs go to /Game/<Pack>, Megascans to /Game/Fab/Megascans/..., glTF/FBX models to /Game/Fab/... "
			                  "Returns the content folder it landed in. A listing already in the project is not downloaded again. Large "
			                  "packs may take minutes: when the call ends before the import does, call it again with the same listing. "
			                  "Complete projects and engine code plugins are not added to a project (the Epic Games Launcher installs them)."))
			.ParamString(TEXT("listing"), TEXT("The listing uid (from fab-search / fab-library) or its https://www.fab.com/listings/<uid> URL."),
				EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("format"), TEXT("unreal-engine, glb, gltf or fbx (default: the best one the listing has)."))
			.ParamString(TEXT("quality"), TEXT("Megascans only: low, medium (default), high or raw."))
			.ParamBool(TEXT("reimport"), TEXT("Download and import again even when the listing is already in the project, replacing its "
			                                   "assets (default false)."))
			.ReadOnlyHint(false).DestructiveHint(true).IdempotentHint(false).OpenWorldHint(true)
			.Handle(&FabAdd);
	}
}
