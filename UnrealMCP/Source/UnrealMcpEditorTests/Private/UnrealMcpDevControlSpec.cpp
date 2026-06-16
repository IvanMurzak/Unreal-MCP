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
		// §7 in-UI local-server (issue #95): recording stubs for the server sinks.
		int32 ServerStartCount = 0;
		int32 ServerStopCount = 0;
		bool bServerRunning = false;
	};

	static TSharedRef<FUnrealMcpEditorViewModel> DevCtlMakeViewModel(TSharedRef<FDevCtlRecording> Rec)
	{
		TSharedRef<FUnrealMcpEditorViewModel> VM = MakeShared<FUnrealMcpEditorViewModel>();
		VM->OnPersistConfig = [Rec](const FUnrealMcpConfig&) { Rec->PersistCount++; };
		VM->OnPushConfig = [Rec](const FUnrealMcpConfig&) { Rec->PushCount++; };
		VM->OnSendAuth = [Rec](const FString& Type) -> bool { Rec->AuthSent.Add(Type); return true; };
		VM->OnOpenBrowser = [](const FString&) {};
		VM->OnStartLocalServer = [Rec]() -> bool { Rec->ServerStartCount++; Rec->bServerRunning = true; return true; };
		VM->OnStopLocalServer = [Rec]() { Rec->ServerStopCount++; Rec->bServerRunning = false; };
		VM->IsLocalServerRunningSink = [Rec]() -> bool { return Rec->bServerRunning; };
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
			// The Serialization Check tab id is exposed so the smoke test can assert the window exists + drive it.
			TestEqual("serializationCheckTabId", Result->GetStringField(TEXT("serializationCheckTabId")),
				FString(TEXT("UnrealMcpSerializationCheckWindow")));

			const TArray<TSharedPtr<FJsonValue>>* OutAgents = nullptr;
			TestTrue("aiAgents present", Result->TryGetArrayField(TEXT("aiAgents"), OutAgents) && OutAgents != nullptr);
			if (OutAgents)
			{
				TestEqual("aiAgents count", OutAgents->Num(), 1);
				if (OutAgents->Num() == 1)
					TestEqual("aiAgents[0]", (*OutAgents)[0]->AsString(), FString(TEXT("Claude Code")));
			}
			TestEqual("aiAgentCount", (int32)Result->GetNumberField(TEXT("aiAgentCount")), 1);
		});

		It("reports the transport / auth / device-auth / readout fields and the footer external links", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Stdio);
			VM->SetAuthOption(EUnrealMcpAuthOption::Required);
			VM->SetCustomToken(TEXT("super-secret-bearer-1234567890"));

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(TEXT("GET"), TEXT("/state"), nullptr, &VM.Get(), Result);
			TestEqual("status", Status, 200);

			// Transport + selectability mirror the §7 segmented control.
			TestEqual("transport", Result->GetStringField(TEXT("transport")), FString(TEXT("stdio")));
			TestTrue("transport selectable in Custom", Result->GetBoolField(TEXT("transportSelectable")));
			// Auth option + token presence; the raw token is NEVER reported (§8) — only the masked form.
			TestEqual("authOption", Result->GetStringField(TEXT("authOption")), FString(TEXT("required")));
			TestTrue("hasCustomToken", Result->GetBoolField(TEXT("hasCustomToken")));
			const FString MaskedInState = Result->GetStringField(TEXT("customTokenMasked"));
			TestFalse("masked token is not the raw value", MaskedInState.Equals(TEXT("super-secret-bearer-1234567890")));
			TestFalse("masked token leaks no raw substring", MaskedInState.Contains(TEXT("secret")));
			// Device-auth + connection readouts.
			TestEqual("deviceAuthState idle by default", Result->GetStringField(TEXT("deviceAuthState")), FString(TEXT("Idle")));
			TestFalse("no cloud token", Result->GetBoolField(TEXT("hasCloudToken")));
			// Status dot/label readouts the smoke test asserts instead of reading pixels.
			TestFalse("buttonLabel non-empty", Result->GetStringField(TEXT("buttonLabel")).IsEmpty());
			TestFalse("statusLabel non-empty", Result->GetStringField(TEXT("statusLabel")).IsEmpty());

			// The footer external links — assert intent (the same urls the Help/Bug/Star buttons launch).
			const TSharedPtr<FJsonObject>* Links = nullptr;
			TestTrue("externalLinks present", Result->TryGetObjectField(TEXT("externalLinks"), Links) && Links != nullptr);
			if (Links)
			{
				TestTrue("help link is discord", (*Links)->GetStringField(TEXT("help")).Contains(TEXT("discord.gg")));
				TestTrue("bug link is github issues", (*Links)->GetStringField(TEXT("bug")).Contains(TEXT("/issues")));
				TestTrue("star link is the repo", (*Links)->GetStringField(TEXT("star")).Contains(TEXT("IvanMurzak/Unreal-MCP")));
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

		It("forwards an optional aiAgents array into the view-model (issue #97 AI-agents status row)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/connection-status"),
				DevCtlBody(TEXT("{\"status\":\"Connected\",\"aiAgents\":[\"Claude Code\",\"Cursor\"]}")),
				&VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestTrue("ok", Result->GetBoolField(TEXT("ok")));
			TestEqual("aiAgentCount in response", (int32)Result->GetNumberField(TEXT("aiAgentCount")), 2);
			// The view-model's live list (which the §7 AI-agents status row + its rail dot read) reflects the inject.
			TestEqual("view-model agent count", VM->GetAiAgents().Num(), 2);
			if (VM->GetAiAgents().Num() == 2)
			{
				TestEqual("agent[0]", VM->GetAiAgents()[0], FString(TEXT("Claude Code")));
				TestEqual("agent[1]", VM->GetAiAgents()[1], FString(TEXT("Cursor")));
			}
		});

		It("clears the aiAgents list when a status omits the field (empty-state path)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			// First populate, then inject a status with no aiAgents key — the list must reset to empty so the
			// AI-agents status row falls back to its "No agents connected" empty state.
			TSharedRef<FJsonObject> First = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/connection-status"),
				DevCtlBody(TEXT("{\"status\":\"Connected\",\"aiAgents\":[\"Claude Code\"]}")), &VM.Get(), First);
			TestEqual("populated", VM->GetAiAgents().Num(), 1);

			TSharedRef<FJsonObject> Second = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/connection-status"),
				DevCtlBody(TEXT("{\"status\":\"Connected\"}")), &VM.Get(), Second);
			TestEqual("cleared to empty", VM->GetAiAgents().Num(), 0);
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

		It("click=check returns 200 (opens the Serialization Check window; headless no-op)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"check\"}")), &VM.Get(), Result);

			// `check` opens an aux window rather than mutating the view-model. Under -nullrhi automation the
			// tab invoke is a guarded no-op, but the route still succeeds (the action was dispatched).
			TestEqual("status", Status, 200);
			TestTrue("ok", Result->GetBoolField(TEXT("ok")));
			TestEqual("target echoed", Result->GetStringField(TEXT("target")), FString(TEXT("check")));
		});

		It("click=start (the MCP-server Start button) launches the local server in Custom+http (issue #95)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Http);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"start\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestEqual("server started (not Connect())", Rec->ServerStartCount, 1);
			TestTrue("serverRunning echoed true", Result->GetBoolField(TEXT("serverRunning")));
			TestTrue("serverLaunchable echoed true", Result->GetBoolField(TEXT("serverLaunchable")));
			// The Start button must NOT touch the SignalR connection (that is the "Unreal:" row's Connect).
			// Connect() would optimistically drive the state to Connecting; the server-start path must leave the
			// SignalR connection state untouched (Disconnected here — never started).
			TestEqual("did not drive the SignalR connection (state untouched)",
				VM->GetConnectionState(), EUnrealMcpConnectionState::Disconnected);
		});

		It("click=start-server then stop-server drives a clean server start/stop cycle (Custom+http)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Http);

			TSharedRef<FJsonObject> R1 = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"start-server\"}")), &VM.Get(), R1);
			TestEqual("started once", Rec->ServerStartCount, 1);
			TestTrue("running after start-server", R1->GetBoolField(TEXT("serverRunning")));

			// Idempotent: a repeat start-server while running does NOT stop it.
			TSharedRef<FJsonObject> R2 = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"start-server\"}")), &VM.Get(), R2);
			TestEqual("start-server idempotent (no extra start)", Rec->ServerStartCount, 1);
			TestEqual("start-server idempotent (no stop)", Rec->ServerStopCount, 0);

			TSharedRef<FJsonObject> R3 = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"stop-server\"}")), &VM.Get(), R3);
			TestEqual("stopped once", Rec->ServerStopCount, 1);
			TestFalse("not running after stop-server", R3->GetBoolField(TEXT("serverRunning")));
		});

		It("click=start-server is a NO-OP that never spawns a server in Cloud / Custom+stdio (gating)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			// Custom + stdio: gated out.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Stdio);
			TSharedRef<FJsonObject> R1 = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"start-server\"}")), &VM.Get(), R1);
			TestEqual("no start in Custom+stdio", Rec->ServerStartCount, 0);
			TestFalse("serverRunning false in Custom+stdio", R1->GetBoolField(TEXT("serverRunning")));
			TestFalse("serverLaunchable false in Custom+stdio", R1->GetBoolField(TEXT("serverLaunchable")));

			// Cloud: gated out.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Cloud);
			TSharedRef<FJsonObject> R2 = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"start-server\"}")), &VM.Get(), R2);
			TestEqual("no start in Cloud", Rec->ServerStartCount, 0);
			TestFalse("serverRunning false in Cloud", R2->GetBoolField(TEXT("serverRunning")));
		});

		It("click=stop (the tri-state Stop button) halts the reconnect loop", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->Connect();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"stop\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestFalse("reconnect disarmed", VM->IsReconnectArmed());
			TestEqual("state", VM->GetConnectionState(), EUnrealMcpConnectionState::Disconnected);
		});

		It("click=authorize then click=cancel drives the device-auth flow back to Idle", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> AuthResult = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"authorize\"}")), &VM.Get(), AuthResult);
			TestEqual("pending after authorize", VM->GetDeviceAuthState(), EUnrealMcpDeviceAuthState::Pending);
			TestTrue("auth-start sent", Rec->AuthSent.Contains(TEXT("auth-start")));

			TSharedRef<FJsonObject> CancelResult = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"cancel\"}")), &VM.Get(), CancelResult);
			TestEqual("status", Status, 200);
			TestEqual("idle after cancel (no stored token)", VM->GetDeviceAuthState(), EUnrealMcpDeviceAuthState::Idle);
		});

		It("click=restart-bridge / open-log are acknowledged intents (no view-model mutation)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			const int32 PushBefore = Rec->PushCount;

			TSharedRef<FJsonObject> R1 = MakeShared<FJsonObject>();
			TestEqual("restart-bridge status", FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"restart-bridge\"}")), &VM.Get(), R1), 200);
			TestTrue("restart-bridge ok", R1->GetBoolField(TEXT("ok")));

			TSharedRef<FJsonObject> R2 = MakeShared<FJsonObject>();
			TestEqual("open-log status", FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/click"), DevCtlBody(TEXT("{\"target\":\"open-log\"}")), &VM.Get(), R2), 200);
			TestTrue("open-log ok", R2->GetBoolField(TEXT("ok")));

			// Neither launches a side effect through the view-model (no config push).
			TestEqual("no config push for intent-only clicks", Rec->PushCount, PushBefore);
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

	Describe("Transport / auth-option / auth-token routes (§7 segmented controls + token field)", [this]()
	{
		It("transport drives SetTransportMethod and echoes the effective transport", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/transport"), DevCtlBody(TEXT("{\"transport\":\"http\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestEqual("effective transport", VM->GetEffectiveTransport(), EUnrealMcpTransportMethod::Http);
			TestEqual("echoed transport", Result->GetStringField(TEXT("transport")), FString(TEXT("http")));
		});

		It("transport is locked to http in Cloud mode (echo reflects the lock)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Cloud);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/transport"), DevCtlBody(TEXT("{\"transport\":\"stdio\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			// Cloud forces Http and the selector is not user-editable — the stray stdio set is ignored.
			TestEqual("echoed transport stays http", Result->GetStringField(TEXT("transport")), FString(TEXT("http")));
			TestFalse("not selectable in Cloud", Result->GetBoolField(TEXT("transportSelectable")));
		});

		It("rejects an invalid transport with 400", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/transport"), DevCtlBody(TEXT("{\"transport\":\"carrier-pigeon\"}")), &VM.Get(), Result);
			TestEqual("status", Status, 400);
		});

		It("auth-option drives SetAuthOption", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/auth-option"), DevCtlBody(TEXT("{\"option\":\"required\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestEqual("auth option", VM->GetAuthOption(), EUnrealMcpAuthOption::Required);
			TestEqual("echoed auth option", Result->GetStringField(TEXT("authOption")), FString(TEXT("required")));
		});

		It("rejects an invalid auth-option with 400", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/auth-option"), DevCtlBody(TEXT("{\"option\":\"maybe\"}")), &VM.Get(), Result);
			TestEqual("status", Status, 400);
		});

		It("auth-token drives SetCustomToken and reports only the masked form (§8)", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/auth-token"), DevCtlBody(TEXT("{\"token\":\"super-secret-bearer-1234567890\"}")), &VM.Get(), Result);

			TestEqual("status", Status, 200);
			TestEqual("token stored in view-model", VM->GetCustomToken(), FString(TEXT("super-secret-bearer-1234567890")));
			TestTrue("hasCustomToken", Result->GetBoolField(TEXT("hasCustomToken")));
			// The raw token is NEVER reported back over the dev-control wire.
			const FString Masked = Result->GetStringField(TEXT("customTokenMasked"));
			TestFalse("masked != raw", Masked.Equals(TEXT("super-secret-bearer-1234567890")));
			TestFalse("masked leaks no raw substring", Masked.Contains(TEXT("secret")));
		});

		It("rejects a missing auth-token with 400", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/auth-token"), DevCtlBody(TEXT("{}")), &VM.Get(), Result);
			TestEqual("status", Status, 400);
		});
	});

	Describe("External-link intent (assert intent, never launched)", [this]()
	{
		It("resolves help / bug / star to their urls without launching a browser", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> Help = MakeShared<FJsonObject>();
			TestEqual("help status", FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/external-link"), DevCtlBody(TEXT("{\"link\":\"help\"}")), &VM.Get(), Help), 200);
			TestTrue("help url is discord", Help->GetStringField(TEXT("url")).Contains(TEXT("discord.gg")));
			TestFalse("help not launched", Help->GetBoolField(TEXT("launched")));

			TSharedRef<FJsonObject> Bug = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/external-link"), DevCtlBody(TEXT("{\"link\":\"bug\"}")), &VM.Get(), Bug);
			TestTrue("bug url is github issues", Bug->GetStringField(TEXT("url")).Contains(TEXT("/issues")));

			TSharedRef<FJsonObject> Star = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/external-link"), DevCtlBody(TEXT("{\"link\":\"star\"}")), &VM.Get(), Star);
			TestTrue("star url is the repo", Star->GetStringField(TEXT("url")).Contains(TEXT("IvanMurzak/Unreal-MCP")));
		});

		It("rejects an unknown external-link with 400", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/control/external-link"), DevCtlBody(TEXT("{\"link\":\"facebook\"}")), &VM.Get(), Result);
			TestEqual("status", Status, 400);
		});
	});

	Describe("Inject device-auth (the Cloud device-code rows)", [this]()
	{
		It("injects a pending then authorized device-auth feed so /state surfaces the cloud token", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			// A pending device-auth only takes effect after Authorize armed the flow (the cancel-race guard).
			VM->Authorize();

			TSharedRef<FJsonObject> Pending = MakeShared<FJsonObject>();
			const int32 PendingStatus = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/device-auth"),
				DevCtlBody(TEXT("{\"state\":\"pending\",\"verificationUrl\":\"https://ai-game.dev/device\",\"userCode\":\"WXYZ-1234\"}")),
				&VM.Get(), Pending);
			TestEqual("pending status", PendingStatus, 200);
			TestEqual("pending echoed", Pending->GetStringField(TEXT("deviceAuthState")), FString(TEXT("Pending")));
			TestEqual("user code surfaced", VM->GetDeviceUserCode(), FString(TEXT("WXYZ-1234")));

			TSharedRef<FJsonObject> Authorized = MakeShared<FJsonObject>();
			const int32 AuthStatus = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/device-auth"),
				DevCtlBody(TEXT("{\"state\":\"authorized\",\"token\":\"cloud-bearer-abc\"}")), &VM.Get(), Authorized);
			TestEqual("authorized status", AuthStatus, 200);
			TestTrue("hasCloudToken echoed", Authorized->GetBoolField(TEXT("hasCloudToken")));
			TestTrue("cloud token stored", VM->HasCloudToken());

			// /state now reflects the authorized device-auth + stored cloud token.
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			FUnrealMcpDevControlServer::RouteRequest(TEXT("GET"), TEXT("/state"), nullptr, &VM.Get(), State);
			TestEqual("state deviceAuthState", State->GetStringField(TEXT("deviceAuthState")), FString(TEXT("Authorized")));
			TestTrue("state hasCloudToken", State->GetBoolField(TEXT("hasCloudToken")));
		});

		It("injects a failed device-auth so /state surfaces the Failed indicator", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);

			TSharedRef<FJsonObject> Failed = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/device-auth"),
				DevCtlBody(TEXT("{\"state\":\"failed\",\"message\":\"Authorization was denied.\"}")), &VM.Get(), Failed);
			TestEqual("status", Status, 200);
			TestEqual("failed state", VM->GetDeviceAuthState(), EUnrealMcpDeviceAuthState::Failed);
			TestEqual("failure reason surfaced", VM->GetDeviceAuthError(), FString(TEXT("Authorization was denied.")));
		});

		It("rejects a missing device-auth state with 400", [this]()
		{
			TSharedRef<FDevCtlRecording> Rec = MakeShared<FDevCtlRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = DevCtlMakeViewModel(Rec);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const int32 Status = FUnrealMcpDevControlServer::RouteRequest(
				TEXT("POST"), TEXT("/inject/device-auth"), DevCtlBody(TEXT("{}")), &VM.Get(), Result);
			TestEqual("status", Status, 400);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
