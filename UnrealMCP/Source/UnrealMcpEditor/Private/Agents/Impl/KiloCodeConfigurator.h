// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Misc/Paths.h"
#include "Dom/JsonValue.h"

/**
 * Kilo Code configurator (docs/ARCHITECTURE.md §7). Writes project-local `.kilocode/mcp.json`, nesting under
 * "mcpServers" — mirrors Unity's KiloCodeConfigurator. Both transports add a required `disabled: false`; the HTTP
 * entry uses Kilo Code's `streamable-http` transport type rather than the plain `http` the base emits.
 */
class FKiloCodeConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Kilo Code"); }
	virtual FString GetAgentId() const override { return TEXT("kilo-code"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://app.kilo.ai/get-started"); }
	virtual FString GetIconFileName() const override { return TEXT("kilo-code-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".kilocode"), TEXT("mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
	// Per-agent skills folder (Phase C, issue #53) — mirrors Unity's KiloCodeConfigurator.SkillsPath.
	virtual FString GetSkillsPath(const FString& /*InProjectRoot*/) const override { return TEXT(".kilocode/skills"); }

protected:
	virtual void CustomizeStdio(FJsonAiAgentConfig& Config) const override
	{
		Config.SetProperty(TEXT("disabled"), MakeShared<FJsonValueBoolean>(false), /*bRequired*/ true);
	}
	virtual void CustomizeHttp(FJsonAiAgentConfig& Config) const override
	{
		Config.SetProperty(TEXT("disabled"), MakeShared<FJsonValueBoolean>(false), /*bRequired*/ true);
		Config.SetStringProperty(TEXT("type"), TEXT("streamable-http"), /*bRequired*/ true);
	}
};
