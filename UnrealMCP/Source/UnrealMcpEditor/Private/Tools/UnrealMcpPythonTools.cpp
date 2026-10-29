// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpSchema.h"
#include "UnrealMcpToolArgs.h"
#include "UnrealMcpToolRegistry.h"
#include "Config/UnrealMcpConfig.h"
#include "Tools/UnrealMcpLogCollector.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "IPythonScriptPlugin.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Paths.h"
#include "PythonScriptTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UnrealMcpPythonTools
{
	/** <Project>/.ai-game-dev/snippets: where relative `include` paths resolve. */
	static FString PythonIncludeRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".ai-game-dev"), TEXT("snippets")));
	}

	/** True when the plugin's Content/Python (glue + vendored runtime) is installed; the engine puts it on sys.path. */
	static bool PythonRuntimeInstalled()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
		return Plugin.IsValid() && FPaths::DirectoryExists(FPaths::Combine(Plugin->GetContentDir(), TEXT("Python"), TEXT("unreal_mcp")));
	}

	static const TCHAR* PythonLogLevel(EPythonLogOutputType Type)
	{
		return Type == EPythonLogOutputType::Error ? TEXT("error") : Type == EPythonLogOutputType::Warning ? TEXT("warning") : TEXT("info");
	}

	/** Engine warnings / errors returned per call; the rest is counted (the console source has them all). */
	static constexpr int32 PythonMaxEngineLogs = 20;

	static TSharedPtr<FJsonValue> PythonLogEntry(const TCHAR* Level, const FString& Message)
	{
		TSharedPtr<FJsonObject> Log = MakeShared<FJsonObject>();
		Log->SetStringField(TEXT("level"), Level);
		Log->SetStringField(TEXT("message"), Message);
		return MakeShared<FJsonValueObject>(Log);
	}

	/**
	 * Warnings and errors other categories logged on the game thread (where the snippet runs; background threads'
	 * lines are not its doing) after @p AfterSequence: a failed LoadAsset, a refused property write. Command.LogOutput
	 * carries only LogPython, so without them a snippet fails silently with a null result. The collector is an
	 * unbuffered GLog device, so it already holds every line logged synchronously: no flush needed.
	 */
	static void PythonAddEngineLogs(int64 AfterSequence, TArray<TSharedPtr<FJsonValue>>& Logs)
	{
		static const FName LogPythonCategory(TEXT("LogPython"));
		int32 Matched = 0;
		for (const FUnrealMcpLogEntry& Entry : FUnrealMcpLogCollector::Get().SnapshotAfter(AfterSequence))
		{
			if (!Entry.bGameThread || Entry.Category == LogPythonCategory || Entry.Verbosity > ELogVerbosity::Warning)
				continue;
			if (++Matched <= PythonMaxEngineLogs)
				Logs.Add(PythonLogEntry(Entry.Verbosity == ELogVerbosity::Warning ? TEXT("warning") : TEXT("error"),
					FString::Printf(TEXT("%s: %s"), *Entry.Category.ToString(), *Entry.Message.TrimEnd())));
		}
		if (Matched > PythonMaxEngineLogs)
			Logs.Add(PythonLogEntry(TEXT("warning"), FString::Printf(
				TEXT("%d more engine warning(s)/error(s): read them with rql-query on the console source"), Matched - PythonMaxEngineLogs)));
	}

	/**
	 * Output budget per call (characters): a runaway print loop or a huge value must not flood the agent's context. The
	 * code_exec runtime (Code-Exec contract §6) already clips the value, the error and its own logs to these numbers (the
	 * logs to a little less, see PythonRuntimeLogChars); the
	 * caps here cover what the plugin adds afterwards: unreal.log*() output, engine messages and a failed bootstrap. The
	 * numbers equal the runtime's defaults and are sent explicitly, so both layers read one set.
	 */
	static constexpr int32 PythonMaxValueChars = 20000;
	static constexpr int32 PythonMaxLogChars = 20000;
	static constexpr int32 PythonMaxLogLineChars = 2000;
	static constexpr int32 PythonLogLineOverhead = 12; // the newline and "[warning] " around each message in the text form (as Code-Exec counts)
	// The runtime's own logs get a smaller budget: room for its one "left out" entry (about 120 characters with the 12 of
	// overhead, rounded up), so the plugin cap below never cuts the runtime's output a second time.
	static constexpr int32 PythonRuntimeLogChars = PythonMaxLogChars - 256;

	/** The snippet's time budget when the call sets no 'timeout' (seconds); the glue makes it code_exec's default too. */
	static constexpr int32 PythonDefaultTimeoutSeconds = 30;

	/**
	 * The highest time budget a snippet may get (code_exec's set_max_timeout_ms): what is left of the call's timeout
	 * (UNREAL_MCP_TOOL_TIMEOUT; the call may have waited for the game thread or for Python to start) less a margin for the
	 * reply, so code_exec's watchdog stops an endless loop and frees the game thread before the dispatcher gives up on the
	 * call. Never 0, which means no cap.
	 */
	static int32 PythonMaxTimeBudgetMs(const FUnrealMcpToolCall& Call)
	{
		const int32 Timeout = Call.TimeoutMs > 0 ? Call.TimeoutMs : FUnrealMcpConfig::DefaultToolTimeoutMs; // none: a direct call (tests)
		const double Waited = Call.ReceivedSeconds > 0.0 ? (FPlatformTime::Seconds() - Call.ReceivedSeconds) * 1000.0 : 0.0;
		const int32 Left = FMath::Max(Timeout - FMath::RoundToInt(Waited), 1);
		const int32 Margin = FMath::Clamp(Timeout / 10, 2000, 15000);
		return FMath::Max(Left - Margin, FMath::Max(Left / 2, 1));
	}

	/** Said when a snippet that calls setters recorded nothing for undo (UE records only objects that were Modify()'d). */
	static const TCHAR* PythonNoUndoNote = TEXT("this run recorded no undo step: direct setters (set_actor_location, set_actor_scale3d ...) "
		"do not record undo or mark the level unsaved, so TRANSACTION UNDO would revert the PREVIOUS edit instead. Call obj.modify() "
		"before changing an object, or use set_editor_property / rql-mutate, when the change must be undoable and saved.");

	/** Whether the code calls a setter-like API (set_*, add_*, attach_*, destroy_*, spawn_*): the no-undo note is only worth it then. */
	static bool PythonLooksMutating(const FString& Code)
	{
		static const FRegexPattern Pattern(TEXT("\\.(set_|add_|attach_|destroy_|spawn_|k2_set)[A-Za-z0-9_]*\\s*\\("));
		FRegexMatcher Matcher(Pattern, Code);
		return Matcher.FindNext();
	}

	/**
	 * The undo-step title of a run: "python-execute: " and the first line of the code that is not an import or a comment
	 * (at most 80 characters), so the Undo History and TRANSACTION UNDO say which run they revert.
	 */
	static FString PythonUndoTitle(const FString& Code)
	{
		static constexpr int32 MaxChars = 80;
		TArray<FString> Lines;
		Code.ParseIntoArrayLines(Lines);
		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimStartAndEnd();
			if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("import ")) || Line.StartsWith(TEXT("from ")))
				continue;
			return TEXT("python-execute: ") + (Line.Len() > MaxChars ? Line.Left(MaxChars - 3) + TEXT("...") : Line);
		}
		return TEXT("python-execute");
	}

	static FString PythonClip(const FString& Text, int32 Max, const TCHAR* Hint)
	{
		return Text.Len() <= Max ? Text : FString::Printf(TEXT("%s... [truncated: %d of %d characters shown; %s]"),
			*Text.Left(Max), Max, Text.Len(), Hint);
	}

	/**
	 * Clips the long lines the plugin added (from FirstOwn on: the runtime's lines are clipped already, and clipping them
	 * again would misreport their length), then keeps the first and the last lines within the budget and says how many
	 * were left out. Returns whether anything was shortened.
	 */
	static bool PythonCapLogs(TArray<TSharedPtr<FJsonValue>>& Logs, int32 FirstOwn)
	{
		int32 Total = 0;
		bool bClipped = false;
		TArray<int32> Sizes;
		for (int32 Index = 0; Index < Logs.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Log = Logs[Index]->AsObject();
			FString Message = Log->GetStringField(TEXT("message"));
			if (Index >= FirstOwn && Message.Len() > PythonMaxLogLineChars)
			{
				Message = PythonClip(Message, PythonMaxLogLineChars, TEXT("print less"));
				Log->SetStringField(TEXT("message"), Message);
				bClipped = true;
			}
			Sizes.Add(Message.Len() + PythonLogLineOverhead);
			Total += Sizes.Last();
		}
		if (Total <= PythonMaxLogChars)
			return bClipped;
		const int32 HeadBudget = PythonMaxLogChars * 3 / 4;
		int32 Head = 0, Used = 0;
		while (Head < Logs.Num() && Used + Sizes[Head] <= HeadBudget)
			Used += Sizes[Head++];
		int32 Tail = Logs.Num();
		while (Tail > Head && Used + Sizes[Tail - 1] <= PythonMaxLogChars)
			Used += Sizes[--Tail];
		TArray<TSharedPtr<FJsonValue>> Kept(Logs.GetData(), Head);
		Kept.Add(PythonLogEntry(TEXT("warning"), FString::Printf(TEXT("... %d log line(s) left out (output over %d characters): print less, or return a summary"),
			Tail - Head, PythonMaxLogChars)));
		Kept.Append(Logs.GetData() + Tail, Logs.Num() - Tail);
		Logs = MoveTemp(Kept);
		return true;
	}

	/** The EvaluateStatement result is the repr of a base64 string: the payload between the quotes, nothing escaped. */
	static TSharedPtr<FJsonObject> PythonDecodeResponse(const FString& CommandResult)
	{
		FString Payload = CommandResult.TrimStartAndEnd();
		if (Payload.Len() < 2)
			return nullptr;
		Payload = Payload.Mid(1, Payload.Len() - 2);
		TArray<uint8> Bytes;
		if (!FBase64::Decode(Payload, Bytes))
			return nullptr;
		const FString Json = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
		TSharedPtr<FJsonObject> Response;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Response) ? Response : nullptr;
	}

	/** Diagnostics + logs as text lines for the human-readable message (the structured response carries them too). */
	static FString PythonDetails(const TSharedPtr<FJsonObject>& Response)
	{
		FString Text;
		for (const TSharedPtr<FJsonValue>& Value : Response->GetArrayField(TEXT("diagnostics")))
		{
			const TSharedPtr<FJsonObject> D = Value->AsObject();
			Text += FString::Printf(TEXT("\n%s(%d,%d): %s"), *D->GetStringField(TEXT("file")), (int32)D->GetNumberField(TEXT("line")),
				(int32)D->GetNumberField(TEXT("column")), *D->GetStringField(TEXT("message")));
		}
		const TArray<TSharedPtr<FJsonValue>>& Logs = Response->GetArrayField(TEXT("logs"));
		if (Logs.Num() > 0)
			Text += TEXT("\nLogs:");
		for (const TSharedPtr<FJsonValue>& Value : Logs)
		{
			const TSharedPtr<FJsonObject> L = Value->AsObject();
			Text += FString::Printf(TEXT("\n[%s] %s"), *L->GetStringField(TEXT("level")), *L->GetStringField(TEXT("message")));
		}
		return Text;
	}

	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("python-execute"))
			.Title(TEXT("Execute Python"))
			.Description(TEXT("Run Python inside the Unreal Editor (PythonScriptPlugin, full `unreal` API, pre-imported). Write "
			                  "plain statements; the LAST EXPRESSION is the result (objects come back as their paths). "
			                  "print() and unreal.log*() output is returned as logs; errors point at snippet.py lines. Each "
			                  "call starts from a fresh namespace. Its recorded edits are one undo step (so the snippet itself cannot run TRANSACTION UNDO); only "
			                  "objects that are Modify()'d record (set_editor_property, editor subsystems, obj.modify() first): direct setters such as "
			                  "set_actor_location are neither undoable nor marked unsaved. Engine warnings and errors raised while it "
			                  "runs are returned as logs too. An endless pure-Python loop or runaway recursion does not freeze the editor: it is "
			                  "stopped with a Timeout error after 'timeout' seconds (default 30, capped below the tool timeout) and the editor keeps "
			                  "working (one long call into C, such as an unreal call or time.sleep, finishes first); sys.setrecursionlimit is capped "
			                  "at 3000. Threads the snippet starts with threading.Thread are stopped once the run ends (when their own Python code "
			                  "runs; not threads blocked in a C call or a queue, nor _thread.start_new_thread) and an unfired threading.Timer is "
			                  "cancelled: join the work you start. Known engine crashes that take the editor down (avoid them): importing a malformed "
			                  "or truncated OBJ through Interchange (AssetTools.import_asset_tasks / AssetImportTask) - check the file first or use "
			                  "rql-mutate editor | call ImportAsset(...) (legacy importers); constructing unreal.NiagaraPythonEmitter() directly - read "
			                  "emitters with rql-query (NiagaraSystem.Emitters, Niagara extension). Do not call this "
			                  "editor's MCP server over HTTP from a snippet: that tool call needs the game thread the snippet holds and only "
			                  "times out. Output is capped (~20k characters each for the value and the logs). 'include': helper files/folders (relative to "
			                  "<Project>/.ai-game-dev/snippets): folders are importable, files run before the code. Blueprint "
			                  "graph wiring is not available from Python: add nodes and connect pins with rql-mutate "
			                  "(unreal.asset(path).EventGraph | call AddEvent / AddCallFunction, node | call Connect)."))
			.ParamString(TEXT("code"), TEXT("Python code. Example: \"actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)"
			                                "\\nlen(actors.get_all_level_actors())\""), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("include"), TEXT("array"), TEXT(""), EUnrealMcpParamRequirement::Optional,
				FUnrealMcpSchema::StringArray(TEXT("Helper .py files or folders, relative to <Project>/.ai-game-dev/snippets or absolute.")))
			.ParamNumber(TEXT("timeout"), TEXT("Time budget in seconds, default 30: a snippet still running then is stopped with a Timeout "
			                                   "error. Capped a few seconds below the tool timeout. Use a small value (e.g. 5) to try code "
			                                   "that might not end."))
			.DestructiveHint(true)
			.OpenWorldHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Code = Call.GetString(TEXT("code"));
				if (Code.TrimStartAndEnd().IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'code'."));

				FString IncludeError;
				const TArray<FString> Include = FUnrealMcpToolArgs::GetStringArray(Call, TEXT("include"), IncludeError);
				if (!IncludeError.IsEmpty())
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("InvalidRequest: %s"), *IncludeError));
				double TimeoutSeconds = PythonDefaultTimeoutSeconds;
				const TSharedPtr<FJsonValue> Timeout = Call.Arguments->TryGetField(TEXT("timeout"));
				if (Timeout.IsValid())
				{
					// A JSON number only: UE's TryGetNumber would also read true as 1 and parse strings.
					TimeoutSeconds = Timeout->Type == EJson::Number ? Timeout->AsNumber() : 0.0;
					if (!(TimeoutSeconds > 0.0))
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("InvalidRequest: 'timeout' must be a number of seconds greater than 0 (default %d)."), PythonDefaultTimeoutSeconds));
				}

				IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
#if UE_VERSION_OLDER_THAN(5, 6, 0)
				const bool bPythonReady = Python != nullptr && Python->IsPythonAvailable();
#else
				// ForceEnablePythonAtRuntime overrides the "enabled by default" settings, but it returns true even when the
				// interpreter did not come up: IsPythonInitialized afterwards is the real check.
				if (Python != nullptr && !Python->IsPythonInitialized())
				{
					Python->ForceEnablePythonAtRuntime();
					// On a fresh editor Python is configured at the end of engine init and finishes initializing on its
					// first tick. Until then ask the dispatcher to retry on a later tick (bounded by the call's timeout).
					if (!Python->IsPythonConfigured() || (Python->IsPythonAvailable() && !Python->IsPythonInitialized()))
						return FUnrealMcpToolResult::RetryLater(TEXT("Python is still starting."));
				}
				const bool bPythonReady = Python != nullptr && Python->IsPythonInitialized();
#endif
				if (!bPythonReady)
					return FUnrealMcpToolResult::Error(TEXT("NotAvailable: Python is not available in this editor (PythonScriptPlugin disabled or failed to start)."));
				if (!PythonRuntimeInstalled())
					return FUnrealMcpToolResult::Error(TEXT("NotAvailable: the plugin's Content/Python folder (the code_exec runtime) is missing."));

				TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
				Request->SetStringField(TEXT("code"), Code);
				Request->SetStringField(TEXT("includeRoot"), PythonIncludeRoot());
				Request->SetBoolField(TEXT("includeOutsideRoot"), true); // the tool documents absolute include paths
				Request->SetNumberField(TEXT("maxValueChars"), PythonMaxValueChars);
				Request->SetNumberField(TEXT("maxLogChars"), PythonRuntimeLogChars);
				Request->SetNumberField(TEXT("maxLogLineChars"), PythonMaxLogLineChars);
				Request->SetNumberField(TEXT("maxErrorChars"), PythonMaxValueChars);
				// code_exec takes whole milliseconds and caps them at the max below (a longer budget becomes the cap).
				const int32 MaxTimeoutMs = PythonMaxTimeBudgetMs(Call);
				const int32 TimeoutMs = (int32)FMath::Clamp(FMath::CeilToDouble(TimeoutSeconds * 1000.0), 1.0, (double)MAX_int32);
				Request->SetNumberField(TEXT("timeoutMs"), TimeoutMs);
				TArray<TSharedPtr<FJsonValue>> Imports;
				Imports.Add(MakeShared<FJsonValueString>(TEXT("unreal")));
				Request->SetArrayField(TEXT("imports"), Imports);
				TArray<TSharedPtr<FJsonValue>> IncludeValues;
				for (const FString& Path : Include)
					IncludeValues.Add(MakeShared<FJsonValueString>(Path));
				Request->SetArrayField(TEXT("include"), IncludeValues);
				const FTCHARToUTF8 Utf8(*UnrealMcpSerializeCondensed(Request));

				// base64 both ways: agent code is never quoted into Python source, and the response never needs unescaping.
				FPythonCommandEx Command;
				Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
				Command.Flags = EPythonCommandFlags::Unattended;
				Command.Command = FString::Printf(TEXT("__import__('unreal_mcp.python_execute', fromlist=['run']).run('%s', %d)"),
					*FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), MaxTimeoutMs);
				// One undo step, as rql-mutate (the transaction buffer drops it when a read-only snippet recorded nothing).
				const int64 LogSequence = FUnrealMcpLogCollector::Get().GetLastSequence();
				const int32 Transaction = GEditor ? GEditor->BeginTransaction(TEXT("UnrealMcp"), FText::FromString(PythonUndoTitle(Code)), nullptr) : INDEX_NONE;
				// Only Modify()'d objects with actual changes record: after a run of direct setters (set_actor_location)
				// the transaction is dropped as empty, and an undo would revert the PREVIOUS edit. Our own (top-level)
				// transaction is the last queue entry; whether it survives EndTransaction tells (nested: unknowable).
				FGuid OwnTransaction;
				if (Transaction == 0 && GEditor->Trans && GEditor->Trans->GetQueueLength() > 0)
					if (const FTransaction* Current = GEditor->Trans->GetTransaction(GEditor->Trans->GetQueueLength() - 1))
						OwnTransaction = Current->GetContext().TransactionId;
				const bool bRan = Python->ExecPythonCommandEx(Command);
				if (Transaction != INDEX_NONE)
					GEditor->EndTransaction();
				const bool bRecordedNothing = OwnTransaction.IsValid() && GEditor->Trans->FindTransactionIndex(OwnTransaction) == INDEX_NONE;

				const TSharedPtr<FJsonObject> Response = bRan ? PythonDecodeResponse(Command.CommandResult) : nullptr;
				if (!Response.IsValid())
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Python bootstrap failed: %s"),
						*PythonClip(Command.CommandResult, PythonMaxValueChars, TEXT("see the editor log"))));

				// unreal.log*() and engine messages raised during the call bypass sys.stdout: add them to the logs.
				TArray<TSharedPtr<FJsonValue>> Logs = Response->GetArrayField(TEXT("logs"));
				const int32 FirstOwn = Logs.Num();
				for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
					Logs.Add(PythonLogEntry(PythonLogLevel(Entry.Type), Entry.Output.TrimEnd()));
				PythonAddEngineLogs(LogSequence, Logs);
				if (Timeout.IsValid() && TimeoutMs > MaxTimeoutMs)
					Logs.Add(PythonLogEntry(TEXT("info"), FString::Printf(TEXT("timeout %g s was capped at %.1f s (the tool timeout less a margin)"),
						TimeoutSeconds, MaxTimeoutMs / 1000.0)));
				if (bRecordedNothing && Response->GetBoolField(TEXT("success")) && PythonLooksMutating(Code))
					Logs.Add(PythonLogEntry(TEXT("warning"), PythonNoUndoNote));
				if (PythonCapLogs(Logs, FirstOwn))
				{
					// The response says which fields a limit shortened, in the order value, logs, error (contract §6).
					const TArray<TSharedPtr<FJsonValue>>* Truncated = nullptr;
					TArray<FString> Fields;
					if (Response->TryGetArrayField(TEXT("truncated"), Truncated))
						for (const TSharedPtr<FJsonValue>& Field : *Truncated)
							Fields.Add(Field->AsString());
					if (!Fields.Contains(TEXT("logs")))
						Fields.Insert(TEXT("logs"), Fields.Contains(TEXT("value")) ? 1 : 0);
					TArray<TSharedPtr<FJsonValue>> Values;
					for (const FString& Field : Fields)
						Values.Add(MakeShared<FJsonValueString>(Field));
					Response->SetArrayField(TEXT("truncated"), Values);
				}
				Response->SetArrayField(TEXT("logs"), Logs);
				FString Value; // already clipped by the runtime (maxValueChars), as the error is (maxErrorChars)
				const bool bHasValue = Response->TryGetStringField(TEXT("value"), Value);

				const FString Details = PythonDetails(Response);
				if (!Response->GetBoolField(TEXT("success")))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("%s: %s%s"),
						*Response->GetStringField(TEXT("errorKind")), *Response->GetStringField(TEXT("error")), *Details));

				return FUnrealMcpToolResult::Success((bHasValue ? Value : TEXT("null")) + Details, Response);
			});
	}
}
