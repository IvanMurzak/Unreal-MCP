// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"

/**
 * Cline configurator (docs/ARCHITECTURE.md §7). Cline is a VS Code extension that stores its MCP servers in a GLOBAL
 * config file under VS Code's globalStorage (shared across all projects, independent of @p ProjectRoot) — mirrors
 * Unity's ClineConfigurator:
 *   - Windows: %APPDATA%\Code\User\globalStorage\saoudrizwan.claude-dev\settings\cline_mcp_settings.json
 *   - macOS:   ~/Library/Application Support/Code/User/globalStorage/.../cline_mcp_settings.json
 *   - Linux:   ~/.config/Code/User/globalStorage/.../cline_mcp_settings.json
 * Body key "mcpServers"; the HTTP entry uses Cline's `streamableHttp` transport type.
 */
class FClineConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Cline"); }
	virtual FString GetAgentId() const override { return TEXT("cline"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://cline.bot/"); }
	virtual FString GetIconFileName() const override { return TEXT("cline-64.png"); }

	virtual FString GetConfigFilePath(const FString& /*InProjectRoot*/) const override
	{
		const TCHAR* Ext = TEXT("saoudrizwan.claude-dev");
#if PLATFORM_WINDOWS
		const FString Base = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Base, TEXT("Code"), TEXT("User"), TEXT("globalStorage"), Ext, TEXT("settings"), TEXT("cline_mcp_settings.json")));
#elif PLATFORM_MAC
		const FString Home = FPlatformProcess::UserHomeDir();
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Home, TEXT("Library"), TEXT("Application Support"), TEXT("Code"), TEXT("User"), TEXT("globalStorage"), Ext, TEXT("settings"), TEXT("cline_mcp_settings.json")));
#else
		const FString Home = FPlatformProcess::UserHomeDir();
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Home, TEXT(".config"), TEXT("Code"), TEXT("User"), TEXT("globalStorage"), Ext, TEXT("settings"), TEXT("cline_mcp_settings.json")));
#endif
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }

protected:
	virtual void CustomizeHttp(FJsonAiAgentConfig& Config) const override
	{
		// Cline uses the camelCase `streamableHttp` transport type (distinct from Kilo/Zoo's `streamable-http`).
		Config.SetStringProperty(TEXT("type"), TEXT("streamableHttp"), /*bRequired*/ true);
	}
};
