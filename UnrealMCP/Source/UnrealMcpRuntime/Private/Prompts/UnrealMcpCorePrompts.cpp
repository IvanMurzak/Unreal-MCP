// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntimeCorePrompts.h"
#include "UnrealMcpPromptRegistry.h"

/**
 * The core `level-design-brief` prompt (docs/ARCHITECTURE.md §A.1). A self-contained, engine-call-free
 * prompt that templates a level-design brief from a required `theme` argument — the prompt analog of the
 * `ping` core tool (the end-to-end smoke target for the v2 prompt path:
 * MCP server -> SignalR -> sidecar -> IPC -> UE plugin (game thread) -> back).
 */
namespace UnrealMcpCorePrompts
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpPromptRegistry& Registry)
	{
		Registry.Prompt(TEXT("level-design-brief"))
			.Title(TEXT("Level Design Brief"))
			.Description(TEXT("Generate a level design brief for a themed level — layout, pacing, key "
			                  "encounters, and mood — from a single 'theme' argument."))
			.Role(EUnrealMcpPromptRole::User)
			.ParamString(TEXT("theme"), TEXT("The level theme (e.g. 'abandoned space station', 'haunted forest')."), EUnrealMcpParamRequirement::Required)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpPromptResult
			{
				// Runs ON the game thread (the dispatcher guarantees it, §4). No engine calls — pure templating.
				const FString Theme = Call.GetString(TEXT("theme"));
				if (Theme.IsEmpty())
					return FUnrealMcpPromptResult::Error(TEXT("theme is required."));

				const FString Text = FString::Printf(
					TEXT("Draft a level design brief for a \"%s\"-themed level. Cover layout, pacing, key encounters, and mood."),
					*Theme);

				const FString Description = FString::Printf(TEXT("Level design brief for a \"%s\"-themed level."), *Theme);
				return FUnrealMcpPromptResult::Success(Text, EUnrealMcpPromptRole::User, Description);
			});
	}
}
