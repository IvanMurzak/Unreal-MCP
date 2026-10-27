// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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
namespace UnrealMcpEditorToolsLocal
{
	/** Log lines a console command printed, at most this many (the rest: the console source). */
	static constexpr int32 ConsoleMaxLogLines = 30;
	static constexpr int32 ConsoleMaxLogLineChars = 300;
	/** Output text in the result (the full text stays in the structured 'output' field). */
	static constexpr int32 ConsoleMaxOutputChars = 8000;

	/**
	 * The game-thread log lines logged while the command ran: many commands report only there, not to the output device
	 * (TRANSACTION UNDO names the step it undid: "Undo RQL: actors | ..."; unknown commands say nothing at all).
	 */
	static TArray<FString> ConsoleLogLinesAfter(int64 Sequence, int32& OutMore)
	{
		TArray<FString> Lines;
		OutMore = 0;
		for (const FUnrealMcpLogEntry& Entry : FUnrealMcpLogCollector::Get().SnapshotAfter(Sequence))
		{
			if (!Entry.bGameThread || Entry.Verbosity > ELogVerbosity::Log)
				continue;
			if (Lines.Num() >= ConsoleMaxLogLines)
			{
				++OutMore;
				continue;
			}
			FString Message = Entry.Message.TrimEnd();
			if (Message.Len() > ConsoleMaxLogLineChars)
				Message = Message.Left(ConsoleMaxLogLineChars) + TEXT("...");
			Lines.Add(FString::Printf(TEXT("%s: %s"), *Entry.Category.ToString(), *Message));
		}
		return Lines;
	}
}

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
				const int64 LogSequence = FUnrealMcpLogCollector::Get().GetLastSequence();
				const bool bHandled = GEngine->Exec(World, *Command, Output);
				int32 MoreLogLines = 0;
				const TArray<FString> LogLines = UnrealMcpEditorToolsLocal::ConsoleLogLinesAfter(LogSequence, MoreLogLines);

				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetStringField(TEXT("command"), Command);
				S->SetBoolField(TEXT("handled"), bHandled);
				S->SetStringField(TEXT("output"), Output);
				TArray<TSharedPtr<FJsonValue>> LogValues;
				for (const FString& Line : LogLines)
					LogValues.Add(MakeShared<FJsonValueString>(Line));
				S->SetArrayField(TEXT("log"), LogValues);
				FString Text = FString::Printf(TEXT("Ran '%s' (%s)."), *Command, bHandled ? TEXT("handled") : TEXT("not handled"));
				if (const FString Printed = Output.TrimEnd(); !Printed.IsEmpty())
				{
					using namespace UnrealMcpEditorToolsLocal;
					Text += TEXT("\nOutput:\n") + (Printed.Len() <= ConsoleMaxOutputChars ? Printed : Printed.Left(ConsoleMaxOutputChars)
						+ FString::Printf(TEXT("\n[truncated: %d of %d characters shown; the full text is in the structured 'output' field]"),
							ConsoleMaxOutputChars, Printed.Len()));
				}
				if (LogLines.Num() > 0)
					Text += TEXT("\nLog:\n") + FString::Join(LogLines, TEXT("\n"));
				if (MoreLogLines > 0)
					Text += FString::Printf(TEXT("\n(%d more log line(s): rql-query on the console source)"), MoreLogLines);
				return FUnrealMcpToolResult::Success(Text, S);
			});
	}
}
