// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Misc/Paths.h"

/**
 * Visual Studio (Copilot) configurator (docs/ARCHITECTURE.md §7). Writes project-local `.vs/mcp.json`, nesting under
 * "servers" — mirrors Unity's VisualStudioCopilotConfigurator. Visual Studio (the full IDE) and VS Code are distinct
 * agents with distinct config files; both Copilot integrations use the "servers" body key.
 */
class FVisualStudioCopilotConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Visual Studio (Copilot)"); }
	virtual FString GetAgentId() const override { return TEXT("vs-copilot"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://visualstudio.microsoft.com/downloads/"); }
	virtual FString GetTutorialUrl() const override { return TEXT("https://www.youtube.com/watch?v=RGdak4T69mc"); }
	virtual FString GetIconFileName() const override { return TEXT("visual-studio-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".vs"), TEXT("mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("servers"); }
};
