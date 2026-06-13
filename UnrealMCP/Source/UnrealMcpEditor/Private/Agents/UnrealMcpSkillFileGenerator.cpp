// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Agents/UnrealMcpSkillFileGenerator.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpLog.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// Collapse a possibly-multi-line description into a single padded line and cap its length so the YAML
	// front-matter `description:` stays a valid single scalar. Word-boundary trim with an ellipsis when truncated.
	// Pure; no secret handling needed (descriptions are static tool docs).
	FString SkillGenSingleLineCapped(const FString& In, int32 MaxLen)
	{
		FString Flat = In;
		Flat.ReplaceInline(TEXT("\r\n"), TEXT(" "));
		Flat.ReplaceInline(TEXT("\n"), TEXT(" "));
		Flat.ReplaceInline(TEXT("\r"), TEXT(" "));
		Flat.ReplaceInline(TEXT("\t"), TEXT(" "));
		// Collapse runs of spaces.
		while (Flat.Contains(TEXT("  ")))
			Flat.ReplaceInline(TEXT("  "), TEXT(" "));
		Flat.TrimStartAndEndInline();

		if (Flat.Len() <= MaxLen)
			return Flat;

		FString Capped = Flat.Left(MaxLen);
		int32 LastSpace = INDEX_NONE;
		if (Capped.FindLastChar(TEXT(' '), LastSpace) && LastSpace > MaxLen / 2)
			Capped = Capped.Left(LastSpace);
		Capped.TrimEndInline();
		return Capped + TEXT("...");
	}

	// Escape a YAML double-quoted scalar value (backslash + double-quote). The value is already single-lined.
	FString SkillGenEscapeYamlQuoted(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return Out;
	}

	// Pretty-print a JSON object (default pretty writer = tab indentation, which the editor renders fine).
	FString SkillGenPrettyJson(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
			return FString();
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		Out.TrimEndInline();
		return Out;
	}
}

FString FUnrealMcpSkillFileGenerator::SanitizeSkillFolderName(const FString& ToolName)
{
	FString Out;
	Out.Reserve(ToolName.Len());
	bool bPendingHyphen = false;
	for (const TCHAR Ch : ToolName)
	{
		const bool bAlnum = (Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('A') && Ch <= TEXT('Z')) || (Ch >= TEXT('0') && Ch <= TEXT('9'));
		if (bAlnum)
		{
			if (bPendingHyphen && Out.Len() > 0)
				Out.AppendChar(TEXT('-'));
			bPendingHyphen = false;
			Out.AppendChar(FChar::ToLower(Ch));
		}
		else
		{
			// Any non-alnum becomes a single hyphen separator (collapsed, never leading/trailing).
			bPendingHyphen = true;
		}
	}
	return Out.IsEmpty() ? FString(TEXT("tool")) : Out;
}

FString FUnrealMcpSkillFileGenerator::BuildSkillMarkdown(const FUnrealMcpRegisteredTool& Tool)
{
	const FString FolderName = SanitizeSkillFolderName(Tool.Name);
	const FString YamlDesc = SkillGenEscapeYamlQuoted(SkillGenSingleLineCapped(Tool.Description, MaxSkillDescriptionLength));
	const FString Title = Tool.Title.IsEmpty() ? Tool.Name : Tool.Title;

	TArray<FString> Lines;

	// YAML front-matter (Agent-Skills shape: name + description). `name` is the sanitized folder name.
	Lines.Add(TEXT("---"));
	Lines.Add(FString::Printf(TEXT("name: %s"), *FolderName));
	Lines.Add(FString::Printf(TEXT("description: \"%s\""), *YamlDesc));
	Lines.Add(TEXT("---"));
	Lines.Add(TEXT(""));

	// Title + the full description body (verbatim — it is human-readable tool documentation).
	Lines.Add(FString::Printf(TEXT("# %s"), *Title));
	Lines.Add(TEXT(""));
	if (!Tool.Description.IsEmpty())
	{
		Lines.Add(Tool.Description.TrimStartAndEnd());
		Lines.Add(TEXT(""));
	}

	// Behavioural hints (the MCP read-only / destructive / idempotent / open-world flags) — useful context for an agent.
	{
		TArray<FString> Hints;
		if (Tool.bReadOnlyHint)   Hints.Add(TEXT("read-only"));
		if (Tool.bDestructiveHint) Hints.Add(TEXT("destructive"));
		if (Tool.bIdempotentHint) Hints.Add(TEXT("idempotent"));
		if (Tool.bOpenWorldHint)  Hints.Add(TEXT("open-world"));
		if (Hints.Num() > 0)
		{
			Lines.Add(FString::Printf(TEXT("**Hints:** %s"), *FString::Join(Hints, TEXT(", "))));
			Lines.Add(TEXT(""));
		}
	}

	// ## Input — a parameter table + the JSON Schema block.
	Lines.Add(TEXT("## Input"));
	Lines.Add(TEXT(""));
	if (Tool.Params.Num() == 0)
	{
		Lines.Add(TEXT("This tool takes no parameters."));
		Lines.Add(TEXT(""));
	}
	else
	{
		Lines.Add(TEXT("| Name | Type | Required | Description |"));
		Lines.Add(TEXT("| --- | --- | --- | --- |"));
		for (const FUnrealMcpParamSpec& Param : Tool.Params)
		{
			// Keep a stray pipe in a description from breaking the table.
			FString Desc = Param.Description;
			Desc.ReplaceInline(TEXT("|"), TEXT("\\|"));
			Desc.ReplaceInline(TEXT("\n"), TEXT(" "));
			const TCHAR* Req = (Param.Requirement == EUnrealMcpParamRequirement::Required) ? TEXT("yes") : TEXT("no");
			Lines.Add(FString::Printf(TEXT("| `%s` | %s | %s | %s |"), *Param.Name, *Param.JsonType, Req, *Desc));
		}
		Lines.Add(TEXT(""));

		// The full JSON Schema for the inputs (built from the declared params).
		Lines.Add(TEXT("### Input JSON Schema"));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("```json"));
		Lines.Add(SkillGenPrettyJson(Tool.BuildInputSchema()));
		Lines.Add(TEXT("```"));
		Lines.Add(TEXT(""));
	}

	// ## Output — the structured output schema when the tool declares one.
	if (Tool.OutputSchema.IsValid())
	{
		Lines.Add(TEXT("## Output"));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("### Output JSON Schema"));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("```json"));
		Lines.Add(SkillGenPrettyJson(Tool.OutputSchema));
		Lines.Add(TEXT("```"));
		Lines.Add(TEXT(""));
	}

	// ## How to Call — the plain unreal-mcp-cli form. Token discipline (§8): NO token at all (not even a
	// placeholder); the agent supplies its own configured credentials. The skill doc stays secret-free.
	Lines.Add(TEXT("## How to Call"));
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("```bash"));
	Lines.Add(FString::Printf(TEXT("unreal-mcp-cli run-tool %s --input '{ ... }'"), *Tool.Name));
	Lines.Add(TEXT("```"));
	Lines.Add(TEXT(""));

	return FString::Join(Lines, TEXT("\n"));
}

int32 FUnrealMcpSkillFileGenerator::PruneStaleSkillFolders(const FString& SkillsRootAbsolute, const TSet<FString>& CurrentFolderNames)
{
	IFileManager& FileManager = IFileManager::Get();
	int32 Pruned = 0;

	// Enumerate immediate child directories of the skills root.
	TArray<FString> ChildDirs;
	FileManager.FindFiles(ChildDirs, *(SkillsRootAbsolute / TEXT("*")), /*Files*/ false, /*Directories*/ true);

	for (const FString& ChildDir : ChildDirs)
	{
		// Guard 1: only ever consider a folder we would have produced ourselves — it must own a SKILL.md.
		const FString SkillFile = FPaths::Combine(SkillsRootAbsolute, ChildDir, TEXT("SKILL.md"));
		if (!FileManager.FileExists(*SkillFile))
			continue;

		// Guard 2: keep folders that still correspond to a current tool (the folder name is already the
		// sanitized form the writer used, so compare directly).
		if (CurrentFolderNames.Contains(ChildDir))
			continue;

		const FString FolderPath = FPaths::Combine(SkillsRootAbsolute, ChildDir);
		if (FileManager.DeleteDirectory(*FolderPath, /*RequireExists*/ false, /*Tree*/ true))
			++Pruned;
		else
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] failed to prune stale skill folder: %s"), *FolderPath);
	}

	return Pruned;
}

FUnrealMcpSkillFileGenerator::FResult FUnrealMcpSkillFileGenerator::Generate(const FUnrealMcpToolRegistry& Registry, const FString& SkillsRootAbsolute)
{
	FResult Result;
	Result.SkillsRootAbsolute = SkillsRootAbsolute;

	if (SkillsRootAbsolute.IsEmpty())
	{
		Result.Error = TEXT("Skills root path is empty.");
		return Result;
	}

	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.MakeDirectory(*SkillsRootAbsolute, /*Tree*/ true))
	{
		// MakeDirectory returns false when it already exists OR on a genuine failure; treat a non-existent dir as failure.
		if (!FileManager.DirectoryExists(*SkillsRootAbsolute))
		{
			Result.Error = FString::Printf(TEXT("Could not create skills directory: %s"), *SkillsRootAbsolute);
			return Result;
		}
	}

	const TArray<FString> ToolNames = Registry.GetToolNamesSorted();
	TSet<FString> CurrentFolderNames;
	CurrentFolderNames.Reserve(ToolNames.Num());
	int32 Written = 0;
	for (const FString& ToolName : ToolNames)
	{
		const FUnrealMcpRegisteredTool* Tool = Registry.Find(ToolName);
		if (Tool == nullptr)
			continue;

		const FString FolderName = SanitizeSkillFolderName(Tool->Name);
		CurrentFolderNames.Add(FolderName);
		const FString FilePath = FPaths::Combine(SkillsRootAbsolute, FolderName, TEXT("SKILL.md"));
		const FString Markdown = BuildSkillMarkdown(*Tool);

		// Unconditional overwrite (idempotent regeneration). UTF-8 without BOM keeps the docs portable.
		if (FFileHelper::SaveStringToFile(Markdown, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			++Written;
		else
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] failed to write skill file: %s"), *FilePath);
	}

	// Prune stale generator-owned folders: a tool removed/renamed since the last run leaves an orphaned
	// `<old-name>/SKILL.md` whose docs no longer describe a real tool. Only delete an immediate child dir
	// that BOTH contains a generator-owned SKILL.md AND is not in the current set — never unrelated user content.
	Result.FilesPruned = PruneStaleSkillFolders(SkillsRootAbsolute, CurrentFolderNames);

	// Every write failing (with tools to write) is a failure, not a "succeeded (0 files)" — otherwise the panel
	// reports success for a run that produced nothing.
	if (Written == 0 && ToolNames.Num() > 0)
	{
		Result.Error = FString::Printf(TEXT("Failed to write any of %d skill file(s) under %s."), ToolNames.Num(), *SkillsRootAbsolute);
		return Result;
	}

	Result.bSuccess = true;
	Result.FilesWritten = Written;
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] generated %d skill file(s) (pruned %d stale) under %s."), Written, Result.FilesPruned, *SkillsRootAbsolute);
	return Result;
}

void FUnrealMcpSkillFileGenerator::PopulateCoreRegistry(FUnrealMcpToolRegistry& Registry)
{
	// Register every core family — declaration only (no world / bridge), so this is headless-safe (see the runtime's
	// Startup ordering: families register before the bridge accepts). Keep in lockstep with FUnrealMcpRuntime::Startup.
	UnrealMcpPingTool::Register(Registry);
	UnrealMcpActorTools::Register(Registry);
	UnrealMcpBlueprintTools::Register(Registry);
	UnrealMcpAssetTools::Register(Registry);
	UnrealMcpEditorTools::Register(Registry);
	UnrealMcpLevelTools::Register(Registry);
	UnrealMcpScreenshotTools::Register(Registry);
	UnrealMcpSourceTools::Register(Registry);
}
