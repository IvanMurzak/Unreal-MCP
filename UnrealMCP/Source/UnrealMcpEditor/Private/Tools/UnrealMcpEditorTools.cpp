// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "PlayInEditorDataTypes.h"
#include "ScopedTransaction.h"

/**
 * The editor-application / selection tool family (docs/ARCHITECTURE.md §10 "editor/reflection family"
 * editor-only subset, issue #19). Four native C++ EDITOR-ONLY tools, all declared via the §3.3 builder
 * and run ON the game thread (the bridge dispatches Registry.Execute through
 * FUnrealMcpGameThreadDispatcher, §4):
 *   - editor-application-get-state / editor-application-set-state — PIE state snapshot + start/stop/
 *     pause/resume. Transitions are LATENT (RequestPlaySession/RequestEndPlayMap fire next tick), so
 *     set-state returns an honest `pending` rather than a false "done".
 *   - editor-selection-get / editor-selection-set — actor selection read/write via §3.2 refs.
 *
 * The RUNTIME-SAFE subset of the original editor/reflection family — console-get-logs / console-clear-logs,
 * console-run-command, reflection-method-find / reflection-method-call — moved DOWN into the runtime
 * module's UnrealMcpConsoleReflectionTools in R4 (§12.7). These four tools stay editor-only because they
 * depend on GEditor (PIE control, the editor selection set), which does not exist in a packaged game.
 */
namespace
{
	// --- Local schema builders -------------------------------------------------------------------

	/** An `array` of strings (the §3.2 refs list for editor-selection-set). */
	TSharedPtr<FJsonObject> MakeEditorStringArraySchema(const FString& Desc)
	{
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("array"));
		if (!Desc.IsEmpty())
			Schema->SetStringField(TEXT("description"), Desc);
		Schema->SetObjectField(TEXT("items"), Items);
		return Schema;
	}

	// --- Small helpers ---------------------------------------------------------------------------

	/** Read a string-array argument; non-string entries fail via OutError (the array is cleared). */
	TArray<FString> GetEditorStringArray(const FUnrealMcpToolCall& Call, const FString& Key, FString& OutError)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Call.Arguments->TryGetArrayField(Key, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					Out.Add(S);
				}
				else
				{
					OutError = FString::Printf(TEXT("'%s' must be an array of strings; a non-string entry was provided."), *Key);
					Out.Reset();
					return Out;
				}
			}
		}
		else if (Call.Arguments->HasField(Key))
		{
			// Present but not an array (a bare string/object/number): error rather than returning an empty
			// list, which the caller would read as "select nothing" and use to silently clear the selection.
			OutError = FString::Printf(TEXT("'%s' must be an array of strings."), *Key);
		}
		return Out;
	}

	/** JSON identity for one selected actor (§3.2 ref shape; reuses the actor-family identity block). */
	TSharedPtr<FJsonObject> SelectionIdentity(const AActor* Actor)
	{
		return FUnrealMcpObjectRef::ActorIdentity(Actor);
	}
}

namespace UnrealMcpEditorTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		// ============================================================================================
		// editor-application-get-state — PIE / editor state snapshot.
		// ============================================================================================
		Registry.Tool(TEXT("editor-application-get-state"))
			.Title(TEXT("Get Editor Application State"))
			.Description(TEXT("Snapshot of the editor play state: whether Play-In-Editor is running, paused, "
			                  "or simulating, plus the current editor map. Read-only."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				if (!GEditor)
					return FUnrealMcpToolResult::Error(TEXT("No editor available (not running in the editor)."));

				const bool bPlaying = GEditor->IsPlaySessionInProgress();
				const bool bSimulating = GEditor->IsSimulateInEditorInProgress();
				bool bPaused = false;
				if (GEditor->PlayWorld)
					bPaused = GEditor->PlayWorld->bDebugPauseExecution;

				UWorld* EditorWorld = FUnrealMcpObjectRef::GetEditorWorld();
				const FString MapName = EditorWorld ? FPackageName::GetShortName(EditorWorld->GetOutermost()->GetName()) : FString();

				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetBoolField(TEXT("isPlaying"), bPlaying);
				S->SetBoolField(TEXT("isPaused"), bPaused);
				S->SetBoolField(TEXT("isSimulating"), bSimulating);
				S->SetStringField(TEXT("editorMap"), MapName);

				const FString Summary = bPlaying
					? FString::Printf(TEXT("PIE %s (map '%s')."), bPaused ? TEXT("paused") : TEXT("running"), *MapName)
					: FString::Printf(TEXT("Editor idle (map '%s')."), *MapName);
				return FUnrealMcpToolResult::Success(Summary, S);
			});

		// ============================================================================================
		// editor-application-set-state — start/stop/pause/resume PIE (latent — honest 'pending').
		// ============================================================================================
		Registry.Tool(TEXT("editor-application-set-state"))
			.Title(TEXT("Set Editor Application State"))
			.Description(TEXT("Drive Play-In-Editor: action = 'start' | 'stop' | 'pause' | 'resume'. PIE "
			                  "transitions are deferred to the next editor tick, so this returns a structured "
			                  "'pending' result; poll editor-application-get-state to confirm the transition. "
			                  "Invalid transitions (e.g. pause while not playing) return a structured error."))
			.ParamString(TEXT("action"), TEXT("'start' | 'stop' | 'pause' | 'resume'."), EUnrealMcpParamRequirement::Required)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				if (!GEditor)
					return FUnrealMcpToolResult::Error(TEXT("No editor available (not running in the editor)."));

				const FString Action = Call.GetString(TEXT("action")).ToLower();
				const bool bPlaying = GEditor->IsPlaySessionInProgress();
				const bool bPaused = GEditor->PlayWorld && GEditor->PlayWorld->bDebugPauseExecution;

				auto Pending = [&Action](const FString& Note) -> FUnrealMcpToolResult
				{
					TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
					S->SetStringField(TEXT("action"), Action);
					S->SetBoolField(TEXT("pending"), true);
					S->SetStringField(TEXT("note"), Note);
					return FUnrealMcpToolResult::Success(Note, S);
				};

				if (Action == TEXT("start"))
				{
					if (bPlaying)
						return FUnrealMcpToolResult::Error(TEXT("PIE is already running; stop it before starting again."));
					FRequestPlaySessionParams Params;
					Params.SessionDestination = EPlaySessionDestinationType::InProcess;
					GEditor->RequestPlaySession(Params);
					return Pending(TEXT("PIE start requested; it begins on the next editor tick (poll editor-application-get-state)."));
				}
				if (Action == TEXT("stop"))
				{
					if (!bPlaying)
						return FUnrealMcpToolResult::Error(TEXT("PIE is not running; nothing to stop."));
					GEditor->RequestEndPlayMap();
					return Pending(TEXT("PIE stop requested; it ends on the next editor tick (poll editor-application-get-state)."));
				}
				if (Action == TEXT("pause"))
				{
					if (!bPlaying)
						return FUnrealMcpToolResult::Error(TEXT("PIE is not running; cannot pause."));
					if (bPaused)
						return FUnrealMcpToolResult::Error(TEXT("PIE is already paused."));
					GEditor->SetPIEWorldsPaused(true);
					return Pending(TEXT("PIE pause requested."));
				}
				if (Action == TEXT("resume"))
				{
					if (!bPlaying)
						return FUnrealMcpToolResult::Error(TEXT("PIE is not running; cannot resume."));
					if (!bPaused)
						return FUnrealMcpToolResult::Error(TEXT("PIE is not paused; nothing to resume."));
					GEditor->SetPIEWorldsPaused(false);
					return Pending(TEXT("PIE resume requested."));
				}
				return FUnrealMcpToolResult::Error(FString::Printf(
					TEXT("Unknown action '%s'. Use 'start', 'stop', 'pause', or 'resume'."), *Action));
			});

		// ============================================================================================
		// editor-selection-get — currently selected actors (§3.2 identity refs).
		// ============================================================================================
		Registry.Tool(TEXT("editor-selection-get"))
			.Title(TEXT("Get Editor Selection"))
			.Description(TEXT("List the actors currently selected in the editor, each as a §3.2 identity ref "
			                  "{ name, label, class, path }. Read-only."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				if (!GEditor)
					return FUnrealMcpToolResult::Error(TEXT("No editor available (not running in the editor)."));

				USelection* Selection = GEditor->GetSelectedActors();
				TArray<TSharedPtr<FJsonValue>> Actors;
				if (Selection)
				{
					TArray<AActor*> Selected;
					Selection->GetSelectedObjects<AActor>(Selected);
					for (const AActor* Actor : Selected)
					{
						if (Actor)
							Actors.Add(MakeShared<FJsonValueObject>(SelectionIdentity(Actor)));
					}
				}

				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetNumberField(TEXT("count"), Actors.Num());
				S->SetArrayField(TEXT("actors"), Actors);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("%d actor(s) selected."), Actors.Num()), S);
			});

		// ============================================================================================
		// editor-selection-set — select actors by §3.2 ref (or clear).
		// ============================================================================================
		Registry.Tool(TEXT("editor-selection-set"))
			.Title(TEXT("Set Editor Selection"))
			.Description(TEXT("Replace the editor's actor selection with the actors named by 'actors' (label / "
			                  "name / path refs, §3.2). Pass clear=true to deselect everything."))
			.Param(TEXT("actors"), TEXT("array"), TEXT("Actor refs to select (label / name / path)."), EUnrealMcpParamRequirement::Optional, MakeEditorStringArraySchema(TEXT("Actor refs to select.")))
			.ParamBool(TEXT("clear"), TEXT("Deselect all actors. When true, 'actors' is ignored."))
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				if (!GEditor)
					return FUnrealMcpToolResult::Error(TEXT("No editor available (not running in the editor)."));

				FString ArrErr;
				const TArray<FString> Refs = GetEditorStringArray(Call, TEXT("actors"), ArrErr);
				if (!ArrErr.IsEmpty())
					return FUnrealMcpToolResult::Error(ArrErr);

				const bool bClear = Call.GetBool(TEXT("clear"));

				// Require explicit intent to deselect: a call with no actor refs and no clear flag would
				// otherwise silently wipe the whole selection and still report success. Make the caller say
				// clear=true so a malformed/empty call can't quietly nuke the user's selection.
				if (!bClear && Refs.Num() == 0)
					return FUnrealMcpToolResult::Error(TEXT(
						"No actors to select. Provide 'actors' with at least one ref, or pass clear=true to deselect everything."));

				// Resolve every requested actor BEFORE mutating the selection: a bad ref aborts the whole
				// call so the editor's selection is never left half-applied.
				TArray<AActor*> Resolved;
				if (!bClear)
				{
					for (const FString& Ref : Refs)
					{
						AActor* Actor = FUnrealMcpObjectRef::ResolveActor(Ref);
						if (!Actor)
							return FUnrealMcpToolResult::Error(FString::Printf(
								TEXT("No actor matched '%s' (by label/name/path); selection unchanged."), *Ref));
						Resolved.Add(Actor);
					}
				}

				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "EditorSelectionSet", "MCP: Set Selection"));
				GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
				for (AActor* Actor : Resolved)
					GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ false);
				GEditor->NoteSelectionChange();

				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetNumberField(TEXT("count"), Resolved.Num());
				return FUnrealMcpToolResult::Success(
					bClear ? TEXT("Selection cleared.")
					       : FString::Printf(TEXT("Selected %d actor(s)."), Resolved.Num()), S);
			});
	}
}
