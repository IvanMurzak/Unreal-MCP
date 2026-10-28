// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"

/**
 * The tool family: `unreal-tool-list`, the parity of Unity-MCP's `unity-tool-list`. It lists every tool an MCP client can
 * call (the STANDARD surface, served) INCLUDING the ones hidden from tools/list by the per-tool Visible toggle, so a
 * generic MCP client that only sees tools/list can still find a hidden tool and call it by name. Listing only: it
 * never changes visibility (the MCP Tools window does).
 */
namespace UnrealMcpToolListToolsLocal
{
	FString Inputs(const FUnrealMcpRegisteredTool& Tool)
	{
		TArray<FString> Out;
		for (const FUnrealMcpParamSpec& Param : Tool.Params)
		{
			Out.Add(FString::Printf(TEXT("%s: %s%s"), *Param.Name, *Param.JsonType,
				Param.Requirement == EUnrealMcpParamRequirement::Required ? TEXT(" (required)") : TEXT("")));
		}
		return Out.Num() ? FString::Join(Out, TEXT(", ")) : FString(TEXT("none"));
	}
}

namespace UnrealMcpToolListTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		FUnrealMcpToolRegistry* RegistryPtr = &Registry; // the registry owns this handler, so it outlives it
		Registry.Tool(TEXT("unreal-tool-list"))
			.Title(TEXT("List Tools"))
			.Description(TEXT("List every tool of this Unreal Editor, including the ones hidden from tools/list (marked hidden): a hidden "
			                  "tool still runs when called by name. Optional regex 'filter' on the name, title and description; "
			                  "'includeDescription' / 'includeInputs' add each tool's description and parameters."))
			.ParamString(TEXT("filter"), TEXT("Case-insensitive regex matched against each tool's name, title and description."))
			.ParamBool(TEXT("includeDescription"), TEXT("Add each tool's full description (default false)."))
			.ParamBool(TEXT("includeInputs"), TEXT("Add each tool's parameters as name: type (default false)."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([RegistryPtr](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				using namespace UnrealMcpToolListToolsLocal;
				const FString Filter = Call.GetString(TEXT("filter")).TrimStartAndEnd();
				const bool bDescription = Call.GetBool(TEXT("includeDescription"));
				const bool bInputs = Call.GetBool(TEXT("includeInputs"));
				TOptional<FRegexPattern> Pattern;
				if (!Filter.IsEmpty())
					Pattern.Emplace(Filter, ERegexPatternFlags::CaseInsensitive);

				TArray<FString> Lines;
				TArray<TSharedPtr<FJsonValue>> Rows;
				int32 Hidden = 0;
				for (const FString& Name : RegistryPtr->GetToolNamesSorted())
				{
					const FUnrealMcpRegisteredTool* Tool = RegistryPtr->Find(Name);
					// System tools are never callable through tools/call; a tool outside the enabledTools whitelist is not served.
					if (!Tool || !Tool->bEnabled || Tool->ToolType != EUnrealMcpToolType::Standard)
						continue;
					if (Pattern.IsSet())
					{
						FRegexMatcher Matcher(Pattern.GetValue(), Tool->Name + TEXT("\n") + Tool->Title + TEXT("\n") + Tool->Description);
						if (!Matcher.FindNext())
							continue;
					}
					Hidden += Tool->bVisible ? 0 : 1;
					const FString InputsText = bInputs ? Inputs(*Tool) : FString();
					FString Line = FString::Printf(TEXT("%s | %s | %s"), *Tool->Name, *Tool->Title, Tool->bVisible ? TEXT("visible") : TEXT("hidden"));
					if (Tool->ExtensionId != TEXT("core"))
						Line += TEXT(" | ") + Tool->ExtensionId;
					if (bInputs)
						Line += TEXT("\n  inputs: ") + InputsText;
					if (bDescription)
						Line += TEXT("\n  ") + Tool->Description.Replace(TEXT("\n"), TEXT("\n  "));
					Lines.Add(Line);

					TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Tool->Name);
					Row->SetStringField(TEXT("title"), Tool->Title);
					Row->SetBoolField(TEXT("visible"), Tool->bVisible);
					Row->SetStringField(TEXT("extensionId"), Tool->ExtensionId);
					if (bDescription)
						Row->SetStringField(TEXT("description"), Tool->Description);
					if (bInputs)
						Row->SetStringField(TEXT("inputs"), InputsText);
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}

				FString Text = FString::Printf(TEXT("%d tool(s)%s, %d hidden"), Lines.Num(), Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" matching '%s'"), *Filter), Hidden);
				if (Hidden > 0)
					Text += TEXT(" (hidden tools are left out of tools/list but still run when called by name)");
				if (Lines.Num() == 0 && Pattern.IsSet())
					Text += TEXT(" (no tool matches: check the regex, or call without 'filter')"); // an invalid regex matches nothing silently
				// Extensions (landscape, Control Rig, Niagara, ...) add RQL sources and operations, not tools: point there.
				Text += TEXT("\nEditor features and installed extensions are RQL sources, not tools: rql-schema lists them (e.g. landscape, controlRig).");
				Text += TEXT("\nname | title | visible/hidden [| extension]\n") + FString::Join(Lines, TEXT("\n"));
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetArrayField(TEXT("tools"), Rows);
				Structured->SetNumberField(TEXT("hidden"), Hidden);
				return FUnrealMcpToolResult::Success(Text, Structured);
			});
	}
}
