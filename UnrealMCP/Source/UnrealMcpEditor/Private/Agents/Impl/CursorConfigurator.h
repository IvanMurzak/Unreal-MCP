// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Misc/Paths.h"

/**
 * Cursor configurator (docs/ARCHITECTURE.md §7). Writes `.cursor/mcp.json` under the project root, nesting under
 * "mcpServers" — exactly the cli's cursor agent def (cli/src/lib/setup-mcp.ts) and Unity's CursorConfigurator.
 * Phase-A reference agent #2.
 */
class FCursorConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Cursor"); }
	virtual FString GetAgentId() const override { return TEXT("cursor"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://cursor.com/download"); }
	virtual FString GetIconFileName() const override { return TEXT("cursor-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".cursor"), TEXT("mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
};
