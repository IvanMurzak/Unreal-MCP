// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "DevControl/UnrealMcpDevControlServer.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UI/UnrealMcpAuxWindows.h"
#include "Config/UnrealMcpConfig.h"
#include "UnrealMcpLog.h"

#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "HttpServerRequest.h"
#include "HttpPath.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

// Unity-build ODR rule (CLAUDE.md): every file-local helper carries the family-unique `DevControl` prefix
// so a same-name/same-signature helper in another unity-grouped .cpp cannot collide.
namespace
{
	// Serialize a JSON object to a condensed string (the dev-control response body / state snapshot).
	FString DevControlSerialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	// The canonical connection-state label the dev-control /state snapshot reports (matches ParseConnectionState).
	const TCHAR* DevControlConnectionStateLabel(EUnrealMcpConnectionState State)
	{
		switch (State)
		{
			case EUnrealMcpConnectionState::Connected:    return TEXT("Connected");
			case EUnrealMcpConnectionState::Connecting:   return TEXT("Connecting");
			case EUnrealMcpConnectionState::Degraded:     return TEXT("Degraded");
			case EUnrealMcpConnectionState::Disconnected: default: return TEXT("Disconnected");
		}
	}

	// The connection-mode string the dev-control surface speaks ("Cloud"/"Custom").
	const TCHAR* DevControlConnectionModeLabel(EUnrealMcpConnectionMode Mode)
	{
		return Mode == EUnrealMcpConnectionMode::Cloud ? TEXT("Cloud") : TEXT("Custom");
	}

	// Parse a "Cloud"/"Custom" mode string (case-insensitive). Returns true + the mode on a match.
	bool DevControlParseMode(const FString& Raw, EUnrealMcpConnectionMode& OutMode)
	{
		if (Raw.Equals(TEXT("Cloud"), ESearchCase::IgnoreCase)) { OutMode = EUnrealMcpConnectionMode::Cloud; return true; }
		if (Raw.Equals(TEXT("Custom"), ESearchCase::IgnoreCase)) { OutMode = EUnrealMcpConnectionMode::Custom; return true; }
		return false;
	}

	// Build a `{"ok":false,"error":<msg>}` result into OutResult and return the given status code.
	int32 DevControlFail(TSharedRef<FJsonObject>& OutResult, const FString& Error)
	{
		OutResult->SetBoolField(TEXT("ok"), false);
		OutResult->SetStringField(TEXT("error"), Error);
		return -1; // status filled in by the caller
	}

	// Whether a peer address is loopback. FInternetAddr has no IsLoopbackAddress(), so compare the IP-only
	// string form against the IPv4 / IPv6 loopback literals (covers 127.0.0.1 and ::1 across protocol types).
	bool DevControlIsLoopback(const TSharedPtr<FInternetAddr>& Addr)
	{
		if (!Addr.IsValid())
			return false;
		const FString Ip = Addr->ToString(/*bAppendPort*/ false);
		return Ip.StartsWith(TEXT("127.")) || Ip == TEXT("::1") || Ip == TEXT("0:0:0:0:0:0:0:1");
	}
}

FUnrealMcpDevControlServer::FUnrealMcpDevControlServer() = default;

FUnrealMcpDevControlServer::~FUnrealMcpDevControlServer()
{
	Stop();
}

bool FUnrealMcpDevControlServer::Start(uint32 Port, const TSharedPtr<FUnrealMcpEditorViewModel>& InViewModel)
{
	if (bRunning)
		return true;

	// A link-time dependency on "HTTPServer" does NOT auto-LOAD the module at runtime, so force-load it before
	// FHttpServerModule::Get() (otherwise IsAvailable() is false and the bridge silently never starts).
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("HTTPServer")))
	{
		FModuleManager::Get().LoadModule(TEXT("HTTPServer"));
	}

	if (!FHttpServerModule::IsAvailable())
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[dev-control] HTTPServer module unavailable; dev-control server not started."));
		return false;
	}

	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	Router = HttpServerModule.GetHttpRouter(Port, /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[dev-control] could not bind HTTP router on port %u; dev-control server not started."), Port);
		return false;
	}

	ViewModel = InViewModel;
	BoundPort = Port;

	// Bind the v1 routes. Each handler funnels through HandleRequest(<method>, ...) so loopback verification,
	// body parsing, view-model resolution and JSON response framing live in exactly one place.
	auto Bind = [this](const TCHAR* InPath, EHttpServerRequestVerbs Verbs, const FString& Method)
	{
		const FString Path = InPath;
		FHttpRouteHandle Handle = Router->BindRoute(FHttpPath(Path), Verbs,
			FHttpRequestHandler::CreateLambda([this, Method, Path](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) -> bool
			{
				return HandleRequest(Method, Path, Request, OnComplete);
			}));
		if (Handle.IsValid())
			RouteHandles.Add(Handle);
	};

	Bind(TEXT("/health"),                   EHttpServerRequestVerbs::VERB_GET,  TEXT("GET"));
	Bind(TEXT("/state"),                    EHttpServerRequestVerbs::VERB_GET,  TEXT("GET"));
	Bind(TEXT("/inject/connection-status"), EHttpServerRequestVerbs::VERB_POST, TEXT("POST"));
	Bind(TEXT("/control/server-url"),       EHttpServerRequestVerbs::VERB_POST, TEXT("POST"));
	Bind(TEXT("/control/connection-mode"),  EHttpServerRequestVerbs::VERB_POST, TEXT("POST"));
	Bind(TEXT("/control/select-agent"),     EHttpServerRequestVerbs::VERB_POST, TEXT("POST"));
	Bind(TEXT("/control/click"),            EHttpServerRequestVerbs::VERB_POST, TEXT("POST"));

	// FHttpServerModule only ticks listeners that have been started; start them so this router accepts.
	HttpServerModule.StartAllListeners();

	bRunning = true;
	UE_LOG(LogUnrealMcp, Log, TEXT("[dev-control] Listening on http://127.0.0.1:%u"), BoundPort);
	return true;
}

void FUnrealMcpDevControlServer::Stop()
{
	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			if (Handle.IsValid())
				Router->UnbindRoute(Handle);
		}
	}
	RouteHandles.Reset();
	Router.Reset();
	ViewModel.Reset();
	BoundPort = 0;

	if (bRunning)
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("[dev-control] stopped."));
		bRunning = false;
	}
}

bool FUnrealMcpDevControlServer::HandleRequest(const FString& Method, const FString& Path, const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	auto Respond = [&OnComplete](int32 StatusCode, const TSharedRef<FJsonObject>& Result)
	{
		const FString Body = DevControlSerialize(Result);
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = static_cast<EHttpServerResponseCodes>(StatusCode);
		OnComplete(MoveTemp(Response));
	};

	// Defence in depth behind the env gate: HTTPServer binds all interfaces, so reject any non-loopback peer.
	// (PeerAddress may be null on some transports; treat unknown as untrusted.)
	if (!DevControlIsLoopback(Request.PeerAddress))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("error"), TEXT("dev-control accepts loopback requests only"));
		Respond(403, Result);
		return true;
	}

	// Parse the JSON body (empty body → null object, tolerated by RouteRequest; malformed → 400).
	TSharedPtr<FJsonObject> Body;
	FString BodyString;
	if (Request.Body.Num() > 0)
	{
		const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		BodyString = FString(Converter.Length(), Converter.Get());
	}
	if (!BodyString.IsEmpty())
	{
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(BodyString);
		if (!FJsonSerializer::Deserialize(Reader, Body) || !Body.IsValid())
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("malformed JSON body"));
			Respond(400, Result);
			return true;
		}
	}

	// Handlers run on the game thread (FHttpServerModule ticks there), so it is safe to touch the view-model.
	TSharedPtr<FUnrealMcpEditorViewModel> VM = ViewModel.Pin();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	const int32 Status = RouteRequest(Method, Path, Body, VM.Get(), Result);
	Respond(Status, Result);
	return true;
}

int32 FUnrealMcpDevControlServer::RouteRequest(
	const FString& Method,
	const FString& Path,
	const TSharedPtr<FJsonObject>& Body,
	FUnrealMcpEditorViewModel* ViewModelPtr,
	TSharedRef<FJsonObject>& OutResult)
{
	// /health needs no view-model — it is the liveness probe.
	if (Method == TEXT("GET") && Path == TEXT("/health"))
	{
		OutResult->SetBoolField(TEXT("ok"), true);
		OutResult->SetStringField(TEXT("service"), TEXT("unreal-mcp-dev-control"));
		OutResult->SetNumberField(TEXT("version"), 1);
		return 200;
	}

	// Every other route drives or reads the live dock view-model. If it is gone (teardown / not yet created),
	// answer 503 rather than fabricating state.
	if (ViewModelPtr == nullptr)
	{
		DevControlFail(OutResult, TEXT("view-model unavailable (dock not initialized)"));
		return 503;
	}

	if (Method == TEXT("GET") && Path == TEXT("/state"))
	{
		OutResult->SetBoolField(TEXT("ok"), true);
		OutResult->SetStringField(TEXT("connectionState"), DevControlConnectionStateLabel(ViewModelPtr->GetConnectionState()));
		OutResult->SetStringField(TEXT("connectionMode"), DevControlConnectionModeLabel(ViewModelPtr->GetConnectionMode()));
		OutResult->SetStringField(TEXT("customHost"), ViewModelPtr->GetCustomHost());
		OutResult->SetStringField(TEXT("selectedAgentId"), ViewModelPtr->GetSelectedAgentId());
		// The Serialization Check window's tab id — lets the smoke test assert membership and (with a
		// `check` click) drive the same footer "Check" button the dock renders.
		OutResult->SetStringField(TEXT("serializationCheckTabId"), FUnrealMcpAuxWindows::SerializationCheckTabId.ToString());

		TArray<TSharedPtr<FJsonValue>> Agents;
		for (const FString& Agent : ViewModelPtr->GetAiAgents())
			Agents.Add(MakeShared<FJsonValueString>(Agent));
		OutResult->SetArrayField(TEXT("aiAgents"), Agents);
		return 200;
	}

	// All remaining routes are POSTs that consume the JSON body. A missing body for a body-required route
	// is a 400.
	if (Method != TEXT("POST"))
	{
		DevControlFail(OutResult, FString::Printf(TEXT("no route for %s %s"), *Method, *Path));
		return 404;
	}

	if (Path == TEXT("/inject/connection-status"))
	{
		FString StatusLabel;
		if (!Body.IsValid() || !Body->TryGetStringField(TEXT("status"), StatusLabel) || StatusLabel.IsEmpty())
		{
			DevControlFail(OutResult, TEXT("missing 'status' (Connected|Connecting|Disconnected|Degraded)"));
			return 400;
		}

		// Build the minimal §1.3 `status` envelope the view-model's ApplyStatus consumes: connectionState +
		// keepConnected. A Disconnected status implies the reconnect loop is off; everything else keeps it armed.
		const bool bKeepConnected = !StatusLabel.Equals(TEXT("Disconnected"), ESearchCase::IgnoreCase);
		TSharedPtr<FJsonObject> StatusJson = MakeShared<FJsonObject>();
		StatusJson->SetStringField(TEXT("connectionState"), StatusLabel);
		StatusJson->SetBoolField(TEXT("keepConnected"), bKeepConnected);
		ViewModelPtr->ApplyStatus(StatusJson);

		OutResult->SetBoolField(TEXT("ok"), true);
		OutResult->SetStringField(TEXT("connectionState"), DevControlConnectionStateLabel(ViewModelPtr->GetConnectionState()));
		return 200;
	}

	if (Path == TEXT("/control/server-url"))
	{
		FString Url;
		if (!Body.IsValid() || !Body->TryGetStringField(TEXT("url"), Url))
		{
			DevControlFail(OutResult, TEXT("missing 'url'"));
			return 400;
		}
		ViewModelPtr->SetCustomHost(Url);
		OutResult->SetBoolField(TEXT("ok"), true);
		OutResult->SetStringField(TEXT("customHost"), ViewModelPtr->GetCustomHost());
		return 200;
	}

	if (Path == TEXT("/control/connection-mode"))
	{
		FString ModeRaw;
		EUnrealMcpConnectionMode Mode;
		if (!Body.IsValid() || !Body->TryGetStringField(TEXT("mode"), ModeRaw) || !DevControlParseMode(ModeRaw, Mode))
		{
			DevControlFail(OutResult, TEXT("missing/invalid 'mode' (Cloud|Custom)"));
			return 400;
		}
		ViewModelPtr->SetConnectionMode(Mode);
		OutResult->SetBoolField(TEXT("ok"), true);
		OutResult->SetStringField(TEXT("connectionMode"), DevControlConnectionModeLabel(ViewModelPtr->GetConnectionMode()));
		return 200;
	}

	if (Path == TEXT("/control/select-agent"))
	{
		FString AgentId;
		if (!Body.IsValid() || !Body->TryGetStringField(TEXT("agentId"), AgentId) || AgentId.IsEmpty())
		{
			DevControlFail(OutResult, TEXT("missing 'agentId'"));
			return 400;
		}
		ViewModelPtr->SetSelectedAgentId(AgentId);
		OutResult->SetBoolField(TEXT("ok"), true);
		OutResult->SetStringField(TEXT("selectedAgentId"), ViewModelPtr->GetSelectedAgentId());
		return 200;
	}

	if (Path == TEXT("/control/click"))
	{
		FString Target;
		if (!Body.IsValid() || !Body->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty())
		{
			DevControlFail(OutResult, TEXT("missing 'target' (connect|disconnect|authorize|revoke|generate-token|check)"));
			return 400;
		}

		// Map the click target onto the matching view-model mutator — the same action the Slate button fires.
		// `check` is the odd one out: it opens the Serialization Check aux window rather than mutating the
		// view-model, so it routes through the static aux-window invoker (a headless no-op — see the helper).
		if (Target.Equals(TEXT("connect"), ESearchCase::IgnoreCase))               ViewModelPtr->Connect();
		else if (Target.Equals(TEXT("disconnect"), ESearchCase::IgnoreCase))       ViewModelPtr->Disconnect();
		else if (Target.Equals(TEXT("authorize"), ESearchCase::IgnoreCase))        ViewModelPtr->Authorize();
		else if (Target.Equals(TEXT("revoke"), ESearchCase::IgnoreCase))           ViewModelPtr->Revoke();
		else if (Target.Equals(TEXT("generate-token"), ESearchCase::IgnoreCase))   ViewModelPtr->GenerateCustomToken();
		else if (Target.Equals(TEXT("check"), ESearchCase::IgnoreCase))            FUnrealMcpAuxWindows::TryInvokeSerializationCheckTab();
		else
		{
			DevControlFail(OutResult, FString::Printf(TEXT("unknown click target '%s'"), *Target));
			return 400;
		}

		OutResult->SetBoolField(TEXT("ok"), true);
		OutResult->SetStringField(TEXT("target"), Target.ToLower());
		OutResult->SetStringField(TEXT("connectionState"), DevControlConnectionStateLabel(ViewModelPtr->GetConnectionState()));
		return 200;
	}

	DevControlFail(OutResult, FString::Printf(TEXT("no route for %s %s"), *Method, *Path));
	return 404;
}
