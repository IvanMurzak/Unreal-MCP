// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "Config/UnrealMcpConfig.h"

/**
 * View-model specs (docs/ARCHITECTURE.md §7): the connection-state machine, Custom-mode URL validation, the
 * token-masking guard (§8), the Connect/Disconnect tri-state + the M9b "Disconnect genuinely halts reconnect"
 * rule, the config-store round-trip the UI drives, and the `status` / `device-auth` IPC-feed application —
 * all without a live bridge, editor world, or real files (the side-effect sinks are recording stubs).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpEditorViewModelSpec, "UnrealMcp.EditorViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// A view-model wired with recording sinks so a spec can assert what the UI persisted / pushed / sent.
	struct FRecording
	{
		int32 PersistCount = 0;
		int32 PushCount = 0;
		TArray<FString> AuthSent;
		TArray<FString> OpenedUrls;
		FUnrealMcpConfig LastPushed;
	};

	static TSharedRef<FUnrealMcpEditorViewModel> MakeViewModel(TSharedRef<FRecording> Rec)
	{
		TSharedRef<FUnrealMcpEditorViewModel> VM = MakeShared<FUnrealMcpEditorViewModel>();
		VM->OnPersistConfig = [Rec](const FUnrealMcpConfig&) { Rec->PersistCount++; };
		VM->OnPushConfig = [Rec](const FUnrealMcpConfig& Cfg) { Rec->PushCount++; Rec->LastPushed = Cfg; };
		VM->OnSendAuth = [Rec](const FString& Type) -> bool { Rec->AuthSent.Add(Type); return true; };
		VM->OnOpenBrowser = [Rec](const FString& Url) { Rec->OpenedUrls.Add(Url); };
		return VM;
	}

END_DEFINE_SPEC(FUnrealMcpEditorViewModelSpec)

void FUnrealMcpEditorViewModelSpec::Define()
{
	Describe("Server URL validation", [this]()
	{
		It("accepts well-formed http/https URLs and rejects malformed ones", [this]()
		{
			FString Error;
			TestTrue("http localhost", FUnrealMcpEditorViewModel::ValidateServerUrl(TEXT("http://localhost:8080"), Error));
			TestTrue("https host", FUnrealMcpEditorViewModel::ValidateServerUrl(TEXT("https://ai-game.dev"), Error));
			TestTrue("http with path", FUnrealMcpEditorViewModel::ValidateServerUrl(TEXT("http://127.0.0.1:5244/mcp"), Error));

			TestFalse("empty", FUnrealMcpEditorViewModel::ValidateServerUrl(TEXT(""), Error));
			TestFalse("no scheme", FUnrealMcpEditorViewModel::ValidateServerUrl(TEXT("localhost:8080"), Error));
			TestFalse("ftp scheme", FUnrealMcpEditorViewModel::ValidateServerUrl(TEXT("ftp://host"), Error));
			TestFalse("scheme but no host", FUnrealMcpEditorViewModel::ValidateServerUrl(TEXT("http://"), Error));
			TestFalse("scheme then slash", FUnrealMcpEditorViewModel::ValidateServerUrl(TEXT("http:///path"), Error));
		});
	});

	Describe("Token masking (§8)", [this]()
	{
		It("never renders the raw token unless explicitly revealed, and never leaks length", [this]()
		{
			const FString Secret = TEXT("super-secret-bearer-1234567890");
			const FString Masked = FUnrealMcpEditorViewModel::MaskTokenForDisplay(Secret, /*bReveal*/ false);
			TestFalse("masked != raw", Masked.Equals(Secret));
			TestFalse("masked contains no raw substring", Masked.Contains(TEXT("secret")));
			// Fixed-width mask: length must NOT equal the secret length.
			TestNotEqual("masked length is fixed, not secret length", Masked.Len(), Secret.Len());

			TestEqual("reveal returns raw", FUnrealMcpEditorViewModel::MaskTokenForDisplay(Secret, true), Secret);
			TestEqual("empty stays empty", FUnrealMcpEditorViewModel::MaskTokenForDisplay(FString(), false), FString());
		});
	});

	Describe("Status tri-state presentation", [this]()
	{
		It("maps each connection state to a distinct button label, status label and colour", [this]()
		{
			TestEqual("Disconnected button", FUnrealMcpEditorViewModel::GetButtonText(EUnrealMcpConnectionState::Disconnected).ToString(), FString(TEXT("Connect")));
			TestEqual("Connected button", FUnrealMcpEditorViewModel::GetButtonText(EUnrealMcpConnectionState::Connected).ToString(), FString(TEXT("Disconnect")));
			TestEqual("Connecting button", FUnrealMcpEditorViewModel::GetButtonText(EUnrealMcpConnectionState::Connecting).ToString(), FString(TEXT("Stop")));

			// Colours differ between connected (green) and disconnected (red).
			const FLinearColor Connected = FUnrealMcpEditorViewModel::GetStatusColor(EUnrealMcpConnectionState::Connected);
			const FLinearColor Disconnected = FUnrealMcpEditorViewModel::GetStatusColor(EUnrealMcpConnectionState::Disconnected);
			TestFalse("connected colour != disconnected colour", Connected.Equals(Disconnected));
		});

		It("treats a reported Disconnected-while-armed as Degraded, not a true stop", [this]()
		{
			TestEqual("armed Disconnected -> Degraded",
				static_cast<int32>(FUnrealMcpEditorViewModel::ParseConnectionState(TEXT("Disconnected"), /*keep*/ true)),
				static_cast<int32>(EUnrealMcpConnectionState::Degraded));
			TestEqual("unarmed Disconnected -> Disconnected",
				static_cast<int32>(FUnrealMcpEditorViewModel::ParseConnectionState(TEXT("Disconnected"), /*keep*/ false)),
				static_cast<int32>(EUnrealMcpConnectionState::Disconnected));
			TestEqual("Connected -> Connected",
				static_cast<int32>(FUnrealMcpEditorViewModel::ParseConnectionState(TEXT("Connected"), true)),
				static_cast<int32>(EUnrealMcpConnectionState::Connected));
		});
	});

	Describe("Connect / Disconnect (the M9b reconnect-halt rule)", [this]()
	{
		It("Connect arms keepConnected and pushes; Disconnect disarms it and pushes keepConnected=false", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			VM->Connect();
			TestTrue("armed after Connect", VM->IsReconnectArmed());
			TestEqual("Connecting state after Connect", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connecting));
			TestTrue("pushed at least once", Rec->PushCount >= 1);
			TestTrue("last pushed keepConnected=true", Rec->LastPushed.bKeepConnected);

			VM->Disconnect();
			TestFalse("disarmed after Disconnect", VM->IsReconnectArmed());
			TestEqual("Disconnected state after Disconnect", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Disconnected));
			TestFalse("last pushed keepConnected=false", Rec->LastPushed.bKeepConnected);
		});
	});

	Describe("Config-store round-trip the UI drives", [this]()
	{
		It("mode / valid-host / auth / generated-token changes persist and push; invalid host does not push", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			const int32 PushAfterMode = Rec->PushCount;
			TestTrue("mode change pushed", PushAfterMode >= 1);

			// Invalid host: field updates but no push.
			VM->SetCustomHost(TEXT("not-a-url"));
			TestEqual("invalid host did not push", Rec->PushCount, PushAfterMode);
			TestEqual("field still updated", VM->GetCustomHost(), FString(TEXT("not-a-url")));

			// Valid host: pushes.
			VM->SetCustomHost(TEXT("http://localhost:5244"));
			TestTrue("valid host pushed", Rec->PushCount > PushAfterMode);

			VM->GenerateCustomToken();
			TestFalse("token generated", VM->GetCustomToken().IsEmpty());
			TestEqual("generating flips auth to Required", static_cast<int32>(VM->GetAuthOption()), static_cast<int32>(EUnrealMcpAuthOption::Required));
			TestTrue("persisted at least once", Rec->PersistCount >= 1);
		});
	});

	Describe("Connection-settings change notification (§7 — the InvalidateAndReloadAgentUI seam, issue #56)", [this]()
	{
		It("fires OnConnectionSettingsChanged when the mode / host / auth / token change so the agent panel can re-resolve", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			int32 Notifications = 0;
			FDelegateHandle Handle = VM->OnConnectionSettingsChanged.AddLambda([&Notifications]() { Notifications++; });

			// Mode change Cloud (default) -> Custom: one notification.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			TestEqual("mode change notified", Notifications, 1);

			// A no-op mode set (already Custom) must NOT notify — avoid spurious panel churn.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			TestEqual("no-op mode change did not notify", Notifications, 1);

			// Host change (even an invalid one) refreshes the previewed url.
			VM->SetCustomHost(TEXT("http://localhost:9100"));
			TestEqual("host change notified", Notifications, 2);

			// A no-op host set (same value) must NOT notify.
			VM->SetCustomHost(TEXT("http://localhost:9100"));
			TestEqual("no-op host change did not notify", Notifications, 2);

			// Auth option change toggles whether the snippet carries a bearer.
			VM->SetAuthOption(EUnrealMcpAuthOption::Required);
			TestEqual("auth change notified", Notifications, 3);

			// Token change is injected into the snippet/Configure output.
			VM->SetCustomToken(TEXT("fresh-token"));
			TestEqual("token change notified", Notifications, 4);

			// A no-op token set (same value) must NOT notify.
			VM->SetCustomToken(TEXT("fresh-token"));
			TestEqual("no-op token change did not notify", Notifications, 4);

			VM->OnConnectionSettingsChanged.Remove(Handle);
		});

		It("does NOT notify after the subscriber unbinds (no dangling-handler refresh)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			int32 Notifications = 0;
			FDelegateHandle Handle = VM->OnConnectionSettingsChanged.AddLambda([&Notifications]() { Notifications++; });
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			TestEqual("notified while bound", Notifications, 1);

			VM->OnConnectionSettingsChanged.Remove(Handle);
			VM->SetCustomHost(TEXT("http://localhost:9200"));
			TestEqual("no notification after unbind", Notifications, 1);
		});
	});

	Describe("Cloud device-code auth", [this]()
	{
		It("Authorize sends auth-start; device-auth feed opens the browser once and stores the token", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			VM->Authorize();
			TestTrue("auth-start sent", Rec->AuthSent.Contains(TEXT("auth-start")));
			TestEqual("pending state", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Pending));

			TSharedPtr<FJsonObject> Pending = MakeShared<FJsonObject>();
			Pending->SetStringField(TEXT("verificationUrl"), TEXT("https://ai-game.dev/device"));
			Pending->SetStringField(TEXT("userCode"), TEXT("WXYZ-1234"));
			Pending->SetStringField(TEXT("state"), TEXT("pending"));
			VM->ApplyDeviceAuth(Pending);
			TestEqual("user code surfaced", VM->GetDeviceUserCode(), FString(TEXT("WXYZ-1234")));
			TestEqual("browser opened once", Rec->OpenedUrls.Num(), 1);

			// A second pending update with the same URL must NOT re-open the browser.
			VM->ApplyDeviceAuth(Pending);
			TestEqual("browser not re-opened", Rec->OpenedUrls.Num(), 1);

			const int32 PersistBeforeAuth = Rec->PersistCount;
			const int32 PushBeforeAuth = Rec->PushCount;
			TSharedPtr<FJsonObject> Authorized = MakeShared<FJsonObject>();
			Authorized->SetStringField(TEXT("state"), TEXT("authorized"));
			Authorized->SetStringField(TEXT("token"), TEXT("cloud-bearer-abc"));
			VM->ApplyDeviceAuth(Authorized);
			TestEqual("authorized state", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Authorized));
			TestTrue("cloud token stored", VM->HasCloudToken());
			// The authorized token MUST be persisted to the §8 store AND pushed into the bridge's effective
			// config — otherwise it is lost on restart / a "Restart bridge" re-pushes a token-less config.
			TestTrue("authorize persisted the token", Rec->PersistCount > PersistBeforeAuth);
			TestTrue("authorize pushed the token", Rec->PushCount > PushBeforeAuth);
			TestEqual("pushed config carries the cloud token", Rec->LastPushed.CloudToken, FString(TEXT("cloud-bearer-abc")));
		});

		It("Authorize stays out of Pending when the auth-start frame cannot be sent (no sidecar)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			// A sink that reports the frame could not be delivered (no connected/handshaken sidecar).
			VM->OnSendAuth = [Rec](const FString& Type) -> bool { Rec->AuthSent.Add(Type); return false; };

			VM->Authorize();
			TestTrue("auth-start attempted", Rec->AuthSent.Contains(TEXT("auth-start")));
			// Must NOT wedge in a code-less Pending state — fall to Failed so the UI does not show an empty code.
			TestEqual("not pending when send failed", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Failed));
			// And surface a reason so the window shows feedback rather than silently collapsing.
			TestEqual("no-sidecar reason surfaced", VM->GetDeviceAuthError(), FString(TEXT("No sidecar connected.")));
		});

		It("surfaces a failed device-auth message and clears it on a fresh Authorize", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// A terminal failed device-auth carries the sidecar's human-readable reason (DeviceAuthMessage.Message)
			// the UI must surface — without it the pending instructions collapse and the window shows nothing.
			TSharedPtr<FJsonObject> Failed = MakeShared<FJsonObject>();
			Failed->SetStringField(TEXT("state"), TEXT("failed"));
			Failed->SetStringField(TEXT("message"), TEXT("Authorization was denied."));
			VM->ApplyDeviceAuth(Failed);
			TestEqual("failed state", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Failed));
			TestEqual("failure reason surfaced", VM->GetDeviceAuthError(), FString(TEXT("Authorization was denied.")));

			// A fresh Authorize clears the stale failure reason before starting the new flow.
			VM->Authorize();
			TestTrue("error cleared on re-authorize", VM->GetDeviceAuthError().IsEmpty());
		});

		It("Revoke clears the cloud token, sends auth-revoke and pushes the now-anonymous config", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// Drive a realistic flow: Authorize (→ Pending) THEN the sidecar's authorized frame stores the token.
			// (An authorized device-auth only ever arrives after auth-start; applying it straight to an Idle VM
			// is now ignored by the cancel-race complement, so the spec mirrors the real ordering.)
			VM->Authorize();
			TSharedPtr<FJsonObject> Authorized = MakeShared<FJsonObject>();
			Authorized->SetStringField(TEXT("state"), TEXT("authorized"));
			Authorized->SetStringField(TEXT("token"), TEXT("cloud-bearer-abc"));
			VM->ApplyDeviceAuth(Authorized);
			TestTrue("token present before revoke", VM->HasCloudToken());

			VM->Revoke();
			TestFalse("token cleared after revoke", VM->HasCloudToken());
			TestTrue("auth-revoke sent", Rec->AuthSent.Contains(TEXT("auth-revoke")));
		});

		It("ignores an authorized device-auth that races in after the user cancelled the flow", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// First-time flow (no stored token yet): Authorize then cancel it before authorization completes.
			VM->Authorize();
			TestEqual("pending after Authorize", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Pending));
			VM->CancelAuth();
			TestEqual("idle after cancel (no stored token)", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Idle));

			const int32 PushBefore = Rec->PushCount;
			// The sidecar's authorized frame (emitted before its own cancel guard ran) races in AFTER the cancel.
			TSharedPtr<FJsonObject> Authorized = MakeShared<FJsonObject>();
			Authorized->SetStringField(TEXT("state"), TEXT("authorized"));
			Authorized->SetStringField(TEXT("token"), TEXT("cloud-bearer-late"));
			VM->ApplyDeviceAuth(Authorized);

			// Cancel-race complement: a just-cancelled flow (Idle) ignores the late authorized — the dropped token
			// is NOT resurrected, the indicator stays Idle, and nothing is persisted/pushed.
			TestEqual("still idle, late authorized ignored", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Idle));
			TestFalse("no token resurrected", VM->HasCloudToken());
			TestEqual("nothing pushed for the ignored authorized", Rec->PushCount, PushBefore);
		});

		It("CancelAuth keeps the Authorized indicator when a cloud token is already stored", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// Establish a stored cloud token (a completed prior authorization, via the realistic Authorize flow).
			VM->Authorize();
			TSharedPtr<FJsonObject> Authorized = MakeShared<FJsonObject>();
			Authorized->SetStringField(TEXT("state"), TEXT("authorized"));
			Authorized->SetStringField(TEXT("token"), TEXT("cloud-bearer-abc"));
			VM->ApplyDeviceAuth(Authorized);
			TestTrue("token stored", VM->HasCloudToken());

			// The user starts a RE-authorize then cancels: the indicator must fall back to Authorized (a bearer is
			// still stored), not Idle — otherwise the Authorized/Revoke affordance vanishes until restart.
			VM->Authorize();
			VM->CancelAuth();
			TestEqual("authorized indicator preserved on cancel-with-token",
				static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Authorized));
			TestTrue("token still stored", VM->HasCloudToken());
		});
	});

	Describe("Custom-mode host trimming (§7 validated field)", [this]()
	{
		It("trims surrounding whitespace before storing and pushing the dial target", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// A padded-but-valid URL must validate AND be stored/pushed trimmed — the sidecar must not dial the
			// untrimmed target.
			VM->SetCustomHost(TEXT("  http://localhost:5244  "));
			TestEqual("stored host is trimmed", VM->GetCustomHost(), FString(TEXT("http://localhost:5244")));
			TestTrue("trimmed valid host pushed", Rec->PushCount >= 1);
			TestEqual("pushed dial target is trimmed", Rec->LastPushed.CustomHost, FString(TEXT("http://localhost:5244")));
		});
	});

	Describe("Status feed application", [this]()
	{
		It("applies connectionState and aiAgents from a status message", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			TArray<TSharedPtr<FJsonValue>> Agents;
			Agents.Add(MakeShared<FJsonValueString>(TEXT("Claude Code")));
			Agents.Add(MakeShared<FJsonValueString>(TEXT("Cursor")));
			Status->SetArrayField(TEXT("aiAgents"), Agents);

			VM->ApplyStatus(Status);
			TestEqual("connected", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));
			TestEqual("two agents", VM->GetAiAgents().Num(), 2);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
