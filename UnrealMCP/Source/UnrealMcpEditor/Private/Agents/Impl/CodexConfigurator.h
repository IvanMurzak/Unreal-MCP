// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/TomlAiAgentConfig.h"
#include "Misc/Paths.h"

/**
 * Codex configurator (docs/ARCHITECTURE.md §7/§8). The one Phase-B agent that writes TOML: project-local
 * `.codex/config.toml`, server entry under the dotted-table `[mcp_servers.unreal-mcp]` — mirrors Unity's
 * CodexConfigurator. Built on FTomlAiAgentConfig (the second concrete FAiAgentConfig subclass after the JSON one).
 *
 * STDIO: `enabled=true`, `command=<server>` (Path-compared), `args=[port, client-transport, authorization]`,
 *        optional `tool_timeout_sec=300`; removes url/type/startup_timeout_sec.
 * HTTP:  `enabled=true`, `url=<host>/mcp` (Url-compared), optional `tool_timeout_sec=300`/`startup_timeout_sec=30`;
 *        removes command/args/type.
 *
 * Token discipline (§8): Codex reads its bearer from an ENVIRONMENT VARIABLE, not from the config file. We therefore
 * NEVER write the raw token into the TOML — the STDIO args omit `token=` entirely, and HTTP auth is expressed only as
 * a `bearer_token_env_var` indirection (the env-var NAME, never the value) when auth is required. This keeps the
 * on-disk Codex config secret-free.
 */
class FCodexConfigurator : public FAiAgentConfigurator
{
public:
	/** The env-var name Codex reads the bearer token from (NAME only ever written to disk; never the value). */
	static constexpr const TCHAR* EnvVarNameAuthToken = TEXT("GAME_DEV_AUTH_TOKEN");

	virtual FString GetAgentName() const override { return TEXT("Codex"); }
	virtual FString GetAgentId() const override { return TEXT("codex"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://openai.com/codex/"); }
	virtual FString GetIconFileName() const override { return TEXT("codex-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".codex"), TEXT("config.toml")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcp_servers"); }
	// Per-agent skills folder (Phase C, issue #53) — mirrors Unity's CodexConfigurator.SkillsPath.
	virtual FString GetSkillsPath(const FString& /*InProjectRoot*/) const override { return TEXT(".agents/skills"); }

protected:
	virtual TSharedRef<FAiAgentConfig> BuildStdio() const override
	{
		TSharedRef<FTomlAiAgentConfig> Config = MakeShared<FTomlAiAgentConfig>(
			GetAgentName(), GetConfigFilePath(GetProjectRoot()), GetBodyPath());

		// Codex STDIO args mirror the shared CLI contract but OMIT the token (it comes from the env var, §8).
		const FAiAgentConnectionInfo& Conn = GetConnection();
		TArray<FString> Args;
		Args.Add(FString::Printf(TEXT("port=%d"), Conn.Port));
		Args.Add(TEXT("client-transport=stdio"));
		Args.Add(FString::Printf(TEXT("authorization=%s"), Conn.bAuthRequired ? TEXT("required") : TEXT("none")));

		Config->SetBoolProperty(TEXT("enabled"), true, /*bRequired*/ true);
		Config->SetStringProperty(TEXT("command"), GetStdioCommand(), /*bRequired*/ true, EUnrealMcpValueComparison::Path);
		Config->SetStringArrayProperty(TEXT("args"), Args, /*bRequired*/ true);
		Config->SetIntProperty(TEXT("tool_timeout_sec"), 300, /*bRequired*/ false);
		Config->SetPropertyToRemove(TEXT("url"));
		Config->SetPropertyToRemove(TEXT("type"));
		Config->SetPropertyToRemove(TEXT("startup_timeout_sec"));

		// Auth indirection: when required, point Codex at the env var holding the token (NAME only).
		if (Conn.bAuthRequired && !Conn.Token.IsEmpty())
			Config->SetStringProperty(TEXT("bearer_token_env_var"), EnvVarNameAuthToken, /*bRequired*/ true);
		else
			Config->SetPropertyToRemove(TEXT("bearer_token_env_var"));

		return Config;
	}

	virtual TSharedRef<FAiAgentConfig> BuildHttp() const override
	{
		TSharedRef<FTomlAiAgentConfig> Config = MakeShared<FTomlAiAgentConfig>(
			GetAgentName(), GetConfigFilePath(GetProjectRoot()), GetBodyPath());

		const FAiAgentConnectionInfo& Conn = GetConnection();
		Config->SetBoolProperty(TEXT("enabled"), true, /*bRequired*/ true);
		Config->SetStringProperty(TEXT("url"), Conn.HttpUrl, /*bRequired*/ true, EUnrealMcpValueComparison::Url);
		Config->SetIntProperty(TEXT("tool_timeout_sec"), 300, /*bRequired*/ false);
		Config->SetIntProperty(TEXT("startup_timeout_sec"), 30, /*bRequired*/ false);
		Config->SetPropertyToRemove(TEXT("command"));
		Config->SetPropertyToRemove(TEXT("args"));
		Config->SetPropertyToRemove(TEXT("type"));

		if (Conn.bAuthRequired && !Conn.Token.IsEmpty())
			Config->SetStringProperty(TEXT("bearer_token_env_var"), EnvVarNameAuthToken, /*bRequired*/ true);
		else
			Config->SetPropertyToRemove(TEXT("bearer_token_env_var"));

		return Config;
	}
};
