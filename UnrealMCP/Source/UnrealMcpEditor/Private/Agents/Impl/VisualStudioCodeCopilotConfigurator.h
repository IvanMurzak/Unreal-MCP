// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Misc/Paths.h"

/**
 * Visual Studio Code (Copilot) configurator (docs/ARCHITECTURE.md §7). Writes project-local `.vscode/mcp.json` —
 * matching the cli's `vscode` agent def (cli/src/lib/setup-mcp.ts) and Unity's VisualStudioCodeCopilotConfigurator.
 * VS Code nests its servers under "servers" (NOT "mcpServers" — writing mcpServers there is silently ignored).
 */
class FVisualStudioCodeCopilotConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Visual Studio Code (Copilot)"); }
	virtual FString GetAgentId() const override { return TEXT("vscode-copilot"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://code.visualstudio.com/download"); }
	virtual FString GetTutorialUrl() const override { return TEXT("https://www.youtube.com/watch?v=ZhP7Ju91mOE"); }
	virtual FString GetIconFileName() const override { return TEXT("vs-code-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".vscode"), TEXT("mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("servers"); }
};
