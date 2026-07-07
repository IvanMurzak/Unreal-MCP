// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpSchema.h"          // shared §3.2 schema builders (FUnrealMcpSchema::StringArray/ObjectBag)
#include "UnrealMcpToolArgs.h"        // shared FUnrealMcpToolArgs::GetStringArray
#include "Tools/UnrealMcpObjectRef.h"
#include "Tools/UnrealMcpLogCollector.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#if __has_include("Misc/StringOutputDevice.h")
#include "Misc/StringOutputDevice.h"
#else
#include "Containers/UnrealString.h"
#endif
#include "PlayInEditorDataTypes.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

/**
 * The editor / console / reflection tool family (docs/ARCHITECTURE.md §10 "editor/reflection family").
 *
 * Nine native C++ tools, all declared via the §3.3 builder and run ON the game thread (the bridge
 * dispatches Registry.Execute through FUnrealMcpGameThreadDispatcher, §4):
 *   - editor-application-get-state / editor-application-set-state — PIE state snapshot + start/stop/
 *     pause/resume. Transitions are LATENT (RequestPlaySession/RequestEndPlayMap fire next tick), so
 *     set-state returns an honest `pending` rather than a false "done".
 *   - editor-selection-get / editor-selection-set — actor selection read/write via §3.2 refs.
 *   - console-get-logs / console-clear-logs — filtered/paginated slices of the FUnrealMcpLogCollector
 *     GLog ring buffer (registered at module startup), and a clear.
 *   - console-run-command — GEngine->Exec with captured output.
 *   - reflection-method-find / reflection-method-call — UFunction discovery + safety-gated invocation
 *     (ProcessEvent over a constructed param frame; §3.2 JSON arg mapping).
 */
namespace
{
	// The string-array refs schema + reader (editor-selection-set) and the free-form args-bag schema
	// (reflection-method-call) are the shared FUnrealMcpSchema::StringArray / FUnrealMcpSchema::ObjectBag /
	// FUnrealMcpToolArgs::GetStringArray surfaces (UnrealMcpSchema.h / UnrealMcpToolArgs.h) — see the call sites
	// below. Only editor-family-specific helpers remain in this namespace.

	/** The §3.2 JSON-schema type token for a reflected FProperty (best-effort, for discovery output). */
	FString PropertyTypeName(const FProperty* Prop)
	{
		if (!Prop) return TEXT("null");
		if (Prop->IsA<FBoolProperty>()) return TEXT("boolean");
		if (Prop->IsA<FFloatProperty>() || Prop->IsA<FDoubleProperty>()) return TEXT("number");
		if (Prop->IsA<FByteProperty>() || Prop->IsA<FIntProperty>() || Prop->IsA<FInt64Property>()
			|| Prop->IsA<FUInt32Property>() || Prop->IsA<FInt8Property>() || Prop->IsA<FInt16Property>()
			|| Prop->IsA<FUInt16Property>() || Prop->IsA<FUInt64Property>())
			return TEXT("integer");
		if (Prop->IsA<FEnumProperty>()) return TEXT("string");
		if (Prop->IsA<FStrProperty>() || Prop->IsA<FNameProperty>() || Prop->IsA<FTextProperty>()) return TEXT("string");
		if (Prop->IsA<FObjectPropertyBase>() || Prop->IsA<FClassProperty>() || Prop->IsA<FSoftObjectProperty>()) return TEXT("string");
		if (Prop->IsA<FArrayProperty>() || Prop->IsA<FSetProperty>()) return TEXT("array");
		if (Prop->IsA<FStructProperty>() || Prop->IsA<FMapProperty>()) return TEXT("object");
		return TEXT("string");
	}

	/** Whether @p Function is safe to invoke from a tool (the §10 safety gate). */
	bool IsCallableFunction(const UFunction* Function)
	{
		if (!Function)
			return false;
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
			return true;
#if WITH_EDITORONLY_DATA
		// CallInEditor UFUNCTIONs are explicitly editor-invocable even when not BlueprintCallable.
		if (Function->GetBoolMetaData(TEXT("CallInEditor")))
			return true;
#endif
		return false;
	}

	/**
	 * Whether @p Function may be invoked on a class CDO (the 'class' path of reflection-method-call).
	 * Only static or CallInEditor functions are safe there: a plain instance method run on the default
	 * object mutates the shared class defaults (CDO pollution that serializes into new instances) and a
	 * world-dependent instance method can crash on a world-less CDO. Instance methods must go via 'target'.
	 */
	bool IsCdoCallableFunction(const UFunction* Function)
	{
		if (!Function)
			return false;
		if (Function->HasAnyFunctionFlags(FUNC_Static))
			return true;
#if WITH_EDITORONLY_DATA
		if (Function->GetBoolMetaData(TEXT("CallInEditor")))
			return true;
#endif
		return false;
	}

	/** JSON identity for one selected actor (§3.2 ref shape; reuses the actor-family identity block). */
	TSharedPtr<FJsonObject> SelectionIdentity(const AActor* Actor)
	{
		return FUnrealMcpObjectRef::ActorIdentity(Actor);
	}

	/** Build the per-function discovery descriptor for reflection-method-find. */
	TSharedPtr<FJsonObject> DescribeFunction(UFunction* Function)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Function->GetName());
		Json->SetStringField(TEXT("class"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : FString());
		Json->SetBoolField(TEXT("isStatic"), Function->HasAnyFunctionFlags(FUNC_Static));
		Json->SetBoolField(TEXT("isBlueprintCallable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		Json->SetBoolField(TEXT("isPure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
		Json->SetBoolField(TEXT("isNative"), Function->HasAnyFunctionFlags(FUNC_Native));
#if WITH_EDITORONLY_DATA
		Json->SetBoolField(TEXT("isCallInEditor"), Function->GetBoolMetaData(TEXT("CallInEditor")));
#else
		Json->SetBoolField(TEXT("isCallInEditor"), false);
#endif
		Json->SetBoolField(TEXT("isCallable"), IsCallableFunction(Function));

		TArray<TSharedPtr<FJsonValue>> Params;
		TSharedPtr<FJsonObject> ReturnParam;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FProperty* Prop = *It;
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Prop->GetName());
			P->SetStringField(TEXT("type"), PropertyTypeName(Prop));
			P->SetStringField(TEXT("cppType"), Prop->GetCPPType());
			const bool bReturn = Prop->HasAnyPropertyFlags(CPF_ReturnParm);
			const bool bOut = Prop->HasAnyPropertyFlags(CPF_OutParm);
			P->SetBoolField(TEXT("isReturn"), bReturn);
			P->SetBoolField(TEXT("isOut"), bOut && !bReturn);
			if (bReturn)
				ReturnParam = P;
			else
				Params.Add(MakeShared<FJsonValueObject>(P));
		}
		Json->SetArrayField(TEXT("params"), Params);
		if (ReturnParam.IsValid())
			Json->SetObjectField(TEXT("returnType"), ReturnParam);
		return Json;
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
			.Param(TEXT("actors"), TEXT("array"), TEXT("Actor refs to select (label / name / path)."), EUnrealMcpParamRequirement::Optional, FUnrealMcpSchema::StringArray(TEXT("Actor refs to select.")))
			.ParamBool(TEXT("clear"), TEXT("Deselect all actors. When true, 'actors' is ignored."))
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				if (!GEditor)
					return FUnrealMcpToolResult::Error(TEXT("No editor available (not running in the editor)."));

				FString ArrErr;
				const TArray<FString> Refs = FUnrealMcpToolArgs::GetStringArray(Call, TEXT("actors"), ArrErr);
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

		// ============================================================================================
		// console-get-logs — filtered/paginated ring-buffer slice.
		// ============================================================================================
		Registry.Tool(TEXT("console-get-logs"))
			.Title(TEXT("Get Console Logs"))
			.Description(TEXT("Read recent engine log lines captured by the plugin's GLog ring buffer "
			                  "(capped at 10000). Filter by minimum severity ('Fatal'|'Error'|'Warning'|"
			                  "'Display'|'Log'|'Verbose'|'VeryVerbose'|'All'), exact category, and a message "
			                  "substring; 'limit' returns the most-recent N after filtering. Read-only."))
			.ParamString(TEXT("verbosity"), TEXT("Minimum severity to include (default 'All')."))
			.ParamString(TEXT("category"), TEXT("Exact log category (e.g. 'LogTemp'); empty = all."))
			.ParamString(TEXT("search"), TEXT("Case-insensitive message substring; empty = all."))
			.ParamInt(TEXT("limit"), TEXT("Max entries to return (most-recent first-trimmed); default 100, <=0 = all."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				ELogVerbosity::Type MinVerbosity = ELogVerbosity::All;
				if (Call.Has(TEXT("verbosity")))
				{
					const FString Token = Call.GetString(TEXT("verbosity"));
					if (!Token.IsEmpty() && !FUnrealMcpLogCollector::ParseVerbosity(Token, MinVerbosity))
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("Unknown verbosity '%s'. Use Fatal/Error/Warning/Display/Log/Verbose/VeryVerbose/All."), *Token));
				}
				const FString Category = Call.GetString(TEXT("category"));
				const FString Search = Call.GetString(TEXT("search"));
				const int32 Limit = (int32)Call.GetInt(TEXT("limit"), 100);

				const FUnrealMcpLogCollector& Collector = FUnrealMcpLogCollector::Get();
				const TArray<FUnrealMcpLogEntry> Slice = Collector.Snapshot(MinVerbosity, Category, Search, Limit);

				TArray<TSharedPtr<FJsonValue>> Lines;
				for (const FUnrealMcpLogEntry& Entry : Slice)
				{
					TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
					L->SetStringField(TEXT("timestamp"), Entry.Timestamp.ToIso8601());
					L->SetStringField(TEXT("verbosity"), FUnrealMcpLogCollector::VerbosityToString(Entry.Verbosity));
					L->SetStringField(TEXT("category"), Entry.Category.ToString());
					L->SetStringField(TEXT("message"), Entry.Message);
					Lines.Add(MakeShared<FJsonValueObject>(L));
				}

				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetNumberField(TEXT("count"), Lines.Num());
				S->SetNumberField(TEXT("totalBuffered"), Collector.Num());
				S->SetArrayField(TEXT("logs"), Lines);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Returned %d log line(s) (%d buffered)."), Lines.Num(), Collector.Num()), S);
			});

		// ============================================================================================
		// console-clear-logs — empty the ring buffer.
		// ============================================================================================
		Registry.Tool(TEXT("console-clear-logs"))
			.Title(TEXT("Clear Console Logs"))
			.Description(TEXT("Clear the plugin's GLog ring buffer. Returns how many entries were dropped."))
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const int32 Removed = FUnrealMcpLogCollector::Get().Clear();
				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetNumberField(TEXT("cleared"), Removed);
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Cleared %d log entry(ies)."), Removed), S);
			});

		// ============================================================================================
		// console-run-command — GEngine->Exec with captured output.
		// ============================================================================================
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

		// ============================================================================================
		// reflection-method-find — UFunction discovery by class (+ optional name filter).
		// ============================================================================================
		Registry.Tool(TEXT("reflection-method-find"))
			.Title(TEXT("Find Reflection Method"))
			.Description(TEXT("Discover UFunctions on a class (native class path, short name, or Blueprint asset/"
			                  "generated-class path, §3.2). Returns signatures: params (name/type), return type, "
			                  "and flags (static/instance, BlueprintCallable, pure, CallInEditor, callable). "
			                  "Optional 'name' filters by case-insensitive substring. Read-only."))
			.ParamString(TEXT("class"), TEXT("Class ref to search (native path/name or Blueprint path)."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Optional case-insensitive substring to match the function name."))
			.ParamInt(TEXT("limit"), TEXT("Max functions to return; default 100, <=0 = all."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ClassRef = Call.GetString(TEXT("class"));
				UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassRef);
				if (!Class)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Class '%s' was not found."), *ClassRef));

				const FString NameFilter = Call.GetString(TEXT("name"));
				const int32 Limit = (int32)Call.GetInt(TEXT("limit"), 100);

				TArray<TSharedPtr<FJsonValue>> Methods;
				int32 Matched = 0;
				TSet<FName> SeenNames;
				for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					UFunction* Function = *It;
					// IncludeSuper walks most-derived first, so an override appears once per declaring class
					// in the hierarchy. Keep only the first (most-derived) occurrence of each name.
					bool bAlreadySeen = false;
					SeenNames.Add(Function->GetFName(), &bAlreadySeen);
					if (bAlreadySeen)
						continue;
					if (!NameFilter.IsEmpty() && !Function->GetName().Contains(NameFilter, ESearchCase::IgnoreCase))
						continue;
					++Matched;
					if (Limit > 0 && Methods.Num() >= Limit)
						continue; // keep counting Matched for an honest total, but stop materializing
					Methods.Add(MakeShared<FJsonValueObject>(DescribeFunction(Function)));
				}

				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetStringField(TEXT("class"), Class->GetPathName());
				S->SetNumberField(TEXT("matched"), Matched);
				S->SetNumberField(TEXT("returned"), Methods.Num());
				S->SetArrayField(TEXT("methods"), Methods);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d method(s) on '%s' (returned %d)."), Matched, *Class->GetName(), Methods.Num()), S);
			});

		// ============================================================================================
		// reflection-method-call — safety-gated ProcessEvent over a constructed param frame.
		// ============================================================================================
		Registry.Tool(TEXT("reflection-method-call"))
			.Title(TEXT("Call Reflection Method"))
			.Description(TEXT("Invoke a UFunction by name. Provide EITHER 'target' (an object/actor ref, §3.2 — "
			                  "the function is resolved and called on it) OR 'class' (the function is called on "
			                  "that class's CDO — use for static/CallInEditor functions). 'args' is a JSON object "
			                  "mapping parameter names to values (§3.2). SAFETY: only BlueprintCallable or "
			                  "CallInEditor functions may be called; anything else is rejected — and the 'class' "
			                  "(CDO) path additionally requires static/CallInEditor (instance methods must use "
			                  "'target'). ACCEPTED RISK: callable functions can still be destructive (QuitGame, "
			                  "console exec, DestroyActor), consistent with console-run-command. Returns the "
			                  "function's return value and any out-params."))
			.ParamString(TEXT("target"), TEXT("Object/actor ref to call the method on (instance methods)."))
			.ParamString(TEXT("class"), TEXT("Class ref whose CDO to call on (static/CallInEditor methods)."))
			.ParamString(TEXT("method"), TEXT("The UFunction name to invoke."), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("args"), TEXT("object"), TEXT("Parameter name -> value map (§3.2)."), EUnrealMcpParamRequirement::Optional, FUnrealMcpSchema::ObjectBag(TEXT("Parameter name -> value map.")))
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString MethodName = Call.GetString(TEXT("method"));
				if (MethodName.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'method'."));

				const FString TargetRef = Call.GetString(TEXT("target"));
				const FString ClassRef = Call.GetString(TEXT("class"));
				if (TargetRef.IsEmpty() && ClassRef.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Provide either 'target' (instance) or 'class' (CDO/static)."));
				// The receiver is EITHER/OR — reject an ambiguous pair instead of silently preferring one.
				if (!TargetRef.IsEmpty() && !ClassRef.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Provide EITHER 'target' (instance) OR 'class' (CDO/static), not both."));

				// Resolve the receiving object: an explicit instance ref, else the class CDO.
				const bool bViaCDO = TargetRef.IsEmpty();
				UObject* Target = nullptr;
				if (!TargetRef.IsEmpty())
				{
					Target = FUnrealMcpObjectRef::ResolveObject(TargetRef);
					if (!Target)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Target '%s' was not found."), *TargetRef));
				}
				else
				{
					UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassRef);
					if (!Class)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Class '%s' was not found."), *ClassRef));
					Target = Class->GetDefaultObject();
					if (!Target)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Class '%s' has no default object."), *ClassRef));
				}

				UFunction* Function = Target->FindFunction(FName(*MethodName));
				if (!Function)
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Method '%s' was not found on '%s'."), *MethodName, *Target->GetClass()->GetName()));

				// SAFETY GATE (§10): reject anything that is not explicitly callable. An unguarded ProcessEvent
				// surface would let an agent invoke arbitrary native engine functions — a security finding.
				// ACCEPTED RISK: BlueprintCallable/CallInEditor still includes destructive functions
				// (UKismetSystemLibrary::QuitGame, ExecuteConsoleCommand, AActor::K2_DestroyActor, the editor
				// scripting libraries). console-run-command already grants equivalent power, so this is in
				// keeping with the family's purpose rather than a separate escalation — gated by intent, not
				// by an allow-list of individual functions.
				if (!IsCallableFunction(Function))
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Method '%s' is not BlueprintCallable or CallInEditor; refusing to call it."), *MethodName));

				// The 'class' path runs on the CDO, which is only safe for static / CallInEditor functions
				// (see IsCdoCallableFunction). Reject a plain instance method here and steer it to 'target'.
				if (bViaCDO && !IsCdoCallableFunction(Function))
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Method '%s' is an instance method; call it via 'target' (an instance), not 'class' "
						     "(whose CDO is only for static/CallInEditor functions)."), *MethodName));

				// 'args' is optional; absent -> an empty bag (every param defaults). But a PRESENT-but-non-object
				// 'args' (e.g. an array or a bare string) is a malformed call — error rather than silently
				// dropping it to an empty bag, which would zero every parameter without telling the caller.
				const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
				if (Call.Arguments->HasField(TEXT("args")) && !(Call.Arguments->TryGetObjectField(TEXT("args"), ArgsPtr) && ArgsPtr))
					return FUnrealMcpToolResult::Error(TEXT("'args' must be a JSON object mapping parameter names to values."));
				TSharedPtr<FJsonObject> Args = ArgsPtr ? *ArgsPtr : MakeShared<FJsonObject>();

				// Build + own a parameter frame for the duration of the call (init -> import -> call -> read -> destroy).
				TArray<uint8> ParmFrame;
				ParmFrame.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
				uint8* Frame = ParmFrame.GetData();

				for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
					It->InitializeValue_InContainer(Frame);

				// Import inputs (everything that is not the return value; out-params may also be seeded).
				FString ImportError;
				for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					FProperty* Prop = *It;
					if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
						continue;
					const TSharedPtr<FJsonValue> Field = Args->TryGetField(Prop->GetName());
					if (!Field.IsValid())
						continue; // absent -> zero/default; UE handles optional/defaulted params
					void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Frame);
					FText Fail;
					if (!FJsonObjectConverter::JsonValueToUProperty(Field, Prop, ValuePtr, 0, 0, false, &Fail))
					{
						ImportError = FString::Printf(TEXT("Failed to convert argument '%s': %s"), *Prop->GetName(), *Fail.ToString());
						break;
					}
				}

				if (ImportError.IsEmpty())
				{
					// AActor::ProcessEvent gates script execution in the editor: for an actor in the editor
					// world (not PIE, not a CDO) a plain BlueprintCallable (non-CallInEditor) function is
					// SILENTLY SKIPPED unless GAllowActorScriptExecutionInEditor is set — the tool would
					// otherwise return Success with a zeroed return/out-params. Scope the call in the same
					// guard the editor's own details-panel / CallInEditor invocations use (it also resets the
					// runaway-loop counter). The §10 safety gate above remains the authorization layer; this
					// only lets an already-authorized call actually run.
					FEditorScriptExecutionGuard ScriptGuard;
					Target->ProcessEvent(Function, Frame);
				}

				// Read the return value + any out-params back into JSON (before destroying the frame).
				TSharedPtr<FJsonObject> Returned = MakeShared<FJsonObject>();
				TSharedPtr<FJsonValue> ReturnValue;
				if (ImportError.IsEmpty())
				{
					for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
					{
						FProperty* Prop = *It;
						const bool bReturn = Prop->HasAnyPropertyFlags(CPF_ReturnParm);
						const bool bOut = Prop->HasAnyPropertyFlags(CPF_OutParm);
						if (!bReturn && !bOut)
							continue;
						const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Frame);
						TSharedPtr<FJsonValue> JV = FJsonObjectConverter::UPropertyToJsonValue(Prop, ValuePtr);
						if (bReturn)
							ReturnValue = JV;
						else
							Returned->SetField(Prop->GetName(), JV);
					}
				}

				// Always destroy the frame (init succeeded for every param above).
				for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
					It->DestroyValue_InContainer(Frame);

				if (!ImportError.IsEmpty())
					return FUnrealMcpToolResult::Error(ImportError);

				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetStringField(TEXT("method"), MethodName);
				S->SetStringField(TEXT("target"), Target->GetPathName());
				if (ReturnValue.IsValid())
					S->SetField(TEXT("returnValue"), ReturnValue);
				S->SetObjectField(TEXT("outParams"), Returned);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Called '%s' on '%s'."), *MethodName, *Target->GetName()), S);
			});
	}
}
