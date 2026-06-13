// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Misc/Paths.h"
#include "Dom/JsonValue.h"

/**
 * Zoo Code configurator (docs/ARCHITECTURE.md §7). Writes project-local `.roo/mcp.json`, nesting under "mcpServers"
 * — mirrors Unity's ZooCodeConfigurator (a Roo Code fork: config lives under `.roo/`). Same shape as Kilo Code:
 * required `disabled: false` on both transports, `streamable-http` for the HTTP type.
 */
class FZooCodeConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Zoo Code"); }
	virtual FString GetAgentId() const override { return TEXT("zoo-code"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://www.zoocode.dev/"); }
	virtual FString GetIconFileName() const override { return TEXT("zoo-code-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".roo"), TEXT("mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
	// Per-agent skills folder (Phase C, issue #53) — mirrors Unity's ZooCodeConfigurator.SkillsPath.
	virtual FString GetSkillsPath(const FString& /*InProjectRoot*/) const override { return TEXT(".roo/skills"); }

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
