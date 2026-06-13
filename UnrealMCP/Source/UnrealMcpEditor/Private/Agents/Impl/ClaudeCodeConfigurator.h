// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Misc/Paths.h"

/**
 * Claude Code configurator (docs/ARCHITECTURE.md §7). Writes the project-level `.mcp.json` at the project root,
 * nesting under "mcpServers" — exactly the cli's claude-code agent def (cli/src/lib/setup-mcp.ts) and Unity's
 * ClaudeCodeConfigurator. Phase-A reference agent #1.
 */
class FClaudeCodeConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Claude Code"); }
	virtual FString GetAgentId() const override { return TEXT("claude-code"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://docs.anthropic.com/en/docs/claude-code/overview"); }
	virtual FString GetIconFileName() const override { return TEXT("claude-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
	// Per-agent skills folder (Phase C, issue #53) — mirrors Unity's ClaudeCodeConfigurator.SkillsPath.
	virtual FString GetSkillsPath(const FString& /*InProjectRoot*/) const override { return TEXT(".claude/skills"); }
};
