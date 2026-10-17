// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlTools.h"

#include "UnrealMcpRqlGuide.h"
#include "UnrealMcpRqlSession.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcpRqlToolsLocal
{
	FUnrealMcpToolRegistry* GRegistry = nullptr;
	TFunction<void()> GToolsChanged;

	FString Text(const char* Utf8)
	{
		return UnrealMcpRqlFromStd(Utf8);
	}

	/** The QueryResult the sidecar renders when a call arrives without the parsed plan (e.g. a direct IPC caller). */
	FUnrealMcpToolResult MissingPlan()
	{
		rql::QueryResult Result = rql::QueryResult::fail({ rql::Diagnostic::error("unsupported",
			"RQL text is parsed by the unreal-mcp-bridge sidecar; this call carried no 'plan'",
			"call the tool through the MCP server (unreal-mcp-cli run-tool rql-query ...)") });
		return FUnrealMcpToolResult::Error(UnrealMcpRqlFromStd(Result.to_json(false).dump()));
	}

	FUnrealMcpToolResult Run(const FUnrealMcpToolCall& Call, bool bAllowMutation)
	{
		const FString Plan = Call.GetString(TEXT("plan"));
		if (Plan.IsEmpty())
			return MissingPlan();
		bool bOk = false;
		const FString Json = UnrealMcpRqlTools::ExecutePlan(Plan, bAllowMutation, bAllowMutation && Call.GetBool(TEXT("dry_run"), false), bOk);
		return bOk ? FUnrealMcpToolResult::Success(Json) : FUnrealMcpToolResult::Error(Json);
	}

	void RegisterTools(FUnrealMcpToolRegistry& Registry)
	{
		const FString Skill = UnrealMcpRqlTools::SkillMarkdown();

		Registry.Tool(TEXT("rql-query"))
			.Title(TEXT("RQL / Query"))
			.Description(Text(UnrealMcpRqlGuide::QueryIntro()) + TEXT("\n\n") + Text(UnrealMcpRqlGuide::Language()) + TEXT("\n\n")
				+ Text(UnrealMcpRqlGuide::QueryExamples()))
			.SkillDescription(Text(UnrealMcpRqlGuide::QueryBlurb()))
			// The full unreal-rql skill (sources and guides of the installed modules): the sidecar writes it as the
			// `unreal-rql` SKILL.md and keeps it out of tools/list.
			.SkillBody(Skill)
			.ParamString(TEXT("query"), TEXT("RQL query, e.g. actors | where $ is PointLight | select $ref Label Location"),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) { return Run(Call, /*bAllowMutation*/ false); });

		Registry.Tool(TEXT("rql-mutate"))
			.Title(TEXT("RQL / Mutate"))
			.Description(Text(UnrealMcpRqlGuide::MutateIntro()) + TEXT("\n") + Text(UnrealMcpRqlGuide::MutateStages())
				+ Text(UnrealMcpRqlGuide::MutateExamples()) + Text(UnrealMcpRqlGuide::MutateOutro()) + TEXT("\n\n")
				+ Text(UnrealMcpRqlGuide::QuerySyntaxShort()))
			.SkillDescription(Text(UnrealMcpRqlGuide::MutateBlurb()))
			.ParamString(TEXT("query"), TEXT("RQL query ending with a set or call stage, e.g. actors | where Label == \"Player\" | set HiddenInEditor = true"),
				EUnrealMcpParamRequirement::Required)
			.ParamBool(TEXT("dry_run"), TEXT("true = preview the receipt without changing anything"))
			.DestructiveHint(true)
			.OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) { return Run(Call, /*bAllowMutation*/ true); });

		Registry.Tool(TEXT("rql-schema"))
			.Title(TEXT("RQL / Schema"))
			.Description(Text(UnrealMcpRqlGuide::SchemaDescription()))
			.SkillDescription(Text(UnrealMcpRqlGuide::SchemaBlurb()))
			.ParamString(TEXT("type"), TEXT("Optional type name, e.g. \"PointLight\", \"StaticMeshComponent\", \"Vector\""))
			.ReadOnlyHint(true)
			.OpenWorldHint(false)
			.Handle([](const FUnrealMcpToolCall& Call)
			{
				bool bOk = false;
				const FString Json = UnrealMcpRqlTools::Schema(Call.GetString(TEXT("type")), bOk);
				return bOk ? FUnrealMcpToolResult::Success(Json) : FUnrealMcpToolResult::Error(Json);
			});
	}
}

using namespace UnrealMcpRqlToolsLocal;

namespace UnrealMcpRqlTools
{
	void Register(FUnrealMcpToolRegistry& Registry)
	{
		RegisterTools(Registry);
	}

	void Attach(FUnrealMcpToolRegistry& Registry, TFunction<void()> OnChanged)
	{
		GRegistry = &Registry;
		GToolsChanged = MoveTemp(OnChanged);
	}

	void Detach()
	{
		GRegistry = nullptr;
		GToolsChanged = nullptr;
	}

	/** The module set changed (UnrealMcpRqlModule.cpp): rebuild the descriptions and push them. */
	void Republish()
	{
		if (!GRegistry)
			return;
		RegisterTools(*GRegistry); // core path: replaces the same-named entries and bumps the revision
		if (GToolsChanged)
			GToolsChanged();
	}

	FString ExecutePlan(const FString& PlanJson, bool bAllowMutation, bool bDryRun, bool& bOutOk)
	{
		FUnrealMcpRqlSession Session;
		return Session.Execute(PlanJson, bAllowMutation, bDryRun, bOutOk);
	}

	FString Schema(const FString& TypeName, bool& bOutOk)
	{
		FUnrealMcpRqlSession Session;
		return Session.Schema(TypeName, bOutOk);
	}

	FString SkillMarkdown()
	{
		FUnrealMcpRqlSession Session;
		return Session.SkillMarkdown();
	}
}
