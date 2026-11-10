// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

/**
 * One editor warning or error about the scene or project: a row of the RQL `diagnostics` source (docs/rql.md
 * "Editor diagnostics"). The field names are shared with the Unity and Godot plugins.
 */
struct FUnrealMcpDiagnostic
{
	/** Raised each time the diagnostic appears (or changes): every new one has a higher Sequence than all before it. */
	int64 Sequence = 0;
	/** "Viewport" (on-screen messages), "Compiler" (Blueprints that fail to compile), "MessageLog:<page>" or "Log:<category>". */
	FString Source;
	/** "Error", "Warning" or "Info". */
	FString Severity;
	FString Message;
	/** How often a log line was seen (1 for the others). */
	int32 Count = 1;
	FDateTime FirstSeen;
	FDateTime LastSeen;
};

/** When a tool result reports new diagnostics (UNREAL_MCP_DIAGNOSTICS_PUSH / the UnrealMcp.DiagnosticsPush console variable). */
enum class EUnrealMcpDiagnosticsPush : uint8
{
	Off,      // never: the agent reads the `diagnostics` source itself
	Mutating, // after tools that are not read-only (the default, chosen by the agent-lab A/B)
	All,      // after every tool
};

/**
 * The editor's current warnings and errors (docs/rql.md "Editor diagnostics"), gathered on the game thread:
 * - Viewport: the on-screen messages the level viewport shows. FCoreDelegates::OnGetOnScreenMessages is broadcast as the
 *   viewport does; the engine's map warnings and the renderer warnings that never reach a delegate or the log (the
 *   renderer draws them straight onto the canvas) are derived from the same scene state the engine checks.
 * - Compiler: loaded Blueprints whose last compile failed or warned.
 * - MessageLog:<page>: the errors and warnings of the Message Log pages (Map Check, PIE, Blueprint Log, Load Errors, ...).
 * - Log:<category>: Warning / Error lines of the editor log since the editor finished starting (deduplicated; the
 *   `console` source has every line).
 * Viewport, Compiler and MessageLog rows are state: they leave the list when the problem is gone. Log rows stay (the
 * most recent MaxLogItems distinct lines).
 *
 * After a tool call the result carries the Warning / Error rows the agent has not seen yet (TakeNewBlock): a row counts as
 * seen once a note reported it or a tool's own text showed it (the rows a `diagnostics` query returned). Game thread only.
 */
class UNREALMCPRQL_API FUnrealMcpEditorDiagnostics
{
public:
	/** Most rows one tool result reports (then "+N more"). */
	static constexpr int32 MaxPushLines = 10;

	/** The query the push block names for the full list. */
	static const TCHAR* ListQuery();

	static FUnrealMcpEditorDiagnostics& Get();

	/** Re-read the editor's diagnostics. */
	void Refresh();

	/** The current diagnostics, oldest first (call Refresh first). */
	TArray<FUnrealMcpDiagnostic> Snapshot() const;

	/**
	 * Refresh, then the Warning / Error diagnostics the agent has not seen as a short block (at most MaxPushLines lines,
	 * errors first, with a pointer to the full list), and mark them seen. Those whose message @p AlreadyShown holds (the
	 * tool's own text, plain or JSON-escaped) count as seen and are left out. With @p bReport false (a call that gets no
	 * note) only those are marked and "" is returned. "" when there is nothing new.
	 */
	FString TakeNewBlock(const FString& AlreadyShown, bool bReport = true);

	/** Install the tool-result annotator (FUnrealMcpToolRegistry::SetResultAnnotator) and the console variable; Uninstall removes the annotator. */
	static void Install();
	static void Uninstall();

private:
	struct FGathered
	{
		FString Source;
		FString Severity;
		FString Message;
	};

	void GatherViewport(TArray<FGathered>& Out) const;
	void GatherCompiler(TArray<FGathered>& Out) const;
	void GatherMessageLogs(TArray<FGathered>& Out) const;
	void IngestLog(const FDateTime& Now);

	struct FRow : FUnrealMcpDiagnostic
	{
		bool bSeen = false; // reported in a note or shown in a tool's text
	};

	/** The row of @p Item: the existing one (LastSeen = Now) or a new one with the next Sequence. */
	FRow& Upsert(const FGathered& Item, const FDateTime& Now, bool& bOutAdded);

	TMap<FString, FRow> Items; // key: Source \n Severity \n Message
	int64 LastSequence = 0;
	int64 LogCursor = 0;       // FUnrealMcpLogCollector sequence read so far
	int32 LogItems = 0;        // Log:<category> rows in Items
};
