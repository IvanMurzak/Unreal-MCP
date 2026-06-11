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
}

void FUnrealMcpEditorViewModel::SetCustomHost(const FString& InHost)
{
	// Keep the field editable as the user types, but only persist/push a host that validates — pushing a
	// malformed URL to the sidecar would just make it dial nowhere (§7 validated field).
	Config.CustomHost = InHost;
	FString Error;
	if (ValidateServerUrl(InHost, Error))
	{
		PersistAndPush();
	}
}

void FUnrealMcpEditorViewModel::SetAuthOption(EUnrealMcpAuthOption InOption)
{
	if (Config.AuthOption == InOption)
		return;
	Config.AuthOption = InOption;
	PersistAndPush();
}

void FUnrealMcpEditorViewModel::SetCustomToken(const FString& InToken)
{
	Config.CustomToken = InToken;
	PersistAndPush();
}

void FUnrealMcpEditorViewModel::GenerateCustomToken()
{
	Config.CustomToken = FUnrealMcpSidecarManager::GenerateToken();
	// Generating a token only makes sense when auth is actually required — flip it so the new token is used.
	Config.AuthOption = EUnrealMcpAuthOption::Required;
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] generated a new Custom-mode token (%s)."),
		*FUnrealMcpConfig::MaskSecret(Config.CustomToken));
	PersistAndPush();
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
	DeviceAuthState = EUnrealMcpDeviceAuthState::Pending;
	DeviceVerificationUrl.Reset();
	DeviceUserCode.Reset();
	if (OnSendAuth)
		OnSendAuth(TEXT("auth-start"));
}

void FUnrealMcpEditorViewModel::CancelAuth()
{
	DeviceAuthState = EUnrealMcpDeviceAuthState::Idle;
	DeviceVerificationUrl.Reset();
	DeviceUserCode.Reset();
	if (OnSendAuth)
		OnSendAuth(TEXT("auth-cancel"));
}

void FUnrealMcpEditorViewModel::Revoke()
{
	Config.CloudToken.Reset();
	DeviceAuthState = EUnrealMcpDeviceAuthState::Idle;
	DeviceVerificationUrl.Reset();
	DeviceUserCode.Reset();
	if (OnSendAuth)
		OnSendAuth(TEXT("auth-revoke"));
	// CloudToken changed — persist (Save restores env/.env overrides) and push the now-anonymous config.
	PersistAndPush();
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
			DeviceAuthState = EUnrealMcpDeviceAuthState::Authorized;
			// The sidecar stored the token; mirror its presence so the UI flips to the Revoke affordance.
			FString Token;
			if (DeviceAuth->TryGetStringField(TEXT("token"), Token) && !Token.IsEmpty())
				Config.CloudToken = Token;
		}
		else if (StateRaw.Equals(TEXT("failed"), ESearchCase::IgnoreCase) || StateRaw.Equals(TEXT("denied"), ESearchCase::IgnoreCase) || StateRaw.Equals(TEXT("expired"), ESearchCase::IgnoreCase))
		{
			DeviceAuthState = EUnrealMcpDeviceAuthState::Failed;
		}
		else if (StateRaw.Equals(TEXT("pending"), ESearchCase::IgnoreCase))
		{
			DeviceAuthState = EUnrealMcpDeviceAuthState::Pending;
		}
	}

	// Open the verification page the first time we learn the URL (one-shot; never re-open on poll updates).
	if (!bHadUrl && !DeviceVerificationUrl.IsEmpty() && OnOpenBrowser)
		OnOpenBrowser(DeviceVerificationUrl);
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
