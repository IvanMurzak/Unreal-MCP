// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Misc/Paths.h"
#include "Dom/JsonValue.h"

/**
 * Rider (Junie) configurator (docs/ARCHITECTURE.md §7). Writes project-local `.junie/mcp/mcp.json`, nesting under
 * "mcpServers" — mirrors Unity's RiderConfigurator. Junie requires an explicit `enabled: true` flag on the entry and
 * has no `disabled` key, so both transports add `enabled` (required) and scrub a stale `disabled`.
 */
class FRiderConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Rider (Junie)"); }
	virtual FString GetAgentId() const override { return TEXT("rider-junie"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://www.jetbrains.com/rider/download/"); }
	virtual FString GetIconFileName() const override { return TEXT("rider-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".junie"), TEXT("mcp"), TEXT("mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
	// Per-agent skills folder (Phase C, issue #53) — mirrors Unity's RiderConfigurator.SkillsPath.
	virtual FString GetSkillsPath(const FString& /*InProjectRoot*/) const override { return TEXT(".junie/skills"); }

protected:
	virtual void CustomizeStdio(FJsonAiAgentConfig& Config) const override { ApplyEnabled(Config); }
	virtual void CustomizeHttp(FJsonAiAgentConfig& Config) const override { ApplyEnabled(Config); }

private:
	static void ApplyEnabled(FJsonAiAgentConfig& Config)
	{
		Config.SetProperty(TEXT("enabled"), MakeShared<FJsonValueBoolean>(true), /*bRequired*/ true);
		Config.SetPropertyToRemove(TEXT("disabled"));
	}
};
