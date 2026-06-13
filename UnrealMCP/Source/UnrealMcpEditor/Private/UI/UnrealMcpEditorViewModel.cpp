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
	PersistAndPush();
	// The resolved connection facts (auth-required, token source, http url) all hinge on the mode, so the agent
	// configurators must re-resolve their cached config + rebuild their panel (Unity's InvalidateAndReloadAgentUI).
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
	if (ValidateServerUrl(Trimmed, Error))
	{
		PersistAndPush();
	}
	// Refresh the configurators on any host change (even before it validates) so the previewed HTTP url tracks
	// what the user typed; the panel reads the host through the connection-info provider, not the dialed target.
	if (bChanged)
		OnConnectionSettingsChanged.Broadcast();
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

void FUnrealMcpEditorViewModel::Authorize()
{
	DeviceVerificationUrl.Reset();
	DeviceUserCode.Reset();
	DeviceAuthError.Reset();
	// Only enter Pending if the auth-start frame actually reached the sidecar. SendAuthMessage returns false
	// when no sidecar is connected/handshaken; entering Pending regardless would wedge the UI showing
	// "Complete authorization in your browser…" with an empty user code until the user manually cancels.
	const bool bSent = OnSendAuth ? OnSendAuth(TEXT("auth-start")) : false;
	DeviceAuthState = bSent ? EUnrealMcpDeviceAuthState::Pending : EUnrealMcpDeviceAuthState::Failed;
	// Surface a reason for the Failed state so the window does not silently collapse the pending instructions.
	if (!bSent)
		DeviceAuthError = TEXT("No sidecar connected.");
}

void FUnrealMcpEditorViewModel::CancelAuth()
{
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
	Config.CloudToken.Reset();
	DeviceAuthState = EUnrealMcpDeviceAuthState::Idle;
	DeviceVerificationUrl.Reset();
	DeviceUserCode.Reset();
	DeviceAuthError.Reset();
	if (OnSendAuth)
		OnSendAuth(TEXT("auth-revoke"));
	// CloudToken changed — persist (Save restores env/.env overrides) and push the now-anonymous config.
	PersistAndPush();
	// The Cloud-mode bearer the configurators inject is now gone — refresh so the snippet drops it.
	OnConnectionSettingsChanged.Broadcast();
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
