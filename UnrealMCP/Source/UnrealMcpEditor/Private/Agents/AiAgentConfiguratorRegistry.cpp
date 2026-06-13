// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Agents/AiAgentConfiguratorRegistry.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/Impl/ClaudeCodeConfigurator.h"
#include "Agents/Impl/CursorConfigurator.h"
#include "Agents/Impl/ClaudeDesktopConfigurator.h"
#include "Agents/Impl/VisualStudioCodeCopilotConfigurator.h"
#include "Agents/Impl/VisualStudioCopilotConfigurator.h"
#include "Agents/Impl/RiderConfigurator.h"
#include "Agents/Impl/GitHubCopilotCliConfigurator.h"
#include "Agents/Impl/GeminiConfigurator.h"
#include "Agents/Impl/AntigravityConfigurator.h"
#include "Agents/Impl/ClineConfigurator.h"
#include "Agents/Impl/OpenCodeConfigurator.h"
#include "Agents/Impl/CodexConfigurator.h"
#include "Agents/Impl/KiloCodeConfigurator.h"
#include "Agents/Impl/UnityAiConfigurator.h"
#include "Agents/Impl/ZooCodeConfigurator.h"
#include "Agents/Impl/CustomConfigurator.h"

TArray<TSharedRef<FAiAgentConfigurator>> FAiAgentConfiguratorRegistry::MakeDefault()
{
	// All Unity-parity agents. Sorted alphabetically by display name (matches Unity/Godot), which keeps the dropdown
	// stable. The Custom configurator is appended AFTER the sort so it always stays last regardless of its name
	// (the Custom-last invariant). 16 agents + Custom.
	TArray<TSharedRef<FAiAgentConfigurator>> Configurators;
	Configurators.Add(MakeShared<FAntigravityConfigurator>());
	Configurators.Add(MakeShared<FClaudeCodeConfigurator>());
	Configurators.Add(MakeShared<FClaudeDesktopConfigurator>());
	Configurators.Add(MakeShared<FClineConfigurator>());
	Configurators.Add(MakeShared<FCodexConfigurator>());
	Configurators.Add(MakeShared<FCursorConfigurator>());
	Configurators.Add(MakeShared<FGeminiConfigurator>());
	Configurators.Add(MakeShared<FGitHubCopilotCliConfigurator>());
	Configurators.Add(MakeShared<FKiloCodeConfigurator>());
	Configurators.Add(MakeShared<FOpenCodeConfigurator>());
	Configurators.Add(MakeShared<FRiderConfigurator>());
	Configurators.Add(MakeShared<FUnityAiConfigurator>());
	Configurators.Add(MakeShared<FVisualStudioCodeCopilotConfigurator>());
	Configurators.Add(MakeShared<FVisualStudioCopilotConfigurator>());
	Configurators.Add(MakeShared<FZooCodeConfigurator>());

	Configurators.Sort([](const TSharedRef<FAiAgentConfigurator>& A, const TSharedRef<FAiAgentConfigurator>& B)
	{
		return A->GetAgentName() < B->GetAgentName();
	});

	// Append the Custom configurator AFTER the sort to preserve the Custom-last invariant.
	Configurators.Add(MakeShared<FCustomConfigurator>());

	return Configurators;
}

FAiAgentConfiguratorRegistry::FAiAgentConfiguratorRegistry(TArray<TSharedRef<FAiAgentConfigurator>> InConfigurators)
	: Configurators(MoveTemp(InConfigurators))
{
}

FAiAgentConfiguratorRegistry::FAiAgentConfiguratorRegistry()
	: Configurators(MakeDefault())
{
}

int32 FAiAgentConfiguratorRegistry::Num() const
{
	return Configurators.Num();
}

TArray<FString> FAiAgentConfiguratorRegistry::GetAgentNames() const
{
	TArray<FString> Names;
	Names.Reserve(Configurators.Num());
	for (const TSharedRef<FAiAgentConfigurator>& Configurator : Configurators)
		Names.Add(Configurator->GetAgentName());
	return Names;
}

TArray<FString> FAiAgentConfiguratorRegistry::GetAgentIds() const
{
	TArray<FString> Ids;
	Ids.Reserve(Configurators.Num());
	for (const TSharedRef<FAiAgentConfigurator>& Configurator : Configurators)
		Ids.Add(Configurator->GetAgentId());
	return Ids;
}

TSharedPtr<FAiAgentConfigurator> FAiAgentConfiguratorRegistry::GetByAgentId(const FString& AgentId) const
{
	for (const TSharedRef<FAiAgentConfigurator>& Configurator : Configurators)
		if (Configurator->GetAgentId() == AgentId)
			return Configurator;
	return nullptr;
}

TSharedPtr<FAiAgentConfigurator> FAiAgentConfiguratorRegistry::GetByIndex(int32 Index) const
{
	return Configurators.IsValidIndex(Index) ? TSharedPtr<FAiAgentConfigurator>(Configurators[Index]) : nullptr;
}

int32 FAiAgentConfiguratorRegistry::GetIndexByAgentId(const FString& AgentId) const
{
	for (int32 i = 0; i < Configurators.Num(); ++i)
		if (Configurators[i]->GetAgentId() == AgentId)
			return i;
	return INDEX_NONE;
}
