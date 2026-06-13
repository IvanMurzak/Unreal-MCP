// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Misc/Paths.h"
#include "Dom/JsonValue.h"

/**
 * GitHub Copilot CLI configurator (docs/ARCHITECTURE.md §7). Uses the project-local `.mcp.json` (shared with Claude
 * Code — Copilot CLI v1.0.12+ discovers workspace-local MCP configs from the cwd up to the git root). Mirrors Unity's
 * GitHubCopilotCliConfigurator: the STDIO entry drops the `type` key and adds a `tools: ["*"]` allowlist; both
 * transports carry `tools`. The shared `.mcp.json` body key is "mcpServers".
 */
class FGitHubCopilotCliConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("GitHub Copilot CLI"); }
	virtual FString GetAgentId() const override { return TEXT("github-copilot-cli"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://github.com/features/copilot/cli"); }
	virtual FString GetIconFileName() const override { return TEXT("github-copilot-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
	// Per-agent skills folder (Phase C, issue #53) — mirrors Unity's GitHubCopilotCliConfigurator.SkillsPath
	// (Copilot CLI discovers `.claude/skills`, shared with Claude Code).
	virtual FString GetSkillsPath(const FString& /*InProjectRoot*/) const override { return TEXT(".claude/skills"); }

protected:
	virtual void CustomizeStdio(FJsonAiAgentConfig& Config) const override
	{
		// Copilot CLI infers stdio from the presence of command/args — drop the explicit "type".
		Config.SetPropertyToRemove(TEXT("type"));
		Config.SetProperty(TEXT("tools"), MakeShared<FJsonValueArray>(MakeToolsAll()), /*bRequired*/ false);
	}
	virtual void CustomizeHttp(FJsonAiAgentConfig& Config) const override
	{
		Config.SetProperty(TEXT("tools"), MakeShared<FJsonValueArray>(MakeToolsAll()), /*bRequired*/ false);
	}

private:
	static TArray<TSharedPtr<FJsonValue>> MakeToolsAll()
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		Tools.Add(MakeShared<FJsonValueString>(TEXT("*")));
		return Tools;
	}
};
