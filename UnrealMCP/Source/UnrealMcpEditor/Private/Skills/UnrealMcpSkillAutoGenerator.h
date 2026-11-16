// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"

/**
 * The per-agent "auto-generate skills" setting (§7 Agent Configurators, the C++ analog of Unity's SkillAutoGenerate): for
 * every agent the user enabled it for, the skill files are regenerated (the same sidecar `agent-generate-skills` request as
 * the panel's "Generate skill files" button) when the editor's connection becomes ready and whenever the tool set changes
 * (an extension or RQL module loaded / unloaded re-registers the tools: the `unreal-rql` skill carries the modules' guides).
 *
 * Debounced: Schedule() (re)arms one ticker, so a burst of changes generates once. Generation runs in the sidecar (file IO
 * off the editor), the editor only sends the requests: nothing here blocks the game thread or shows a dialog. One log line
 * per agent when the sidecar answers. Game thread only.
 */
class UNREALMCPEDITOR_API FUnrealMcpSkillAutoGenerator
{
public:
	struct FDeps
	{
		/** The agent ids with auto-generate on (FUnrealMcpConfig::SkillAutoGenerateAgents). */
		TFunction<TArray<FString>()> Agents;
		/** The Custom agent's skills folder (FUnrealMcpConfig::SkillsPath). */
		TFunction<FString()> CustomSkillsPath;
		/** The project root the sidecar resolves the skills folders against. */
		TFunction<FString()> ProjectRoot;
		/** Sends one agent-config request to the sidecar; false when none is connected. */
		TFunction<bool(const TSharedPtr<FJsonObject>&)> Send;
		/** Debounce: seconds from the last Schedule() to the generation. */
		float DelaySeconds = 2.0f;
	};

	/** Request ids of this generator's requests start with it, so their results are not the panel's. */
	static constexpr const TCHAR* RequestIdPrefix = TEXT("auto-skills-");

	explicit FUnrealMcpSkillAutoGenerator(FDeps InDeps);
	~FUnrealMcpSkillAutoGenerator();
	FUnrealMcpSkillAutoGenerator(const FUnrealMcpSkillAutoGenerator&) = delete;
	FUnrealMcpSkillAutoGenerator& operator=(const FUnrealMcpSkillAutoGenerator&) = delete;

	/** Generate after DelaySeconds without another Schedule(); @p Reason goes into the log line. */
	void Schedule(const FString& Reason);

	/** Whether a generation is scheduled. */
	bool IsScheduled() const { return TickHandle.IsValid(); }

	/** Sends the requests now (what the ticker runs); returns how many were sent. Cancels the scheduled one. */
	int32 Flush();

	/** An `agent-config-result` of one of this generator's requests: logs it and returns true; false = not ours. */
	bool HandleResult(const TSharedPtr<FJsonObject>& Result);

private:
	FDeps Deps;
	FTSTicker::FDelegateHandle TickHandle;
	FString PendingReason;
	/** Request id -> agent id and the reason it was generated for. */
	TMap<FString, TPair<FString, FString>> InFlight;
};
