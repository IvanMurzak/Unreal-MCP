// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IPythonScriptPlugin.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Paths.h"
#include "PythonScriptTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * The Python tool family: `python-execute` runs agent code inside the editor's Python (PythonScriptPlugin) through the
 * vendored code_exec runtime (Content/Python/code_exec, IvanMurzak/Code-Exec): the last expression is the result, print
 * goes to logs, errors point at `snippet.py` lines, `unreal` is pre-imported. One request = one fresh namespace.
 */
namespace UnrealMcpPythonTools
{
	static const TCHAR* PythonResponseMarker = TEXT("__UNREAL_MCP_PYTHON__");

	static TSharedPtr<FJsonObject> PythonStringArraySchema()
	{
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("array"));
		Schema->SetObjectField(TEXT("items"), Items);
		return Schema;
	}

	/** Folder that relative `include` paths resolve against: <Project>/.ai-game-dev/snippets. */
	UNREALMCPEDITOR_API FString GetPythonIncludeRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".ai-game-dev"), TEXT("snippets")));
	}

	static FString PythonContentDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetContentDir(), TEXT("Python"))) : FString();
	}

	/** The code_exec response is printed as one marker-prefixed line; pick it out of the captured Python output. */
	static TSharedPtr<FJsonObject> FindPythonResponse(const FPythonCommandEx& Command)
	{
		for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
		{
			const int32 At = Entry.Output.Find(PythonResponseMarker, ESearchCase::CaseSensitive);
			if (At == INDEX_NONE)
				continue;
			TSharedPtr<FJsonObject> Response;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Entry.Output.Mid(At + FCString::Strlen(PythonResponseMarker)).TrimStartAndEnd());
			if (FJsonSerializer::Deserialize(Reader, Response) && Response.IsValid())
				return Response;
		}
		return nullptr;
	}

	/** Logs + diagnostics as plain text lines for the human-readable message. */
	static FString PythonDetails(const TSharedPtr<FJsonObject>& Response)
	{
		FString Text;
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		if (Response->TryGetArrayField(TEXT("diagnostics"), Diagnostics))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
			{
				const TSharedPtr<FJsonObject>* D = nullptr;
				if (Value.IsValid() && Value->TryGetObject(D))
					Text += FString::Printf(TEXT("\n%s(%d,%d): %s"), *(*D)->GetStringField(TEXT("file")),
						(int32)(*D)->GetNumberField(TEXT("line")), (int32)(*D)->GetNumberField(TEXT("column")), *(*D)->GetStringField(TEXT("message")));
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Logs = nullptr;
		if (Response->TryGetArrayField(TEXT("logs"), Logs) && Logs->Num() > 0)
		{
			Text += TEXT("\nLogs:");
			for (const TSharedPtr<FJsonValue>& Value : *Logs)
			{
				const TSharedPtr<FJsonObject>* L = nullptr;
				if (Value.IsValid() && Value->TryGetObject(L))
					Text += FString::Printf(TEXT("\n[%s] %s"), *(*L)->GetStringField(TEXT("level")), *(*L)->GetStringField(TEXT("message")));
			}
		}
		return Text;
	}

	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("python-execute"))
			.Title(TEXT("Execute Python"))
			.Description(TEXT("Run Python inside the Unreal Editor (PythonScriptPlugin, full `unreal` API, pre-imported). Write "
			                  "plain statements; the LAST EXPRESSION is the result (objects come back as their paths). "
			                  "print() output is returned as logs; errors point at snippet.py lines. Each call starts from a "
			                  "fresh namespace. 'include': helper files/folders (relative to <Project>/.ai-game-dev/snippets): "
			                  "folders are importable, files run before the code. Blueprint graph wiring is not available "
			                  "from Python: use the blueprint-* tools for that."))
			.ParamString(TEXT("code"), TEXT("Python code. Example: \"actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)"
			                                "\\nlen(actors.get_all_level_actors())\""), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("include"), TEXT("array"), TEXT("Helper .py files or folders, relative to <Project>/.ai-game-dev/snippets or absolute."),
				EUnrealMcpParamRequirement::Optional, PythonStringArraySchema())
			.OpenWorldHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Code = Call.GetString(TEXT("code"));
				if (Code.TrimStartAndEnd().IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'code'."));

				IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
				if (Python == nullptr || !Python->IsPythonAvailable())
					return FUnrealMcpToolResult::Error(TEXT("NotAvailable: Python is not available in this editor (PythonScriptPlugin disabled or failed to start)."));
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
				if (!Python->IsPythonInitialized())
					Python->ForceEnablePythonAtRuntime();
#endif
				const FString ContentDir = PythonContentDir();
				if (ContentDir.IsEmpty() || !FPaths::DirectoryExists(ContentDir))
					return FUnrealMcpToolResult::Error(TEXT("NotAvailable: the plugin's Content/Python folder (code_exec runtime) is missing."));

				TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
				Request->SetStringField(TEXT("code"), Code);
				Request->SetStringField(TEXT("includeRoot"), GetPythonIncludeRoot());
				TArray<TSharedPtr<FJsonValue>> Imports;
				Imports.Add(MakeShared<FJsonValueString>(TEXT("unreal")));
				Request->SetArrayField(TEXT("imports"), Imports);
				const TArray<TSharedPtr<FJsonValue>>* Include = nullptr;
				if (Call.Arguments->TryGetArrayField(TEXT("include"), Include))
					Request->SetArrayField(TEXT("include"), *Include);
				FString RequestJson;
				FJsonSerializer::Serialize(Request.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&RequestJson));
				const FTCHARToUTF8 Utf8(*RequestJson);
				const FString Payload = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

				// The request travels as base64 inside one string literal: no quoting of agent code is ever needed.
				const FString Dir = ContentDir.Replace(TEXT("\\"), TEXT("/"));
				FPythonCommandEx Command;
				Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
				Command.FileExecutionScope = EPythonFileExecutionScope::Private;
				Command.Flags = EPythonCommandFlags::Unattended;
				Command.Command = FString::Printf(
					TEXT("import sys\nif r'%s' not in sys.path:\n    sys.path.insert(0, r'%s')\nimport unreal_mcp_python\nunreal_mcp_python.run('%s')\n"),
					*Dir, *Dir, *Payload);
				Python->ExecPythonCommandEx(Command);

				const TSharedPtr<FJsonObject> Response = FindPythonResponse(Command);
				if (!Response.IsValid())
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Python bootstrap failed: %s"), *Command.CommandResult));

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
