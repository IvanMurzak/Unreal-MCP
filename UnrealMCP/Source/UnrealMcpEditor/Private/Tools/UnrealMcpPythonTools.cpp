// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpSchema.h"
#include "UnrealMcpToolArgs.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "IPythonScriptPlugin.h"
#include "Interfaces/IPluginManager.h"
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

	/** Sequence of the newest captured log line (flushed first): lines after it were logged while the snippet ran. */
	static int64 PythonLastLogSequence()
	{
		GLog->Flush();
		const TArray<FUnrealMcpLogEntry> Last = FUnrealMcpLogCollector::Get().Snapshot(1);
		return Last.Num() > 0 ? Last[0].Sequence : 0;
	}

	/**
	 * Warnings and errors other categories logged after @p AfterSequence (a failed LoadAsset, a refused property
	 * write): Command.LogOutput carries only LogPython, so without them a snippet fails silently with a null result.
	 */
	static void PythonAddEngineLogs(int64 AfterSequence, TArray<TSharedPtr<FJsonValue>>& Logs)
	{
		FUnrealMcpLogCollector& Collector = FUnrealMcpLogCollector::Get();
		if (!Collector.IsRegistered())
			return;
		GLog->Flush();
		static const FName LogPythonCategory(TEXT("LogPython"));
		int32 Skipped = 0;
		int32 Added = 0;
		for (const FUnrealMcpLogEntry& Entry : Collector.Snapshot(FUnrealMcpLogCollector::EvictChunk))
		{
			const ELogVerbosity::Type Verbosity = (ELogVerbosity::Type)(Entry.Verbosity & ELogVerbosity::VerbosityMask);
			if (Entry.Sequence <= AfterSequence || Entry.Category == LogPythonCategory || Verbosity > ELogVerbosity::Warning)
				continue;
			if (Added == PythonMaxEngineLogs)
			{
				++Skipped;
				continue;
			}
			TSharedPtr<FJsonObject> Log = MakeShared<FJsonObject>();
			Log->SetStringField(TEXT("level"), Verbosity == ELogVerbosity::Warning ? TEXT("warning") : TEXT("error"));
			Log->SetStringField(TEXT("message"), FString::Printf(TEXT("%s: %s"), *Entry.Category.ToString(), *Entry.Message.TrimEnd()));
			Logs.Add(MakeShared<FJsonValueObject>(Log));
			++Added;
		}
		if (Skipped > 0)
		{
			TSharedPtr<FJsonObject> Log = MakeShared<FJsonObject>();
			Log->SetStringField(TEXT("level"), TEXT("warning"));
			Log->SetStringField(TEXT("message"), FString::Printf(TEXT("%d more engine warning(s)/error(s): read them with rql-query on the console source"), Skipped));
			Logs.Add(MakeShared<FJsonValueObject>(Log));
		}
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
			                  "call starts from a fresh namespace, and its edits are one undo step. Engine warnings and errors raised while it "
			                  "runs are returned as logs too. 'include': helper files/folders (relative to "
			                  "<Project>/.ai-game-dev/snippets): folders are importable, files run before the code. Blueprint "
			                  "graph wiring is not available from Python: use the blueprint-* tools for that."))
			.ParamString(TEXT("code"), TEXT("Python code. Example: \"actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)"
			                                "\\nlen(actors.get_all_level_actors())\""), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("include"), TEXT("array"), TEXT(""), EUnrealMcpParamRequirement::Optional,
				FUnrealMcpSchema::StringArray(TEXT("Helper .py files or folders, relative to <Project>/.ai-game-dev/snippets or absolute.")))
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
				Command.Command = FString::Printf(TEXT("__import__('unreal_mcp.python_execute', fromlist=['run']).run('%s')"),
					*FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
				// One undo step, as rql-mutate: Ctrl+Z reverts the snippet's recorded edits, not the user's previous action.
				// The transaction buffer drops it again when nothing was recorded (a read-only snippet).
				const int64 LogSequence = PythonLastLogSequence();
				const int32 Transaction = GEditor ? GEditor->BeginTransaction(TEXT("UnrealMcp"), FText::FromString(TEXT("python-execute")), nullptr) : INDEX_NONE;
				const bool bRan = Python->ExecPythonCommandEx(Command);
				if (Transaction != INDEX_NONE)
					GEditor->EndTransaction();

				const TSharedPtr<FJsonObject> Response = bRan ? PythonDecodeResponse(Command.CommandResult) : nullptr;
				if (!Response.IsValid())
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Python bootstrap failed: %s"), *Command.CommandResult));

				// unreal.log*() and engine messages raised during the call bypass sys.stdout: add them to the logs.
				TArray<TSharedPtr<FJsonValue>> Logs = Response->GetArrayField(TEXT("logs"));
				for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
				{
					TSharedPtr<FJsonObject> Log = MakeShared<FJsonObject>();
					Log->SetStringField(TEXT("level"), PythonLogLevel(Entry.Type));
					Log->SetStringField(TEXT("message"), Entry.Output.TrimEnd());
					Logs.Add(MakeShared<FJsonValueObject>(Log));
				}
				PythonAddEngineLogs(LogSequence, Logs);
				Response->SetArrayField(TEXT("logs"), Logs);

				const FString Details = PythonDetails(Response);
				if (!Response->GetBoolField(TEXT("success")))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("%s: %s%s"),
						*Response->GetStringField(TEXT("errorKind")), *Response->GetStringField(TEXT("error")), *Details));

				FString Value;
				const bool bHasValue = Response->TryGetStringField(TEXT("value"), Value);
				return FUnrealMcpToolResult::Success((bHasValue ? Value : TEXT("null")) + Details, Response);
			});
	}
}
