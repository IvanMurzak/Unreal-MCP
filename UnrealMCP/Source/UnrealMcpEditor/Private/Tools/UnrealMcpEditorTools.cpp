// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#if __has_include("Misc/StringOutputDevice.h")
#include "Misc/StringOutputDevice.h"
#else
#include "Containers/UnrealString.h"
#endif

/**
 * The console tool family: `console-run-command` (GEngine->Exec with captured output), declared via the §3.3 builder
 * and run ON the game thread (the bridge dispatches Registry.Execute through FUnrealMcpGameThreadDispatcher, §4).
 *
 * The rest of the former editor / console / reflection family moved to RQL (docs/rql.md): editor-application-* and
 * editor-selection-* are the `editor` source members (IsPlaying, IsPaused, Selection), console-get-logs /
 * console-clear-logs are the `console` source and `editor | call ClearConsole()`, reflection-method-* is RQL `call`
 * and rql-schema. Running a console command stays a tool, like Unity-MCP keeps its script-execute tool.
 */
namespace UnrealMcpEditorTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("console-run-command"))
			.Title(TEXT("Run Console Command"))
			.Description(TEXT("Execute an Unreal console command (incl. CVars, e.g. 'stat fps', 'r.ScreenPercentage 80') "
			                  "via GEngine->Exec against the editor world, returning any captured output text."))
			.ParamString(TEXT("command"), TEXT("The console command line to execute."), EUnrealMcpParamRequirement::Required)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Command = Call.GetString(TEXT("command"));
				if (Command.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'command'."));
				if (!GEngine)
					return FUnrealMcpToolResult::Error(TEXT("No engine available."));

				UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();

				FStringOutputDevice Output;
				Output.SetAutoEmitLineTerminator(true);
				const bool bHandled = GEngine->Exec(World, *Command, Output);

				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetStringField(TEXT("command"), Command);
				S->SetBoolField(TEXT("handled"), bHandled);
				S->SetStringField(TEXT("output"), Output);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Ran '%s' (%s)."), *Command, bHandled ? TEXT("handled") : TEXT("not handled")), S);
			});
	}
}
