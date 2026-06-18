// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/UnrealMcpEditorViewModel.h"
#include "Sidecar/UnrealMcpSidecarManager.h"
#include "UnrealMcpLog.h"

#include "Dom/JsonObject.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

FUnrealMcpEditorViewModel::FUnrealMcpEditorViewModel() = default;

void FUnrealMcpEditorViewModel::InitializeConfig(const FUnrealMcpConfig& InConfig)
{
	Config = InConfig;
	// Reflect the persisted intent: keepConnected drives whether we present as armed-but-disconnected.
	ConnectionState = Config.bKeepConnected ? EUnrealMcpConnectionState::Connecting : EUnrealMcpConnectionState::Disconnected;
	DeviceAuthState = HasCloudToken() ? EUnrealMcpDeviceAuthState::Authorized : EUnrealMcpDeviceAuthState::Idle;
}

void FUnrealMcpEditorViewModel::PersistAndPush()
{
	if (OnPersistConfig)
		OnPersistConfig(Config);
	if (OnPushConfig)
		OnPushConfig(Config);
}

void FUnrealMcpEditorViewModel::SetConnectionMode(EUnrealMcpConnectionMode InMode)
{
	if (Config.ConnectionMode == InMode)
		return;
	Config.ConnectionMode = InMode;
	// Cloud is HTTP-only AND always authenticated (the cloud server enforces both): switching INTO Cloud forces
	// transport→Http and auth→Required so the locked-in connection facts are consistent the moment the mode flips
	// (mirrors Unity's SetupConnectionModeToggle: `TransportMethod = streamableHttp; AuthOption = required`). The
	// stored Custom transport string is left as-is — ResolveEffectiveTransport already returns Http while in Cloud,
	// and forcing it here keeps the persisted value coherent so a later switch back to Custom restores intent.
	if (InMode == EUnrealMcpConnectionMode::Cloud)
	{
		Config.SetTransportMethod(EUnrealMcpTransportMethod::Http);
		Config.AuthOption = EUnrealMcpAuthOption::Required;
	}
	// Bug #116: a mode switch RETARGETS the connection (Cloud↔Custom dial a different server). If we were showing
	// Connected/Degraded against the now-abandoned target, the dot must drop off green THE MOMENT the mode flips —
	// not stay green until the sidecar's re-dial status crosses the wire. PersistAndPush below triggers the bridge's
	// host-changed re-dial (DecideConfigTransition→Reconnect, which emits Connecting→Connected/etc.), but the UI must
	// not lag behind it: optimistically reflect the in-flight retarget so no window of stale-green exists.
	ReflectTargetChangeOptimistically();
	PersistAndPush();
	// The resolved connection facts (auth-required, token source, http url, transport) all hinge on the mode, so the
	// agent configurators must re-resolve their cached config + rebuild their panel (Unity's InvalidateAndReloadAgentUI).
	OnConnectionSettingsChanged.Broadcast();
}

void FUnrealMcpEditorViewModel::ReflectTargetChangeOptimistically()
{
	// Bug #116 (mode/target switch): when the dial target changes WHILE we are presenting a live/armed link
	// (Connected or Degraded), the link we are showing is to the OLD target and is about to be torn down + re-dialed
	// by the sidecar. Optimistically demote to Connecting so the green dot drops immediately; the sidecar's honest
	// re-dial `status` (Connecting→Connected on success, or Disconnected/Degraded on failure) then converges the UI.
	// Only meaningful while armed: an unarmed (already Disconnected) view-model has no green to drop, and forcing
	// Connecting there would falsely claim a connect is in flight when keepConnected=false means none will run.
	if (Config.bKeepConnected
		&& (ConnectionState == EUnrealMcpConnectionState::Connected || ConnectionState == EUnrealMcpConnectionState::Degraded))
	{
		ConnectionState = EUnrealMcpConnectionState::Connecting;
	}
}

void FUnrealMcpEditorViewModel::SetTransportMethod(EUnrealMcpTransportMethod InMethod)
{
	// Cloud locks the transport to Http (the cloud is HTTP-only) — ignore a stray set so the selector cannot
	// desync the effective transport. Only Custom mode honours the user's stdio/http choice.
	if (Config.ConnectionMode == EUnrealMcpConnectionMode::Cloud)
		return;
	if (Config.GetTransportMethod() == InMethod)
		return; // no-op — no persist / no panel churn (mirrors the other setters' no-op guards).
	Config.SetTransportMethod(InMethod);
	// Transport selection is UI/snippet presentation state (which transport's snippet the agent panel shows + what
	// Configure writes); it does NOT change the live SignalR connection, so — like SetSelectedAgentId — persist only,
	// never OnPushConfig. But it DOES change the agent snippet/status, so fire OnConnectionSettingsChanged to rebuild.
	if (OnPersistConfig)
		OnPersistConfig(Config);
	OnConnectionSettingsChanged.Broadcast();
}

void FUnrealMcpEditorViewModel::SetCustomHost(const FString& InHost)
{
	// Trim before storing: validation already trims internally, so "  http://host  " would validate yet get
	// persisted + pushed padded — the sidecar would then dial the untrimmed target. Store the trimmed form so
	// the field, the §8 store, and the dial target stay consistent. Only push a host that validates — pushing a
	// malformed URL to the sidecar would just make it dial nowhere (§7 validated field).
	const FString Trimmed = InHost.TrimStartAndEnd();
	const bool bChanged = Config.CustomHost != Trimmed;
	Config.CustomHost = Trimmed;
	FString Error;
	if (bChanged && ValidateServerUrl(Trimmed, Error))
	{
		// Bug #116: editing the Server URL to a NEW valid target retargets the dial — the sidecar re-dials the new
		// host (DecideConfigTransition→Reconnect) and the green dot for the OLD host must drop immediately. Mirror
		// the SetConnectionMode path: optimistically demote off green before the push so no stale-green window exists.
		// Only on an actual change to a valid host — a malformed edit never changes the dial target (no push, no retarget).
		ReflectTargetChangeOptimistically();
		PersistAndPush();
	}
	else if (ValidateServerUrl(Trimmed, Error))
	{
		// Re-typing the same valid host (bChanged == false): still push for idempotent parity with the prior
		// behavior, but there is no retarget — the dial target is unchanged, so the connection state must not churn.
		PersistAndPush();
	}
	// Refresh the configurators on any host change (even before it validates) so the previewed HTTP url tracks
	// what the user typed; the panel reads the host through the connection-info provider, not the dialed target.
	if (bChanged)
		OnConnectionSettingsChanged.Broadcast();
}

FString FUnrealMcpEditorViewModel::GetLogLevel() const
{
	// Fall back to the §8 default ("Info") when the persisted value is empty so the dropdown always has a valid
	// selection to render (mirrors the header field's old empty→"Info" coalesce).
	return Config.LogLevel.IsEmpty() ? FString(FUnrealMcpConfig::DefaultLogLevel) : Config.LogLevel;
}

void FUnrealMcpEditorViewModel::SetLogLevel(const FString& InLevel)
{
	// Empty resets to the §8 default so the field never persists a blank verbosity. The level is part of the §1.3
	// `config` message (the sidecar adopts it), so persist AND push — unlike the pure-UI presentation setters.
	const FString NewLevel = InLevel.IsEmpty() ? FString(FUnrealMcpConfig::DefaultLogLevel) : InLevel;
	if (Config.LogLevel == NewLevel)
		return;
	Config.LogLevel = NewLevel;
	PersistAndPush();
}

void FUnrealMcpEditorViewModel::SetAuthOption(EUnrealMcpAuthOption InOption)
{
	if (Config.AuthOption == InOption)
		return;
	Config.AuthOption = InOption;
	PersistAndPush();
	// Auth-required toggles whether the snippet carries a bearer header/arg — the configurators must re-resolve.
	OnConnectionSettingsChanged.Broadcast();
}

void FUnrealMcpEditorViewModel::SetCustomToken(const FString& InToken)
{
	if (Config.CustomToken == InToken)
		return;
	Config.CustomToken = InToken;
	PersistAndPush();
	// The token is injected into the STDIO args / HTTP Authorization header — refresh the configurators.
	OnConnectionSettingsChanged.Broadcast();
}

void FUnrealMcpEditorViewModel::GenerateCustomToken()
{
	Config.CustomToken = FUnrealMcpSidecarManager::GenerateToken();
	// Generating a token only makes sense when auth is actually required — flip it so the new token is used.
	Config.AuthOption = EUnrealMcpAuthOption::Required;
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] generated a new Custom-mode token (%s)."),
		*FUnrealMcpConfig::MaskSecret(Config.CustomToken));
	PersistAndPush();
	// New token + auth flipped to required — the configurators' snippet/Configure output must reflect both.
	OnConnectionSettingsChanged.Broadcast();
}

void FUnrealMcpEditorViewModel::Connect()
{
	Config.bKeepConnected = true;
	ConnectionState = EUnrealMcpConnectionState::Connecting;
	PersistAndPush();
}

void FUnrealMcpEditorViewModel::Disconnect()
{
	// The Godot M9b lesson: Disconnect must GENUINELY halt the reconnect loop. keepConnected=false pushed
	// over `config` tells the sidecar to tear down the SignalR client fully — not merely drop one link.
	Config.bKeepConnected = false;
	ConnectionState = EUnrealMcpConnectionState::Disconnected;
	PersistAndPush();
}

const TCHAR* FUnrealMcpEditorViewModel::BridgeUnavailableMessage()
{
	// Actionable, NOT the cryptic "No sidecar connected." — tell the user how to get a working bridge (issue #99).
	return TEXT("MCP bridge not installed for this platform — reinstall the plugin, or run `unreal-mcp-cli bootstrap-local`.");
}

void FUnrealMcpEditorViewModel::Authorize(double NowSeconds)
{
	DeviceVerificationUrl.Reset();
	DeviceUserCode.Reset();
	DeviceAuthError.Reset();
	bAuthStartQueued = false;

	// Fast path: a connected + handshaken sidecar takes the auth-start frame immediately → straight to Pending,
	// then the sidecar drives the device-code feed (verificationUrl / userCode / authorized) over `device-auth`.
	if (OnSendAuth && OnSendAuth(TEXT("auth-start")))
	{
		DeviceAuthState = EUnrealMcpDeviceAuthState::Pending;
		return;
	}

	// Robust path (issue #99): the sidecar is not connected/handshaken yet. Do NOT dead-end to Failed — request
	// it (re)start, QUEUE the auth-start, and enter the transient Connecting state. NotifySidecarHandshakeComplete()
	// flushes the queued frame the moment the sidecar handshakes; TickAuthTimeout() bounds the wait so the UI never
	// wedges. Only an unresolvable bridge binary fails fast with an actionable message.
	const bool bCanStart = OnEnsureSidecarStarted ? OnEnsureSidecarStarted() : false;
	if (!bCanStart)
	{
		// The bridge binary genuinely cannot be resolved/spawned (packaged-without-<rid> / unset dev override).
		// Awaiting a handshake that will never arrive would be a silent wedge — fail fast, actionably.
		DeviceAuthState = EUnrealMcpDeviceAuthState::Failed;
		DeviceAuthError = BridgeUnavailableMessage();
		return;
	}

	bAuthStartQueued = true;
	ConnectingStartedSeconds = NowSeconds;
	DeviceAuthState = EUnrealMcpDeviceAuthState::Connecting;
}

void FUnrealMcpEditorViewModel::NotifySidecarHandshakeComplete()
{
	// Only meaningful while we are waiting (Connecting) with a queued auth-start. A handshake at any other time
	// (a normal reconnect, a manual Restart bridge with no auth in flight) must not spuriously start an auth flow.
	if (DeviceAuthState != EUnrealMcpDeviceAuthState::Connecting || !bAuthStartQueued)
		return;

	// The sidecar is now connected/handshaken — flush the queued auth-start. On success advance to Pending (the
	// sidecar's device-auth feed drives the rest); a still-failing send is a transient race, so stay Connecting
	// and let either a later handshake re-fire this or the timeout fail it actionably.
	if (OnSendAuth && OnSendAuth(TEXT("auth-start")))
	{
		bAuthStartQueued = false;
		DeviceVerificationUrl.Reset();
		DeviceUserCode.Reset();
		DeviceAuthError.Reset();
		DeviceAuthState = EUnrealMcpDeviceAuthState::Pending;
	}
}

void FUnrealMcpEditorViewModel::TickAuthTimeout(double NowSeconds)
{
	// Bounded Connecting→Failed: only fires while awaiting the sidecar handshake. Once the wait exceeds the bound,
	// the bridge plainly is not coming up — fail actionably and clear the queued frame so a late handshake cannot
	// resurrect a stale auth-start. No-op in every other state (Pending/Authorized/Failed/Idle are driven elsewhere).
	if (DeviceAuthState != EUnrealMcpDeviceAuthState::Connecting)
		return;
	if (NowSeconds - ConnectingStartedSeconds < SidecarConnectTimeoutSeconds)
		return;

	bAuthStartQueued = false;
	DeviceAuthState = EUnrealMcpDeviceAuthState::Failed;
	DeviceAuthError = BridgeUnavailableMessage();
}

void FUnrealMcpEditorViewModel::CancelAuth()
{
	// Cancelling clears any queued/awaiting auth-start (issue #99) so a late handshake/timeout cannot revive it.
	bAuthStartQueued = false;
	// Cancelling an in-progress RE-authorize must not hide an already-stored cloud token: InitializeConfig maps
	// token-present → Authorized, so dropping to Idle here would lose the "Authorized"/Revoke affordance until
	// restart. Fall back to Authorized when a bearer is still stored; only a token-less cancel returns to Idle.
	DeviceAuthState = HasCloudToken() ? EUnrealMcpDeviceAuthState::Authorized : EUnrealMcpDeviceAuthState::Idle;
	DeviceVerificationUrl.Reset();
	DeviceUserCode.Reset();
	DeviceAuthError.Reset();
	if (OnSendAuth)
		OnSendAuth(TEXT("auth-cancel"));
}

void FUnrealMcpEditorViewModel::Revoke()
{
	// A no-op Revoke (no bearer stored) must not churn the store / re-push / rebuild the panel — mirror the
	// no-op guards on SetConnectionMode/SetCustomHost/SetAuthOption/SetCustomToken. Always clear the device-auth
	// indicator state and tell the sidecar (auth-revoke is idempotent), but only persist/push/notify when a token
	// was actually dropped.
	const bool bHadToken = !Config.CloudToken.IsEmpty();
	Config.CloudToken.Reset();
	DeviceAuthState = EUnrealMcpDeviceAuthState::Idle;
	DeviceVerificationUrl.Reset();
	DeviceUserCode.Reset();
	DeviceAuthError.Reset();
	if (OnSendAuth)
		OnSendAuth(TEXT("auth-revoke"));
	if (bHadToken)
	{
		// CloudToken changed — persist (Save restores env/.env overrides) and push the now-anonymous config.
		PersistAndPush();
		// The Cloud-mode bearer the configurators inject is now gone — refresh so the snippet drops it.
		OnConnectionSettingsChanged.Broadcast();
	}
}

void FUnrealMcpEditorViewModel::ApplyStatus(const TSharedPtr<FJsonObject>& Status)
{
	if (!Status.IsValid())
		return;

	FString StateRaw;
	Status->TryGetStringField(TEXT("connectionState"), StateRaw);

	bool bKeep = Config.bKeepConnected;
	Status->TryGetBoolField(TEXT("keepConnected"), bKeep);

	ConnectionState = ParseConnectionState(StateRaw, bKeep);

	// A `status` reflecting an authorized cloud session promotes the device-auth indicator.
	FString CloudAuthState;
	if (Status->TryGetStringField(TEXT("cloudAuthState"), CloudAuthState))
	{
		if (CloudAuthState.Equals(TEXT("Authorized"), ESearchCase::IgnoreCase))
			DeviceAuthState = EUnrealMcpDeviceAuthState::Authorized;
	}

	AiAgents.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Agents = nullptr;
	if (Status->TryGetArrayField(TEXT("aiAgents"), Agents) && Agents)
	{
		for (const TSharedPtr<FJsonValue>& Agent : *Agents)
		{
			FString Label;
			if (Agent.IsValid() && Agent->TryGetString(Label) && !Label.IsEmpty())
			{
				AiAgents.Add(Label);
			}
			else if (Agent.IsValid())
			{
				const TSharedPtr<FJsonObject>* AgentObj = nullptr;
				if (Agent->TryGetObject(AgentObj) && AgentObj && (*AgentObj)->TryGetStringField(TEXT("label"), Label))
					AiAgents.Add(Label);
			}
		}
	}
}

void FUnrealMcpEditorViewModel::ApplyDeviceAuth(const TSharedPtr<FJsonObject>& DeviceAuth)
{
	if (!DeviceAuth.IsValid())
		return;

	FString Url;
	const bool bHadUrl = !DeviceVerificationUrl.IsEmpty();
	if (DeviceAuth->TryGetStringField(TEXT("verificationUrl"), Url) && !Url.IsEmpty())
		DeviceVerificationUrl = Url;

	FString UserCode;
	if (DeviceAuth->TryGetStringField(TEXT("userCode"), UserCode))
		DeviceUserCode = UserCode;

	FString StateRaw;
	if (DeviceAuth->TryGetStringField(TEXT("state"), StateRaw))
	{
		if (StateRaw.Equals(TEXT("authorized"), ESearchCase::IgnoreCase) || StateRaw.Equals(TEXT("success"), ESearchCase::IgnoreCase))
		{
			// Cancel-race complement (§7 item 4): the sidecar emits the authorized device-auth (token included)
			// BEFORE its own auth-cancel/auth-revoke guard runs, and a pushed config would re-apply that bearer
			// sidecar-side. If the user already cancelled/revoked this flow the indicator is back to Idle (a
			// token-less cancel, or a revoke that cleared the bearer) — ignore the late authorized so we do not
			// resurrect a token the user just dropped. (A cancel that KEPT a stored token leaves state Authorized,
			// not Idle, so a legitimate re-confirm still applies.)
			if (DeviceAuthState == EUnrealMcpDeviceAuthState::Idle)
				return;
			DeviceAuthState = EUnrealMcpDeviceAuthState::Authorized;
			DeviceAuthError.Reset();
			// The sidecar stored the token; mirror its presence so the UI flips to the Revoke affordance, and
			// PERSIST + PUSH it (like Revoke does): without this the bearer is lost on editor restart (never
			// written to the §8 config store) and the bridge's effective config never learns it, so a "Restart
			// bridge" re-pushes a token-less config and silently drops the authorized session. Guard on change
			// so a repeated `authorized` poll update does not churn the store / re-push every tick.
			FString Token;
			if (DeviceAuth->TryGetStringField(TEXT("token"), Token) && !Token.IsEmpty() && Config.CloudToken != Token)
			{
				Config.CloudToken = Token;
				PersistAndPush();
				// A fresh Cloud bearer landed — the configurators inject it into the Cloud-mode snippet; refresh.
				OnConnectionSettingsChanged.Broadcast();
			}
		}
		else if (StateRaw.Equals(TEXT("failed"), ESearchCase::IgnoreCase) || StateRaw.Equals(TEXT("denied"), ESearchCase::IgnoreCase) || StateRaw.Equals(TEXT("expired"), ESearchCase::IgnoreCase))
		{
			DeviceAuthState = EUnrealMcpDeviceAuthState::Failed;
			// Surface the sidecar's human-readable reason (DeviceAuthMessage.Message — "Authorization was
			// denied." / "The authorization request expired." / "Could not start authorization.") so the
			// window shows feedback instead of silently collapsing the pending instructions.
			FString Message;
			if (DeviceAuth->TryGetStringField(TEXT("message"), Message) && !Message.IsEmpty())
				DeviceAuthError = Message;
			else if (DeviceAuthError.IsEmpty())
				DeviceAuthError = TEXT("Authorization failed.");
		}
		else if (StateRaw.Equals(TEXT("pending"), ESearchCase::IgnoreCase))
		{
			DeviceAuthState = EUnrealMcpDeviceAuthState::Pending;
			DeviceAuthError.Reset();
		}
	}

	// Open the verification page the first time we learn the URL (one-shot; never re-open on poll updates).
	if (!bHadUrl && !DeviceVerificationUrl.IsEmpty() && OnOpenBrowser)
		OnOpenBrowser(DeviceVerificationUrl);
}

// --- §7 per-tool enable-map ------------------------------------------------------------------------

bool FUnrealMcpEditorViewModel::IsToolDisabled(const FString& ToolName) const
{
	return Config.DisabledTools.Contains(ToolName);
}

void FUnrealMcpEditorViewModel::SetToolEnabled(const FString& ToolName, bool bEnabled)
{
	const bool bCurrentlyDisabled = Config.DisabledTools.Contains(ToolName);
	const bool bWantDisabled = !bEnabled;
	if (bCurrentlyDisabled == bWantDisabled)
		return; // already in the requested state — no persist / no manifest churn.

	if (bWantDisabled)
		Config.DisabledTools.AddUnique(ToolName);
	else
		Config.DisabledTools.Remove(ToolName);

	// Persist the blocklist to the §8 store FIRST (so the choice survives a restart even if the manifest push
	// races a teardown), then ask the runtime to re-apply enablement to the registry + re-push the manifest.
	// Note: this deliberately does NOT call OnPushConfig — tool enablement is a manifest (§2.2) concern, not a
	// §1.3 `config` (connection) concern; pushing the connection config here would be a spurious sidecar re-dial.
	if (OnPersistConfig)
		OnPersistConfig(Config);
	if (OnToolEnablementChanged)
		OnToolEnablementChanged(Config.DisabledTools);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] tool '%s' %s via the MCP Tools window."),
		*ToolName, bEnabled ? TEXT("enabled") : TEXT("disabled"));
}

void FUnrealMcpEditorViewModel::SetSelectedAgentId(const FString& InAgentId)
{
	if (Config.SelectedAgentId == InAgentId)
		return; // already selected — no persist churn.
	Config.SelectedAgentId = InAgentId;

	// Persist-only: the dropdown selection is pure presentation state. Deliberately NO OnPushConfig — the
	// selection does not change the live connection (mirrors SetToolEnabled's "manifest-only, not config" note).
	if (OnPersistConfig)
		OnPersistConfig(Config);
}

bool FUnrealMcpEditorViewModel::IsAutoGenerateSkills(const FString& AgentId) const
{
	return Config.SkillAutoGenerateAgents.Contains(AgentId);
}

void FUnrealMcpEditorViewModel::SetAutoGenerateSkills(const FString& AgentId, bool bEnabled)
{
	if (AgentId.IsEmpty())
		return;
	const bool bCurrently = Config.SkillAutoGenerateAgents.Contains(AgentId);
	if (bCurrently == bEnabled)
		return; // no-op — no persist churn.

	if (bEnabled)
		Config.SkillAutoGenerateAgents.AddUnique(AgentId);
	else
		Config.SkillAutoGenerateAgents.Remove(AgentId);

	// Persist-only: skills generation is pure presentation state (mirrors SetSelectedAgentId). Deliberately NO
	// OnPushConfig — enabling skills does not change the live connection. The panel regenerates the files after this.
	if (OnPersistConfig)
		OnPersistConfig(Config);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] agent '%s' auto-generate-skills %s."),
		*AgentId, bEnabled ? TEXT("enabled") : TEXT("disabled"));
}

void FUnrealMcpEditorViewModel::SetSkillsPath(const FString& InPath)
{
	const FString Normalized = InPath.IsEmpty() ? FString(TEXT(".claude/skills")) : InPath;
	if (Config.SkillsPath == Normalized)
		return; // no-op.
	Config.SkillsPath = Normalized;

	// Persist-only: the Custom skills folder is pure presentation state. Never pushes the §1.3 `config`.
	if (OnPersistConfig)
		OnPersistConfig(Config);
}

// --- Pure helpers ----------------------------------------------------------------------------------

bool FUnrealMcpEditorViewModel::ValidateServerUrl(const FString& Url, FString& OutError)
{
	const FString Trimmed = Url.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OutError = TEXT("Server URL is empty.");
		return false;
	}

	FString Scheme;
	FString Remainder;
	if (!Trimmed.Split(TEXT("://"), &Scheme, &Remainder))
	{
		OutError = TEXT("Server URL must start with http:// or https://.");
		return false;
	}

	if (!Scheme.Equals(TEXT("http"), ESearchCase::IgnoreCase) && !Scheme.Equals(TEXT("https"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Server URL scheme must be http or https.");
		return false;
	}

	// Host is everything up to the first '/', '?' or '#'. It must be non-empty.
	FString Host = Remainder;
	int32 SlashIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Remainder.Len(); ++Index)
	{
		const TCHAR Ch = Remainder[Index];
		if (Ch == TEXT('/') || Ch == TEXT('?') || Ch == TEXT('#'))
		{
			SlashIndex = Index;
			break;
		}
	}
	if (SlashIndex != INDEX_NONE)
		Host = Remainder.Left(SlashIndex);

	if (Host.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("Server URL has no host.");
		return false;
	}

	OutError.Reset();
	return true;
}

FString FUnrealMcpEditorViewModel::MaskTokenForDisplay(const FString& Token, bool bReveal)
{
	if (Token.IsEmpty())
		return FString();
	if (bReveal)
		return Token;
	// Fixed-width mask — the real length is never leaked into the UI (§8).
	return FString::ChrN(8, TEXT('\x2022')); // U+2022 BULLET
}

bool FUnrealMcpEditorViewModel::IsLocalServerLaunchable() const
{
	// ONLY Custom mode + http transport targets a locally-hosted server (mirrors
	// FUnrealMcpServerManager::IsLaunchAllowed). Use the connection-AWARE effective transport so Cloud (which
	// forces Http) still gates OUT on its non-Custom mode rather than slipping through on the locked Http value.
	return Config.ConnectionMode == EUnrealMcpConnectionMode::Custom
		&& Config.ResolveEffectiveTransport() == EUnrealMcpTransportMethod::Http;
}

bool FUnrealMcpEditorViewModel::IsLocalServerRunning() const
{
	return IsLocalServerRunningSink ? IsLocalServerRunningSink() : false;
}

bool FUnrealMcpEditorViewModel::ToggleLocalServer()
{
	// Gating guard: a click in Cloud / Custom+stdio must NEVER spawn a server. This is the single choke point
	// the UI Start button AND the dev-control /control/click "start" both route through.
	if (!IsLocalServerLaunchable())
	{
		UE_LOG(LogUnrealMcp, Verbose,
			TEXT("[Unreal-MCP] local-server toggle ignored — not launchable (requires Custom mode + http transport)."));
		return false;
	}

	if (IsLocalServerRunning())
	{
		if (OnStopLocalServer)
			OnStopLocalServer();
		// Bug #116 (stop local server while connected): the server the editor's transport is connected to has just
		// gone away — the SignalR link is now dead even though no explicit Disconnect was clicked. The dot must not
		// stay green. Optimistically demote off Connected/Degraded so the UI reflects the drop immediately; the
		// bridge's transport-drop status (issue #116, ConnectionState→non-Connected) then converges it. We stay armed
		// (keepConnected unchanged): the user STOPPED the server, not the connection — re-Starting it should resume
		// without re-arming. While armed, a torn-down link reads as Degraded (background-retry), matching ParseConnectionState.
		if (Config.bKeepConnected
			&& (ConnectionState == EUnrealMcpConnectionState::Connected
				|| ConnectionState == EUnrealMcpConnectionState::Connecting
				|| ConnectionState == EUnrealMcpConnectionState::Degraded))
		{
			ConnectionState = EUnrealMcpConnectionState::Degraded;
		}
		return true;
	}
	if (OnStartLocalServer)
		return OnStartLocalServer();
	return false;
}

FText FUnrealMcpEditorViewModel::GetLocalServerButtonText() const
{
	return IsLocalServerRunning() ? LOCTEXT("ServerBtnStop", "Stop") : LOCTEXT("ServerBtnStart", "Start");
}

FText FUnrealMcpEditorViewModel::GetButtonText(EUnrealMcpConnectionState State)
{
	switch (State)
	{
		case EUnrealMcpConnectionState::Connected:  return LOCTEXT("BtnDisconnect", "Disconnect");
		case EUnrealMcpConnectionState::Connecting: return LOCTEXT("BtnStop", "Stop");
		case EUnrealMcpConnectionState::Degraded:   return LOCTEXT("BtnStop", "Stop");
		case EUnrealMcpConnectionState::Disconnected:
		default:                                    return LOCTEXT("BtnConnect", "Connect");
	}
}

FText FUnrealMcpEditorViewModel::GetStatusLabel(EUnrealMcpConnectionState State)
{
	switch (State)
	{
		case EUnrealMcpConnectionState::Connected:  return LOCTEXT("StatusConnected", "Unreal: Connected");
		case EUnrealMcpConnectionState::Connecting: return LOCTEXT("StatusConnecting", "Unreal: Connecting…");
		case EUnrealMcpConnectionState::Degraded:   return LOCTEXT("StatusDegraded", "Unreal: Reconnecting…");
		case EUnrealMcpConnectionState::Disconnected:
		default:                                    return LOCTEXT("StatusDisconnected", "Unreal: Disconnected");
	}
}

FLinearColor FUnrealMcpEditorViewModel::GetStatusColor(EUnrealMcpConnectionState State)
{
	switch (State)
	{
		case EUnrealMcpConnectionState::Connected:  return FLinearColor(0.16f, 0.74f, 0.30f); // green
		case EUnrealMcpConnectionState::Connecting: return FLinearColor(0.95f, 0.69f, 0.13f); // amber
		case EUnrealMcpConnectionState::Degraded:   return FLinearColor(0.95f, 0.49f, 0.13f); // orange
		case EUnrealMcpConnectionState::Disconnected:
		default:                                    return FLinearColor(0.55f, 0.16f, 0.16f); // red
	}
}

EUnrealMcpConnectionState FUnrealMcpEditorViewModel::ParseConnectionState(const FString& Raw, bool bKeepConnected)
{
	if (Raw.Equals(TEXT("Connected"), ESearchCase::IgnoreCase))
		return EUnrealMcpConnectionState::Connected;
	if (Raw.Equals(TEXT("Connecting"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("Reconnecting"), ESearchCase::IgnoreCase))
		return EUnrealMcpConnectionState::Connecting;
	if (Raw.Equals(TEXT("Degraded"), ESearchCase::IgnoreCase))
		return EUnrealMcpConnectionState::Degraded;
	if (Raw.Equals(TEXT("Disconnected"), ESearchCase::IgnoreCase))
	{
		// A reported Disconnected while still armed (keepConnected) is a background retry = Degraded, not a
		// user-initiated stop. Only an unarmed Disconnected is a true Disconnected.
		return bKeepConnected ? EUnrealMcpConnectionState::Degraded : EUnrealMcpConnectionState::Disconnected;
	}
	// Unknown/empty: fall back to the armed flag.
	return bKeepConnected ? EUnrealMcpConnectionState::Connecting : EUnrealMcpConnectionState::Disconnected;
}

#undef LOCTEXT_NAMESPACE
