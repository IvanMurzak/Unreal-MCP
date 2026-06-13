// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Agents/AiAgentConfiguratorRegistry.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/Impl/ClaudeCodeConfigurator.h"
#include "Agents/Impl/CursorConfigurator.h"

TArray<TSharedRef<FAiAgentConfigurator>> FAiAgentConfiguratorRegistry::MakeDefault()
{
	// Phase A: the two reference agents. Sorted alphabetically by display name (matches Unity/Godot), which keeps
	// the dropdown stable as Phase B adds agents. A future Custom configurator is appended AFTER the sort so it
	// always stays last regardless of its name.
	TArray<TSharedRef<FAiAgentConfigurator>> Configurators;
	Configurators.Add(MakeShared<FClaudeCodeConfigurator>());
	Configurators.Add(MakeShared<FCursorConfigurator>());

	Configurators.Sort([](const TSharedRef<FAiAgentConfigurator>& A, const TSharedRef<FAiAgentConfigurator>& B)
	{
		return A->GetAgentName() < B->GetAgentName();
	});

	// (Phase B) append the Custom configurator here, after the sort, to preserve the Custom-last invariant.

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
