// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Misc/Paths.h"
#include "Dom/JsonValue.h"

/**
 * Open Code configurator (docs/ARCHITECTURE.md §7). Writes project-local `opencode.json`, nesting under "mcp" — mirrors
 * Unity's OpenCodeConfigurator. Open Code's schema is distinct from the canonical MCP shape, so both transports are
 * built explicitly:
 *   - STDIO: `{ type: "local", enabled: true, command: [<server>, <args...>] }` — the binary AND its args live in a
 *     single `command` ARRAY (there is no separate `args` key);
 *   - HTTP:  `{ type: "remote", enabled: true, url: "<host>/mcp" }`.
 */
class FOpenCodeConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Open Code"); }
	virtual FString GetAgentId() const override { return TEXT("open-code"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://opencode.ai/download"); }
	virtual FString GetIconFileName() const override { return TEXT("open-code-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT("opencode.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcp"); }

protected:
	virtual TSharedRef<FAiAgentConfig> BuildStdio() const override
	{
		TSharedRef<FJsonAiAgentConfig> Config = MakeShared<FJsonAiAgentConfig>(
			GetAgentName(), GetConfigFilePath(GetProjectRoot()), GetBodyPath());

		// command is a single array: [binary, port=, client-transport=, authorization=, token=].
		TArray<TSharedPtr<FJsonValue>> Command;
		Command.Add(MakeShared<FJsonValueString>(GetStdioCommand()));
		for (const FString& Arg : GetStdioArgs())
			Command.Add(MakeShared<FJsonValueString>(Arg));

		Config->SetStringProperty(TEXT("type"), TEXT("local"), /*bRequired*/ true);
		Config->SetProperty(TEXT("enabled"), MakeShared<FJsonValueBoolean>(true), /*bRequired*/ true);
		Config->SetProperty(TEXT("command"), MakeShared<FJsonValueArray>(Command), /*bRequired*/ true);
		// `url`/`args` belong to the other shape — scrub them.
		Config->SetPropertyToRemove(TEXT("url"));
		Config->SetPropertyToRemove(TEXT("args"));
		return Config;
	}

	virtual TSharedRef<FAiAgentConfig> BuildHttp() const override
	{
		TSharedRef<FJsonAiAgentConfig> Config = MakeShared<FJsonAiAgentConfig>(
			GetAgentName(), GetConfigFilePath(GetProjectRoot()), GetBodyPath());

		Config->SetStringProperty(TEXT("type"), TEXT("remote"), /*bRequired*/ true);
		Config->SetProperty(TEXT("enabled"), MakeShared<FJsonValueBoolean>(true), /*bRequired*/ true);
		Config->SetStringProperty(TEXT("url"), GetConnection().HttpUrl, /*bRequired*/ true, EUnrealMcpValueComparison::Url);
		// Scrub the STDIO-only keys (Open Code's STDIO `command` is an array — removing it is correct on switch).
		Config->SetPropertyToRemove(TEXT("command"));
		Config->SetPropertyToRemove(TEXT("args"));
		return Config;
	}
};
