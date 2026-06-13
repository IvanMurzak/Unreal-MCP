// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Antigravity configurator (docs/ARCHITECTURE.md §7). Global config file `~/.gemini/config/mcp_config.json` (shared
 * across projects, independent of @p ProjectRoot) — mirrors Unity's AntigravityConfigurator. Two quirks vs the base
 * JSON shape:
 *   - the HTTP transport stores the URL under `serverUrl` (NOT `url`), so HTTP is built explicitly here;
 *   - both transports add a required `disabled: false`, and `serverUrl` is an identity key for dedup.
 * The STDIO transport additionally scrubs `serverUrl`/`type` so a transport switch leaves no stale HTTP key.
 */
class FAntigravityConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Antigravity"); }
	virtual FString GetAgentId() const override { return TEXT("antigravity"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://antigravity.google/download"); }
	virtual FString GetIconFileName() const override { return TEXT("antigravity-64.png"); }

	virtual FString GetConfigFilePath(const FString& /*InProjectRoot*/) const override
	{
		const FString Home = FPlatformProcess::UserHomeDir();
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Home, TEXT(".gemini"), TEXT("config"), TEXT("mcp_config.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
	// Per-agent skills folder (Phase C, issue #53) — mirrors Unity's AntigravityConfigurator.SkillsPath.
	virtual FString GetSkillsPath(const FString& /*InProjectRoot*/) const override { return TEXT(".agent/skills"); }

protected:
	virtual void CustomizeStdio(FJsonAiAgentConfig& Config) const override
	{
		Config.AddIdentityKey(TEXT("serverUrl"));
		Config.SetProperty(TEXT("disabled"), MakeShared<FJsonValueBoolean>(false), /*bRequired*/ true);
		// STDIO has no URL key — scrub Antigravity's HTTP `serverUrl` (and the base already scrubs `url`).
		Config.SetPropertyToRemove(TEXT("serverUrl"));
	}

	virtual TSharedRef<FAiAgentConfig> BuildHttp() const override
	{
		// Antigravity's HTTP transport puts the URL under `serverUrl`, not the base's `url`. Build it explicitly.
		TSharedRef<FJsonAiAgentConfig> Config = MakeShared<FJsonAiAgentConfig>(
			GetAgentName(), GetConfigFilePath(GetProjectRoot()), GetBodyPath());

		Config->AddIdentityKey(TEXT("serverUrl"));
		Config->SetProperty(TEXT("disabled"), MakeShared<FJsonValueBoolean>(false), /*bRequired*/ true);
		Config->SetStringProperty(TEXT("serverUrl"), GetConnection().HttpUrl, /*bRequired*/ true, EUnrealMcpValueComparison::Url);

		// Inject auth on the HTTP entry like the base does (header only when auth required AND a token exists).
		const FAiAgentConnectionInfo& Conn = GetConnection();
		if (Conn.bAuthRequired && !Conn.Token.IsEmpty())
		{
			TSharedPtr<FJsonObject> Headers = MakeShared<FJsonObject>();
			Headers->SetStringField(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Conn.Token));
			Config->SetProperty(TEXT("headers"), MakeShared<FJsonValueObject>(Headers), /*bRequired*/ true);
		}
		else
		{
			Config->SetPropertyToRemove(TEXT("headers"));
		}

		// Scrub the STDIO-only keys and the base `url` (Antigravity uses `serverUrl`).
		Config->SetPropertyToRemove(TEXT("command"));
		Config->SetPropertyToRemove(TEXT("args"));
		Config->SetPropertyToRemove(TEXT("url"));
		Config->SetPropertyToRemove(TEXT("type"));
		return Config;
	}
};
