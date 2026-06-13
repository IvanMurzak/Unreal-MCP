// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"

/**
 * Claude Desktop configurator (docs/ARCHITECTURE.md §7). Per-OS GLOBAL config file under the user profile, nesting
 * under "mcpServers" — mirrors Unity's ClaudeDesktopConfigurator. Unlike Claude Code (Anthropic's terminal agent,
 * project-local .mcp.json) this is the desktop app's machine-global config:
 *   - Windows: %APPDATA%\Claude\claude_desktop_config.json
 *   - macOS:   ~/Library/Application Support/Claude/claude_desktop_config.json
 *   - Linux:   ~/.config/Claude/claude_desktop_config.json
 * The path is independent of @p ProjectRoot (global config), so GetConfigFilePath ignores it.
 */
class FClaudeDesktopConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Claude Desktop"); }
	virtual FString GetAgentId() const override { return TEXT("claude-desktop"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://code.claude.com/docs/en/desktop"); }
	virtual FString GetIconFileName() const override { return TEXT("claude-64.png"); }

	virtual FString GetConfigFilePath(const FString& /*InProjectRoot*/) const override
	{
#if PLATFORM_WINDOWS
		const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(AppData, TEXT("Claude"), TEXT("claude_desktop_config.json")));
#elif PLATFORM_MAC
		const FString Home = FPlatformProcess::UserHomeDir();
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Home, TEXT("Library"), TEXT("Application Support"), TEXT("Claude"), TEXT("claude_desktop_config.json")));
#else
		const FString Home = FPlatformProcess::UserHomeDir();
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Home, TEXT(".config"), TEXT("Claude"), TEXT("claude_desktop_config.json")));
#endif
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
};
