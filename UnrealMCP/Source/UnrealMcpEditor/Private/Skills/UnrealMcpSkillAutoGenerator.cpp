// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Skills/UnrealMcpSkillAutoGenerator.h"

#include "Misc/Guid.h"
#include "UnrealMcpLog.h"

FUnrealMcpSkillAutoGenerator::FUnrealMcpSkillAutoGenerator(FDeps InDeps)
	: Deps(MoveTemp(InDeps))
{
}

FUnrealMcpSkillAutoGenerator::~FUnrealMcpSkillAutoGenerator()
{
	if (TickHandle.IsValid())
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
}

void FUnrealMcpSkillAutoGenerator::Schedule(const FString& Reason)
{
	if (TickHandle.IsValid())
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	PendingReason = Reason;
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
	{
		TickHandle.Reset(); // a one-shot: returning false removes it
		Flush();
		return false;
	}), FMath::Max(0.0f, Deps.DelaySeconds));
}

int32 FUnrealMcpSkillAutoGenerator::Flush()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	const FString Reason = PendingReason.IsEmpty() ? FString(TEXT("requested")) : PendingReason;
	PendingReason.Reset();
	const TArray<FString> Agents = Deps.Agents ? Deps.Agents() : TArray<FString>();
	if (Agents.IsEmpty() || !Deps.Send)
		return 0;
	int32 Sent = 0;
	for (const FString& AgentId : Agents)
	{
		if (AgentId.IsEmpty())
			continue;
		// The request the panel's "Generate skill files" button sends (SUnrealMcpAgentConfigurators::RequestGenerateSkills):
		// the sidecar resolves the agent's skills folder and writes every SKILL.md from the manifest it holds.
		TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		const FString RequestId = RequestIdPrefix + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Request->SetStringField(TEXT("type"), TEXT("agent-generate-skills"));
		Request->SetStringField(TEXT("requestId"), RequestId);
		Request->SetStringField(TEXT("agentId"), AgentId);
		if (AgentId == TEXT("other-custom") && Deps.CustomSkillsPath)
			Request->SetStringField(TEXT("customSkillsPath"), Deps.CustomSkillsPath());
		TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
		Settings->SetStringField(TEXT("projectRootPath"), Deps.ProjectRoot);
		Request->SetObjectField(TEXT("settings"), Settings);
		if (!Deps.Send(Request))
		{
			// No sidecar: the next handshake schedules a generation again.
			UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] auto-generate skills (%s): the bridge is not connected, skipped."), *Reason);
			break;
		}
		InFlight.Add(RequestId, TPair<FString, FString>(AgentId, Reason));
		++Sent;
	}
	return Sent;
}

bool FUnrealMcpSkillAutoGenerator::HandleResult(const TSharedPtr<FJsonObject>& Result)
{
	FString RequestId;
	if (!Result.IsValid() || !Result->TryGetStringField(TEXT("requestId"), RequestId) || !RequestId.StartsWith(RequestIdPrefix))
		return false;
	TPair<FString, FString> Request;
	if (!InFlight.RemoveAndCopyValue(RequestId, Request))
		return true; // ours, from before a reconnect: nothing to report
	bool bOk = false;
	Result->TryGetBoolField(TEXT("ok"), bOk);
	FString SkillsPath;
	Result->TryGetStringField(TEXT("skillsPath"), SkillsPath);
	if (bOk)
	{
		int32 Written = 0;
		int32 Pruned = 0;
		Result->TryGetNumberField(TEXT("filesWritten"), Written);
		Result->TryGetNumberField(TEXT("filesPruned"), Pruned);
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] auto-generated skills for '%s' (%s): %d file(s) written, %d pruned, in %s"),
			*Request.Key, *Request.Value, Written, Pruned, *SkillsPath);
	}
	else
	{
		FString Error;
		Result->TryGetStringField(TEXT("error"), Error);
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] auto-generate skills for '%s' (%s) failed: %s"), *Request.Key, *Request.Value, *Error);
	}
	return true;
}
