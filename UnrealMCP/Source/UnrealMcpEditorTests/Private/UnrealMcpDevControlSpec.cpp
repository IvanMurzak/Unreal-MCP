// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "DevControl/UnrealMcpDevControlServer.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "Config/UnrealMcpConfig.h"

/**
 * Dev-control bridge specs (docs/ARCHITECTURE.md §7): the PURE request → view-model routing/parsing that
 * FUnrealMcpDevControlServer::RouteRequest performs — health/state snapshots, the inject + control verbs,
 * and the error surface (bad method / path / body) — all WITHOUT a live HTTP socket. The view-model is the
 * real one, wired with recording sinks so a spec can assert the mutators fired (it is drivable with no live
 * bridge, see its docblock). Helpers carry the spec-unique `DevCtl` prefix (CLAUDE.md unity-build ODR rule).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpDevControlSpec, "UnrealMcp.DevControl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// A view-model wired with recording sinks so a route can be asserted as having driven the dock.
	struct FDevCtlRecording
	{
		int32 PersistCount = 0;
		int32 PushCount = 0;
		TArray<FString> AuthSent;
	};

	static TSharedRef<FUnrealMcpEditorViewModel> DevCtlMakeViewModel(TSharedRef<FDevCtlRecording> Rec)
	{
		TSharedRef<FUnrealMcpEditorViewModel> VM = MakeShared<FUnrealMcpEditorViewModel>();
		VM->OnPersistConfig = [Rec](const FUnrealMcpConfig&) { Rec->PersistCount++; };
		VM->OnPushConfig = [Rec](const FUnrealMcpConfig&) { Rec->PushCount++; };
		VM->OnSendAuth = [Rec](const FString& Type) -> bool { Rec->AuthSent.Add(Type); return true; };
		VM->OnOpenBrowser = [](const FString&) {};
		return VM;
	}

	// Parse a JSON body string into an object (the spec's stand-in for the request body the HTTP layer parses).
	static TSharedPtr<FJsonObject> DevCtlBody(const FString& Json)
	{
		TSharedPtr<FJsonObject> Out;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Out);
		return Out;
	}

END_DEFINE_SPEC(FUnrealMcpDevControlSpec)

void FUnrealMcpDevControlSpec::Define()
{
	Describe("Health + missing view-model", [this]()
	{
		It("answers /health 200 with no view-model required", [this]()
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(TEXT("GET"), TEXT("/health"), nullptr, nullptr, Result);
			TestEqual("status", Status, 200);
			TestTrue("ok", Result->GetBoolField(TEXT("ok")));
		});

		It("answers 503 for a view-model-required route when the dock is gone", [this]()
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(TEXT("GET"), TEXT("/state"), nullptr, nullptr, Result);
			TestEqual("status", Status, 503);
			TestFalse("not ok", Result->GetBoolField(TEXT("ok")));
		});

		It("answers 404 for an unknown route", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(TEXT("GET"), TEXT("/nope"), nullptr, &VM.Get(), Result);
			TestEqual("status", Status, 404);
			TestFalse("not ok", Result->GetBoolField(TEXT("ok")));
		});
	});

	Describe("State snapshot", [this]()
	{
		It("reports the live connection state, mode, host, agent and ai-agents", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetCustomHost(TEXT("http://localhost:8080"));
			VM->SetSelectedAgentId(TEXT("cursor"));

			// Inject an aiAgents list via a status message so /state surfaces it.
			TSharedPtr<FJsonObject> StatusJson = MakeShared<FJsonObject>();
			StatusJson->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			StatusJson->SetBoolField(TEXT("keepConnected"), true);
			TArray<TSharedPtr<FJsonValue>> Agents;
			Agents.Add(MakeShared<FJsonValueString>(TEXT("Claude Code")));
			StatusJson->SetArrayField(TEXT("aiAgents"), Agents);
			VM->ApplyStatus(StatusJson);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(TEXT("GET"), TEXT("/state"), nullptr, &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestTrue("ok", Result->GetBoolField(TEXT("ok")));
			TestEqual("connectionState", Result->GetStringField(TEXT("connectionState")), FString(TEXT("Connected")));
			TestEqual("connectionMode", Result->GetStringField(TEXT("connectionMode")), FString(TEXT("Custom")));
			TestEqual("customHost", Result->GetStringField(TEXT("customHost")), FString(TEXT("http://localhost:8080")));
			TestEqual("selectedAgentId", Result->GetStringField(TEXT("selectedAgentId")), FString(TEXT("cursor")));

			const TArray<TSharedPtr<FJsonValue>>* OutAgents = nullptr;
			TestTrue("aiAgents present", Result->TryGetArrayField(TEXT("aiAgents"), OutAgents) && OutAgents != nullptr);
			if (OutAgents)
			{
				TestEqual("aiAgents count", OutAgents->Num(), 1);
				if (OutAgents->Num() == 1)
					TestEqual("aiAgents[0]", (*OutAgents)[0]->AsString(), FString(TEXT("Claude Code")));
			}
		});
	});

	Describe("Inject connection-status", [this]()
	{
		It("applies a Connected status to the view-model", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/connection-status"), DevCtlBody(TEXT("{\"status\":\"Connected\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestTrue("ok", Result->GetBoolField(TEXT("ok")));
			TestEqual("view-model state", VM->GetConnectionState(), EUnrealMcpConnectionState::Connected);
		});

		It("applies a Disconnected status (reconnect off)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/connection-status"), DevCtlBody(TEXT("{\"status\":\"Disconnected\"}")), &VM.Get(), Result);

			TestEqual("view-model state", VM->GetConnectionState(), EUnrealMcpConnectionState::Disconnected);
		});

		It("rejects a missing status with 400", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/connection-status"), DevCtlBody(TEXT("{}")), &VM.Get(), Result);
			TestEqual("status", Status, 400);
			TestFalse("not ok", Result->GetBoolField(TEXT("ok")));
		});
	});

	Describe("Control mutators", [this]()
	{
		It("server-url drives SetCustomHost (valid URL persists + pushes)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/server-url"), DevCtlBody(TEXT("{\"url\":\"http://127.0.0.1:5244\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestEqual("custom host", VM->GetCustomHost(), FString(TEXT("http://127.0.0.1:5244")));
		});

		It("connection-mode drives SetConnectionMode", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Cloud);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/connection-mode"), DevCtlBody(TEXT("{\"mode\":\"Custom\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestEqual("mode", VM->GetConnectionMode(), EUnrealMcpConnectionMode::Custom);
		});

		It("rejects an invalid connection-mode with 400", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/connection-mode"), DevCtlBody(TEXT("{\"mode\":\"Banana\"}")), &VM.Get(), Result);
			TestEqual("status", Status, 400);
		});

		It("select-agent drives SetSelectedAgentId", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/select-agent"), DevCtlBody(TEXT("{\"agentId\":\"copilot\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestEqual("selected agent", VM->GetSelectedAgentId(), FString(TEXT("copilot")));
		});

		It("click=connect arms the reconnect loop and goes Connecting", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"connect\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestTrue("reconnect armed", VM->IsReconnectArmed());
			TestEqual("state", VM->GetConnectionState(), EUnrealMcpConnectionState::Connecting);
		});

		It("click=disconnect halts the reconnect loop", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->Connect();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"disconnect\"}")), &VM.Get(), Result);

			TestFalse("reconnect disarmed", VM->IsReconnectArmed());
			TestEqual("state", VM->GetConnectionState(), EUnrealMcpConnectionState::Disconnected);
		});

		It("click=generate-token replaces the custom token", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			const FString Before = VM->GetCustomToken();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"generate-token\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestFalse("token changed", VM->GetCustomToken().Equals(Before));
			TestFalse("token non-empty", VM->GetCustomToken().IsEmpty());
		});

		It("rejects an unknown click target with 400", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"explode\"}")), &VM.Get(), Result);
			TestEqual("status", Status, 400);
			TestFalse("not ok", Result->GetBoolField(TEXT("ok")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
