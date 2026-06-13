// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Misc/Paths.h"

/**
 * Unity AI configurator (docs/ARCHITECTURE.md §7). Mirrors Unity's UnityAiConfigurator for registry parity.
 *
 * Unreal note (design decision, issue #50): Unity AI is Unity-Editor-specific and has no meaning inside an Unreal
 * project. It is retained here ONLY for 1:1 parity with the Unity agent roster (so the agent count/order matches
 * across the three engine plugins and a future shared roster spec). It writes its config exactly like any other JSON
 * agent — a project-local `UserSettings/mcp.json` under "mcpServers" (the same relative path Unity uses) — so it is
 * harmless if a user never picks it. Not blocking; recorded in design_notes.
 */
class FUnityAiConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Unity AI"); }
	virtual FString GetAgentId() const override { return TEXT("unity-ai"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://unity.com/features/ai"); }
	virtual FString GetIconFileName() const override { return TEXT("unity-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT("UserSettings"), TEXT("mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
};
