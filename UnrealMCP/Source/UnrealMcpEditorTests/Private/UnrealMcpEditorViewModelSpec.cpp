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
		// Issue #99: the recording hook for "ensure sidecar started". EnsureSidecarStartedCount tracks how many
		// times Authorize asked the runtime to (re)start the bridge; bSidecarCanStart is the fake binary-resolves
		// result (true → a handshake is plausibly imminent → enter Connecting+queue; false → actionable Failed).
		// bSidecarConnected is the fake live send-result the OnSendAuth stub reads: while false, auth-start sends
		// fail (sidecar not connected yet), so Authorize takes the queue-and-await path; flipping it true then
		// firing NotifySidecarHandshakeComplete simulates the bridge handshaking and flushes the queued frame.
		int32 EnsureSidecarStartedCount = 0;
		bool bSidecarCanStart = true;
		bool bSidecarConnected = true;
		// §7 in-UI local-server (issue #95): the recording stubs for the server sinks. bServerRunning is the
		// fake live state the IsLocalServerRunningSink reads; OnStart flips it true, OnStop flips it false.
		int32 ServerStartCount = 0;
		int32 ServerStopCount = 0;
		bool bServerRunning = false;
		// Issue #63: the fake "bridge binary resolvable" state the IsBridgeBinaryResolvableSink reads. Default true
		// (resolvable) so every existing spec sees the raw connection state through GetEffectiveConnectionState; a
		// NoBinary spec flips it false to assert the proactive overlay.
		bool bBridgeBinaryResolvable = true;
	};

	static TSharedRef<FUnrealMcpEditorViewModel> MakeViewModel(TSharedRef<FRecording> Rec)
	{
		TSharedRef<FUnrealMcpEditorViewModel> VM = MakeShared<FUnrealMcpEditorViewModel>();
		VM->OnPersistConfig = [Rec](const FUnrealMcpConfig&) { Rec->PersistCount++; };
		VM->OnPushConfig = [Rec](const FUnrealMcpConfig& Cfg) { Rec->PushCount++; Rec->LastPushed = Cfg; };
		// auth-start succeeds only when the fake sidecar is connected (issue #99 robust path); auth-cancel/revoke
		// always "send" (they are best-effort over whatever connection exists). This lets a spec start disconnected
		// (auth-start queues) then flip bSidecarConnected + fire the handshake to flush.
		VM->OnSendAuth = [Rec](const FString& Type) -> bool
		{
			Rec->AuthSent.Add(Type);
			if (Type == TEXT("auth-start"))
				return Rec->bSidecarConnected;
			return true;
		};
		VM->OnEnsureSidecarStarted = [Rec]() -> bool { Rec->EnsureSidecarStartedCount++; return Rec->bSidecarCanStart; };
		VM->OnOpenBrowser = [Rec](const FString& Url) { Rec->OpenedUrls.Add(Url); };
		VM->OnStartLocalServer = [Rec]() -> bool { Rec->ServerStartCount++; Rec->bServerRunning = true; return true; };
		VM->OnStopLocalServer = [Rec]() { Rec->ServerStopCount++; Rec->bServerRunning = false; };
		VM->IsLocalServerRunningSink = [Rec]() -> bool { return Rec->bServerRunning; };
		VM->IsBridgeBinaryResolvableSink = [Rec]() -> bool { return Rec->bBridgeBinaryResolvable; };
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

	Describe("Proactive 'no sidecar binary' surfacing (issue #63)", [this]()
	{
		It("leaves the effective state as the raw state and shows NO hint while the bridge binary resolves", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec); // bBridgeBinaryResolvable defaults true
			// A fresh VM is Disconnected; with a resolvable binary the effective state equals the raw state.
			TestEqual("effective == raw (Disconnected) while resolvable",
				static_cast<int32>(VM->GetEffectiveConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Disconnected));
			TestTrue("no hint while resolvable",
				FUnrealMcpEditorViewModel::GetConnectionHint(VM->GetEffectiveConnectionState()).IsEmpty());
		});

		It("surfaces a distinct NoBinary state with an actionable hint when the binary cannot be resolved", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			Rec->bBridgeBinaryResolvable = false; // packaged-without-<rid> / unset UNREAL_MCP_BRIDGE_PATH (dev source)
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// The raw live state is still the unchanged Disconnected — only the EFFECTIVE display state overlays NoBinary.
			TestEqual("raw state unchanged (Disconnected)",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Disconnected));
			TestEqual("effective state overlays NoBinary",
				static_cast<int32>(VM->GetEffectiveConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::NoBinary));

			// The label is DISTINCT from the generic Disconnected one ("not just 'Stopped'", per the issue).
			TestNotEqual("NoBinary label differs from Disconnected",
				FUnrealMcpEditorViewModel::GetStatusLabel(EUnrealMcpConnectionState::NoBinary).ToString(),
				FUnrealMcpEditorViewModel::GetStatusLabel(EUnrealMcpConnectionState::Disconnected).ToString());

			// The hint is non-empty and actionable — the SAME install guidance the reactive Authorize failure shows.
			const FText Hint = FUnrealMcpEditorViewModel::GetConnectionHint(VM->GetEffectiveConnectionState());
			TestFalse("hint is non-empty in NoBinary", Hint.IsEmpty());
			TestTrue("hint is actionable (reinstall / bootstrap-local)",
				Hint.ToString().Contains(TEXT("bootstrap-local")) || Hint.ToString().Contains(TEXT("reinstall")));
		});

		It("distinguishes NoBinary (install/config) from a transient Connecting (binary present, not yet handshaken)", [this]()
		{
			// Binary present: clicking Connect yields a transient Connecting, NOT NoBinary.
			TSharedRef<FRecording> RecOk = MakeShared<FRecording>(); // resolvable
			TSharedRef<FUnrealMcpEditorViewModel> VmOk = MakeViewModel(RecOk);
			VmOk->Connect();
			TestEqual("Connecting (not NoBinary) when the binary is present",
				static_cast<int32>(VmOk->GetEffectiveConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connecting));
			TestTrue("no hint while genuinely Connecting",
				FUnrealMcpEditorViewModel::GetConnectionHint(VmOk->GetEffectiveConnectionState()).IsEmpty());

			// Binary absent: the same Connect can never complete, so the effective state is NoBinary, not a perpetual
			// Connecting — the user sees the install/config problem instead of a forever-spinner.
			TSharedRef<FRecording> RecNo = MakeShared<FRecording>();
			RecNo->bBridgeBinaryResolvable = false;
			TSharedRef<FUnrealMcpEditorViewModel> VmNo = MakeViewModel(RecNo);
			VmNo->Connect();
			TestEqual("raw Connecting while armed",
				static_cast<int32>(VmNo->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connecting));
			TestEqual("effective NoBinary overlays the never-completing Connecting",
				static_cast<int32>(VmNo->GetEffectiveConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::NoBinary));
		});

		It("never masks a live Connected/Degraded link even if resolvability reads false", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			// Drive to a live green link the way the runtime does (arm + apply a Connected status).
			VM->Connect();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Status);
			TestEqual("Connected before the resolvability flip",
				static_cast<int32>(VM->GetEffectiveConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));

			// A Connected link PROVES a sidecar (hence a binary) exists — a stale "unresolvable" read must not mask it.
			Rec->bBridgeBinaryResolvable = false;
			TestEqual("still Connected — NoBinary never masks a live link",
				static_cast<int32>(VM->GetEffectiveConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));

			// Same guard for Degraded (armed + a reported Disconnected = background retry of a link that DID exist).
			TSharedPtr<FJsonObject> Dropped = MakeShared<FJsonObject>();
			Dropped->SetStringField(TEXT("connectionState"), TEXT("Disconnected"));
			Dropped->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Dropped);
			TestEqual("Degraded raw after an armed drop",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Degraded));
			TestEqual("Degraded is not overlaid by NoBinary",
				static_cast<int32>(VM->GetEffectiveConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Degraded));
		});

		It("emits a hint ONLY for NoBinary and a Connect button label for it", [this]()
		{
			// The hint line is rendered only in NoBinary — every other state yields an empty hint (no extra row).
			TestFalse("NoBinary hint non-empty", FUnrealMcpEditorViewModel::GetConnectionHint(EUnrealMcpConnectionState::NoBinary).IsEmpty());
			TestTrue("Disconnected hint empty", FUnrealMcpEditorViewModel::GetConnectionHint(EUnrealMcpConnectionState::Disconnected).IsEmpty());
			TestTrue("Connected hint empty", FUnrealMcpEditorViewModel::GetConnectionHint(EUnrealMcpConnectionState::Connected).IsEmpty());
			TestTrue("Connecting hint empty", FUnrealMcpEditorViewModel::GetConnectionHint(EUnrealMcpConnectionState::Connecting).IsEmpty());
			// The static button helper offers Connect as the NoBinary fallback (exhaustive-switch coverage).
			TestEqual("NoBinary button label is Connect",
				FUnrealMcpEditorViewModel::GetButtonText(EUnrealMcpConnectionState::NoBinary).ToString(), FString(TEXT("Connect")));
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

		It("Authorize enters Connecting and QUEUES auth-start when the sidecar is not connected yet (issue #99)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			// Sidecar is NOT connected yet, but its binary CAN start — the robust path: request a (re)start, queue
			// the auth-start, and enter the transient Connecting state (NOT immediate Failed — the old behavior).
			Rec->bSidecarConnected = false;
			Rec->bSidecarCanStart = true;

			VM->Authorize(/*NowSeconds*/ 100.0);
			TestTrue("auth-start attempted", Rec->AuthSent.Contains(TEXT("auth-start")));
			TestEqual("ensure-sidecar-started requested once", Rec->EnsureSidecarStartedCount, 1);
			// Transient connecting/awaiting — NOT Failed, NOT a code-less Pending.
			TestEqual("connecting while awaiting handshake", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Connecting));
			// No premature failure message while we wait.
			TestTrue("no failure reason while connecting", VM->GetDeviceAuthError().IsEmpty());
		});

		It("flushes the queued auth-start on a simulated handshake-complete (issue #99)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			Rec->bSidecarConnected = false;
			Rec->bSidecarCanStart = true;

			VM->Authorize(/*NowSeconds*/ 0.0);
			TestEqual("connecting after Authorize", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Connecting));
			const int32 AuthStartsBeforeHandshake = Rec->AuthSent.FilterByPredicate([](const FString& T){ return T == TEXT("auth-start"); }).Num();

			// The sidecar handshakes: flip the fake connection true, then notify. The queued auth-start flushes and
			// we advance to Pending (the sidecar's device-auth feed then drives the verificationUrl / userCode).
			Rec->bSidecarConnected = true;
			VM->NotifySidecarHandshakeComplete();
			const int32 AuthStartsAfterHandshake = Rec->AuthSent.FilterByPredicate([](const FString& T){ return T == TEXT("auth-start"); }).Num();
			TestEqual("auth-start re-sent on handshake (flushed)", AuthStartsAfterHandshake, AuthStartsBeforeHandshake + 1);
			TestEqual("pending after flush", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Pending));

			// A second handshake notification must NOT re-fire the (already-flushed) auth-start.
			VM->NotifySidecarHandshakeComplete();
			const int32 AuthStartsAfterSecond = Rec->AuthSent.FilterByPredicate([](const FString& T){ return T == TEXT("auth-start"); }).Num();
			TestEqual("no double-flush on a second handshake", AuthStartsAfterSecond, AuthStartsAfterHandshake);
		});

		It("Cancel while awaiting (Connecting) clears the queue and the timeout (issue #99)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			Rec->bSidecarConnected = false;
			Rec->bSidecarCanStart = true;

			VM->Authorize(/*NowSeconds*/ 100.0);
			TestEqual("connecting after Authorize", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Connecting));

			VM->CancelAuth();
			TestEqual("idle after cancel (no stored token)", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Idle));
			TestTrue("auth-cancel sent", Rec->AuthSent.Contains(TEXT("auth-cancel")));

			// A late handshake after the cancel must NOT resurrect the cancelled auth-start (queue was cleared).
			Rec->bSidecarConnected = true;
			const int32 AuthStartsBefore = Rec->AuthSent.FilterByPredicate([](const FString& T){ return T == TEXT("auth-start"); }).Num();
			VM->NotifySidecarHandshakeComplete();
			const int32 AuthStartsAfter = Rec->AuthSent.FilterByPredicate([](const FString& T){ return T == TEXT("auth-start"); }).Num();
			TestEqual("cancelled queue not flushed by a late handshake", AuthStartsAfter, AuthStartsBefore);
			TestEqual("still idle after late handshake", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Idle));

			// And a later timeout tick must NOT revive it either.
			VM->TickAuthTimeout(/*NowSeconds*/ 1000.0);
			TestEqual("still idle after a post-cancel timeout tick", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Idle));
		});

		It("times out to an actionable Failed when the sidecar never connects (issue #99)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			Rec->bSidecarConnected = false;
			Rec->bSidecarCanStart = true;

			VM->Authorize(/*NowSeconds*/ 100.0);
			TestEqual("connecting after Authorize", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Connecting));

			// A tick well inside the bound is a no-op (still awaiting).
			VM->TickAuthTimeout(/*NowSeconds*/ 105.0);
			TestEqual("still connecting inside the timeout bound", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Connecting));

			// A tick past the bound transitions to Failed with the ACTIONABLE message (not the cryptic old one).
			VM->TickAuthTimeout(/*NowSeconds*/ 100.0 + 60.0);
			TestEqual("failed after the timeout", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Failed));
			TestTrue("actionable failure mentions installing the bridge",
				VM->GetDeviceAuthError().Contains(TEXT("bootstrap-local")) || VM->GetDeviceAuthError().Contains(TEXT("reinstall")));
			TestFalse("failure is NOT the cryptic No-sidecar-connected message", VM->GetDeviceAuthError() == TEXT("No sidecar connected."));
		});

		It("Authorize fails fast actionably when the bridge binary cannot start at all (issue #99)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			// Sidecar not connected AND its binary cannot be resolved (packaged-without-<rid> / unset dev override).
			Rec->bSidecarConnected = false;
			Rec->bSidecarCanStart = false;

			VM->Authorize(/*NowSeconds*/ 100.0);
			TestEqual("ensure-sidecar-started attempted once", Rec->EnsureSidecarStartedCount, 1);
			// No point awaiting a handshake that will never arrive — fail fast, actionably (never wedge).
			TestEqual("failed fast when the binary cannot start", static_cast<int32>(VM->GetDeviceAuthState()), static_cast<int32>(EUnrealMcpDeviceAuthState::Failed));
			TestTrue("actionable failure surfaced",
				VM->GetDeviceAuthError().Contains(TEXT("bootstrap-local")) || VM->GetDeviceAuthError().Contains(TEXT("reinstall")));
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

		It("Revoke notifies + pushes only when a token was actually dropped; a no-op Revoke does neither", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			int32 Notifications = 0;
			FDelegateHandle Handle = VM->OnConnectionSettingsChanged.AddLambda([&Notifications]() { Notifications++; });

			// No token stored → Revoke is a no-op: clears indicator state + sends idempotent auth-revoke, but must
			// NOT persist/push or fire a spurious panel rebuild (mirrors the no-op guards on the other setters).
			TestFalse("no token before no-op revoke", VM->HasCloudToken());
			VM->Revoke();
			TestEqual("no-op revoke did not notify", Notifications, 0);
			TestEqual("no-op revoke did not push", Rec->PushCount, 0);
			TestTrue("auth-revoke still sent (idempotent)", Rec->AuthSent.Contains(TEXT("auth-revoke")));

			// Now store a real bearer, then Revoke: this DOES drop a token → notify + push exactly once.
			VM->Authorize();
			TSharedPtr<FJsonObject> Authorized = MakeShared<FJsonObject>();
			Authorized->SetStringField(TEXT("state"), TEXT("authorized"));
			Authorized->SetStringField(TEXT("token"), TEXT("cloud-bearer-xyz"));
			VM->ApplyDeviceAuth(Authorized);
			TestTrue("token present before revoke", VM->HasCloudToken());
			const int32 NotificationsBeforeRevoke = Notifications;
			const int32 PushesBeforeRevoke = Rec->PushCount;

			VM->Revoke();
			TestFalse("token cleared after revoke", VM->HasCloudToken());
			TestEqual("token-dropping revoke notified once", Notifications, NotificationsBeforeRevoke + 1);
			TestEqual("token-dropping revoke pushed once", Rec->PushCount, PushesBeforeRevoke + 1);

			VM->OnConnectionSettingsChanged.Remove(Handle);
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

	Describe("Transport selector (§7 stdio/http — issue #59)", [this]()
	{
		It("Custom mode honours the stored transport and notifies on change", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			int32 Notifications = 0;
			FDelegateHandle Handle = VM->OnConnectionSettingsChanged.AddLambda([&Notifications]() { Notifications++; });

			// Enter Custom so the selector is live (mode change also notifies once).
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			const int32 NotificationsAfterMode = Notifications;
			TestTrue("transport selectable in Custom", VM->IsTransportSelectable());

			VM->SetTransportMethod(EUnrealMcpTransportMethod::Stdio);
			TestEqual("effective transport is stdio", static_cast<int32>(VM->GetEffectiveTransport()), static_cast<int32>(EUnrealMcpTransportMethod::Stdio));
			TestEqual("transport change notified once", Notifications, NotificationsAfterMode + 1);

			// A no-op transport set must NOT notify.
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Stdio);
			TestEqual("no-op transport change did not notify", Notifications, NotificationsAfterMode + 1);

			// Transport selection persists but does NOT push the connection config (it is presentation state).
			const int32 PushBefore = Rec->PushCount;
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Http);
			TestEqual("transport change did not push the connection config", Rec->PushCount, PushBefore);
			TestEqual("effective transport is http", static_cast<int32>(VM->GetEffectiveTransport()), static_cast<int32>(EUnrealMcpTransportMethod::Http));

			VM->OnConnectionSettingsChanged.Remove(Handle);
		});

		It("Cloud locks the transport to Http and forces auth Required (mirrors Unity)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// Start in Custom with stdio + no auth, then switch to Cloud.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Stdio);
			VM->SetAuthOption(EUnrealMcpAuthOption::None);

			VM->SetConnectionMode(EUnrealMcpConnectionMode::Cloud);
			TestFalse("transport not selectable in Cloud", VM->IsTransportSelectable());
			TestEqual("Cloud effective transport is Http", static_cast<int32>(VM->GetEffectiveTransport()), static_cast<int32>(EUnrealMcpTransportMethod::Http));
			TestEqual("Cloud forces auth Required", static_cast<int32>(VM->GetAuthOption()), static_cast<int32>(EUnrealMcpAuthOption::Required));

			// A stray SetTransportMethod in Cloud is ignored (selector is locked).
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Stdio);
			TestEqual("Cloud transport stays Http after stray set", static_cast<int32>(VM->GetEffectiveTransport()), static_cast<int32>(EUnrealMcpTransportMethod::Http));
		});
	});

	Describe("Local MCP-server gating + toggle (§7 in-UI Start — issue #95)", [this]()
	{
		It("is launchable ONLY in Custom mode + http transport", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// Custom + http -> launchable.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Http);
			TestTrue("Custom+http launchable", VM->IsLocalServerLaunchable());

			// Custom + stdio -> NOT launchable.
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Stdio);
			TestFalse("Custom+stdio not launchable", VM->IsLocalServerLaunchable());

			// Cloud (forces http) -> still NOT launchable (mode gates it out).
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Cloud);
			TestFalse("Cloud not launchable", VM->IsLocalServerLaunchable());
		});

		It("ToggleLocalServer starts then stops the server in Custom+http and reports the live state", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Http);

			TestFalse("starts stopped", VM->IsLocalServerRunning());
			TestEqual("button label Start when stopped", VM->GetLocalServerButtonText().ToString(), FString(TEXT("Start")));

			TestTrue("toggle acted (start)", VM->ToggleLocalServer());
			TestEqual("started once", Rec->ServerStartCount, 1);
			TestTrue("now running", VM->IsLocalServerRunning());
			TestEqual("button label Stop when running", VM->GetLocalServerButtonText().ToString(), FString(TEXT("Stop")));

			TestTrue("toggle acted (stop)", VM->ToggleLocalServer());
			TestEqual("stopped once", Rec->ServerStopCount, 1);
			TestFalse("now stopped", VM->IsLocalServerRunning());
		});

		It("ToggleLocalServer is a NO-OP (never touches the sinks) outside Custom+http", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);

			// Custom + stdio: gated out.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Stdio);
			TestFalse("toggle is a no-op in Custom+stdio", VM->ToggleLocalServer());
			TestEqual("no start in Custom+stdio", Rec->ServerStartCount, 0);

			// Cloud: gated out.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Cloud);
			TestFalse("toggle is a no-op in Cloud", VM->ToggleLocalServer());
			TestEqual("no start in Cloud", Rec->ServerStartCount, 0);
			TestFalse("server never started", Rec->bServerRunning);
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

	Describe("Connection-status lifecycle (issue #116)", [this]()
	{
		// Drive the view-model into a live, green "Connected" state the way the runtime does: arm via Connect()
		// then apply a Connected `status` from the sidecar. Returns a VM presenting Connected + armed.
		auto MakeConnectedVM = [](TSharedRef<FRecording> Rec) -> TSharedRef<FUnrealMcpEditorViewModel>
		{
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			VM->Connect();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Status);
			return VM;
		};

		It("Bug 1: switching connection mode while connected drops the green Connected state immediately", [this, MakeConnectedVM]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			// Default config mode is Cloud — connect green in Cloud, then switch to Custom (a retarget).
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeConnectedVM(Rec);
			TestEqual("green before the switch", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));

			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			// The dot must leave green the INSTANT the target changes — not wait for the sidecar's re-dial status.
			TestNotEqual("no longer Connected after the mode switch",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));
			TestEqual("optimistically Connecting to the new target",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connecting));
			// Still armed: the user retargeted, they did not Disconnect — a re-dial to the new target is expected.
			TestTrue("still armed after the retarget", VM->IsReconnectArmed());
			// And it pushed the new config so the sidecar re-dials the new target.
			TestTrue("pushed the retargeted config", Rec->PushCount >= 1);
		});

		It("Bug 1: the symmetric Custom->Cloud switch also drops the green state", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			// Enter Custom, connect green there, then switch back to Cloud.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->Connect();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Status);
			TestEqual("green in Custom", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));

			VM->SetConnectionMode(EUnrealMcpConnectionMode::Cloud);
			TestEqual("Connecting after Custom->Cloud",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connecting));
		});

		It("Bug 1: editing the Server URL to a new valid host while connected drops the green state", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetCustomHost(TEXT("http://localhost:5244"));
			VM->Connect();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Status);
			TestEqual("green before the host edit", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));

			// Edit to a DIFFERENT valid host: retargets the dial → drop off green.
			VM->SetCustomHost(TEXT("http://localhost:9999"));
			TestEqual("Connecting after the host retarget",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connecting));
		});

		It("Bug 1: re-typing the SAME host (no retarget) does NOT churn the Connected state", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetCustomHost(TEXT("http://localhost:5244"));
			VM->Connect();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Status);

			// Same host value — the dial target is unchanged, so the live Connected state must be preserved.
			VM->SetCustomHost(TEXT("http://localhost:5244"));
			TestEqual("still Connected after a no-retarget host set",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));
		});

		It("Bug 1: an invalid host edit while connected does NOT drop green (no retarget happens)", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetCustomHost(TEXT("http://localhost:5244"));
			VM->Connect();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Status);

			// A malformed URL never becomes the dial target (no push) — so the live link to the OLD valid host stands.
			VM->SetCustomHost(TEXT("not-a-url"));
			TestEqual("still Connected after a malformed host edit",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));
		});

		It("Bug 1: a mode switch while DISCONNECTED does not fabricate a Connecting state", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			// Never connected (unarmed, Disconnected). A retarget must not claim a connect is in flight.
			TestEqual("starts Disconnected", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Disconnected));
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			TestEqual("stays Disconnected (no green to drop, no re-dial armed)",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Disconnected));
		});

		It("Bug 2: stopping the local server while connected drives the state off Connected", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			// Custom + http = launchable; start the local server, then connect green to it.
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Http);
			VM->ToggleLocalServer(); // start
			TestTrue("server running", VM->IsLocalServerRunning());
			VM->Connect();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Status);
			TestEqual("green before stopping the server", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));

			// Stop the server the editor is connected to: the dot must leave green WITHOUT a manual Disconnect.
			VM->ToggleLocalServer(); // stop
			TestEqual("server stopped once", Rec->ServerStopCount, 1);
			TestFalse("server no longer running", VM->IsLocalServerRunning());
			TestNotEqual("no longer Connected after stopping the server",
				static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Connected));
			// Stays armed (the user stopped the SERVER, not the connection) — a re-Start should resume; while armed a
			// torn-down link reads as Degraded (background retry), so the action button leaves "Disconnect".
			TestEqual("Degraded after the server stop", static_cast<int32>(VM->GetConnectionState()), static_cast<int32>(EUnrealMcpConnectionState::Degraded));
			TestTrue("still armed", VM->IsReconnectArmed());
		});

		It("Bug 2: the action button label updates off 'Disconnect' once the server-stop drops the link", [this]()
		{
			TSharedRef<FRecording> Rec = MakeShared<FRecording>();
			TSharedRef<FUnrealMcpEditorViewModel> VM = MakeViewModel(Rec);
			VM->SetConnectionMode(EUnrealMcpConnectionMode::Custom);
			VM->SetTransportMethod(EUnrealMcpTransportMethod::Http);
			VM->ToggleLocalServer();
			VM->Connect();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("connectionState"), TEXT("Connected"));
			Status->SetBoolField(TEXT("keepConnected"), true);
			VM->ApplyStatus(Status);
			TestEqual("button is Disconnect while Connected",
				FUnrealMcpEditorViewModel::GetButtonText(VM->GetConnectionState()).ToString(), FString(TEXT("Disconnect")));

			VM->ToggleLocalServer(); // stop
			// Degraded → the tri-state maps to "Stop" (the user can halt the background retry) — no longer "Disconnect".
			TestEqual("button updated off Disconnect after the server stop",
				FUnrealMcpEditorViewModel::GetButtonText(VM->GetConnectionState()).ToString(), FString(TEXT("Stop")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
