// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Extensions/UnrealMcpExtensionCatalog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

const int32 FUnrealMcpExtensionCatalog::SupportedSchemaVersion = 1;

FString FUnrealMcpExtensionCatalog::DefaultCatalogUrl()
{
	// Raw single-source-of-truth JSON on the repo's default branch. The CLI mirrors this JSON as a typed
	// constant kept in lockstep by a parity test (cli/tests/extensions-catalog-parity.test.ts); the panel
	// fetches the JSON directly so it never carries a second drifting copy.
	return TEXT("https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/cli/extensions.catalog.json");
}

bool FUnrealMcpExtensionCatalog::ParseEntry(
	const TSharedPtr<FJsonObject>& Obj, FUnrealMcpCatalogEntry& OutEntry, FString& OutError)
{
	if (!Obj.IsValid())
	{
		OutError = TEXT("a catalog entry was not a JSON object");
		return false;
	}

	// extensionId + pluginName are the required identity fields — an entry missing either cannot be
	// resolved or placed, so reject the whole parse rather than silently dropping it.
	if (!Obj->TryGetStringField(TEXT("extensionId"), OutEntry.ExtensionId) || OutEntry.ExtensionId.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("a catalog entry is missing the required 'extensionId' field");
		return false;
	}
	if (!Obj->TryGetStringField(TEXT("pluginName"), OutEntry.PluginName) || OutEntry.PluginName.TrimStartAndEnd().IsEmpty())
	{
		OutError = FString::Printf(TEXT("catalog entry '%s' is missing the required 'pluginName' field"), *OutEntry.ExtensionId);
		return false;
	}

	// Optional descriptive fields — absent is fine (mirrors the descriptor's nullable members).
	Obj->TryGetStringField(TEXT("name"), OutEntry.Name);
	Obj->TryGetStringField(TEXT("description"), OutEntry.Description);
	Obj->TryGetStringField(TEXT("repo"), OutEntry.Repo);
	Obj->TryGetStringField(TEXT("version"), OutEntry.Version);
	Obj->TryGetStringField(TEXT("minCoreVersion"), OutEntry.MinCoreVersion);
	if (OutEntry.Name.IsEmpty())
		OutEntry.Name = OutEntry.PluginName;

	const TArray<TSharedPtr<FJsonValue>>* EnginePlugins = nullptr;
	if (Obj->TryGetArrayField(TEXT("enginePlugins"), EnginePlugins) && EnginePlugins)
	{
		for (const TSharedPtr<FJsonValue>& Value : *EnginePlugins)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name) && !Name.TrimStartAndEnd().IsEmpty())
				OutEntry.EnginePlugins.Add(Name.TrimStartAndEnd());
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	if (Obj->TryGetArrayField(TEXT("tools"), Tools) && Tools)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Tools)
		{
			const TSharedPtr<FJsonObject>* ToolObj = nullptr;
			if (Value.IsValid() && Value->TryGetObject(ToolObj) && ToolObj && (*ToolObj).IsValid())
			{
				FUnrealMcpCatalogTool Tool;
				(*ToolObj)->TryGetStringField(TEXT("name"), Tool.Name);
				(*ToolObj)->TryGetStringField(TEXT("description"), Tool.Description);
				if (!Tool.Name.IsEmpty())
					OutEntry.Tools.Add(MoveTemp(Tool));
			}
		}
	}

	return true;
}

bool FUnrealMcpExtensionCatalog::ParseCatalogJson(
	const FString& Json, TArray<FUnrealMcpCatalogEntry>& OutEntries, FString& OutError, FString& OutWarning)
{
	OutEntries.Reset();
	OutError.Reset();
	OutWarning.Reset();

	if (Json.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("the catalog JSON was empty");
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("the catalog JSON is malformed");
		return false;
	}

	// schemaVersion is advisory: a NEWER catalog parses best-effort (warn, don't fail) so this client keeps
	// installing the entries it understands; an absent field is treated as the supported version.
	int32 SchemaVersion = SupportedSchemaVersion;
	Root->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion);
	if (SchemaVersion > SupportedSchemaVersion)
	{
		OutWarning = FString::Printf(
			TEXT("catalog schemaVersion %d is newer than the supported version %d; parsing best-effort — update the plugin for full support."),
			SchemaVersion, SupportedSchemaVersion);
	}

	const TArray<TSharedPtr<FJsonValue>>* Extensions = nullptr;
	if (!Root->TryGetArrayField(TEXT("extensions"), Extensions) || Extensions == nullptr)
	{
		OutError = TEXT("the catalog JSON has no 'extensions' array");
		return false;
	}

	OutEntries.Reserve(Extensions->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Extensions)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || Obj == nullptr || !(*Obj).IsValid())
		{
			OutError = TEXT("a catalog 'extensions' element was not a JSON object");
			OutEntries.Reset();
			return false;
		}
		FUnrealMcpCatalogEntry Entry;
		if (!ParseEntry(*Obj, Entry, OutError))
		{
			OutEntries.Reset();
			return false;
		}
		OutEntries.Add(MoveTemp(Entry));
	}

	return true;
}

const FUnrealMcpCatalogEntry* FUnrealMcpExtensionCatalog::FindEntry(
	const TArray<FUnrealMcpCatalogEntry>& Entries, const FString& Id)
{
	const FString Needle = Id.TrimStartAndEnd().ToLower();
	if (Needle.IsEmpty())
		return nullptr;

	// extensionId first (the install identity), then name, then pluginName — the CLI's findExtension order.
	if (const FUnrealMcpCatalogEntry* ById = Entries.FindByPredicate(
		[&Needle](const FUnrealMcpCatalogEntry& E) { return E.ExtensionId.ToLower() == Needle; }))
		return ById;
	if (const FUnrealMcpCatalogEntry* ByName = Entries.FindByPredicate(
		[&Needle](const FUnrealMcpCatalogEntry& E) { return E.Name.ToLower() == Needle; }))
		return ByName;
	if (const FUnrealMcpCatalogEntry* ByPlugin = Entries.FindByPredicate(
		[&Needle](const FUnrealMcpCatalogEntry& E) { return E.PluginName.ToLower() == Needle; }))
		return ByPlugin;
	return nullptr;
}
