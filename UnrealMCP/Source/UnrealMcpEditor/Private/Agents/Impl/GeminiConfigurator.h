// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Misc/Paths.h"

/**
 * Gemini configurator (docs/ARCHITECTURE.md §7). Writes project-local `.gemini/settings.json`, nesting under
 * "mcpServers" — mirrors Unity's GeminiConfigurator. Canonical STDIO + HTTP JSON shape (no per-agent extra keys).
 */
class FGeminiConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Gemini"); }
	virtual FString GetAgentId() const override { return TEXT("gemini"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://geminicli.com/docs/get-started/installation/"); }
	virtual FString GetIconFileName() const override { return TEXT("gemini-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".gemini"), TEXT("settings.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
};
