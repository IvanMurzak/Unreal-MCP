// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Extensions/UnrealMcpExtensionCatalog.h"

#include "UnrealMcpAvailableExtensions.h"

#include "Dom/JsonObject.h"
#include "Internationalization/Regex.h"
#include "Dom/JsonValue.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

const int32 FUnrealMcpExtensionCatalog::SupportedSchemaVersion = 1;

namespace
{
	/** `"5.8"` (extension.json minEngineVersion: major.minor, no leading zero) into its numbers. */
	bool ExtCatParseMinEngineVersion(const FString& Value, int32& OutMajor, int32& OutMinor)
	{
		const FRegexPattern Pattern(TEXT("^([1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
		FRegexMatcher Matcher(Pattern, Value);
		if (!Matcher.FindNext())
			return false;
		OutMajor = FCString::Atoi(*Matcher.GetCaptureGroup(1));
		OutMinor = FCString::Atoi(*Matcher.GetCaptureGroup(2));
		return true;
	}

	/**
	 * Read the optional gates of an index entry or release record into @p OutEntry: `minEngineVersion` (a string, or
	 * null for none) and `experimental` (a boolean). A field that is absent leaves @p OutEntry as it is. False on a
	 * malformed value.
	 */
	bool ExtCatReadGates(const FJsonObject& Obj, FUnrealMcpCatalogEntry& OutEntry)
	{
		if (const TSharedPtr<FJsonValue> MinEngine = Obj.TryGetField(TEXT("minEngineVersion")))
		{
			int32 Major = 0, Minor = 0;
			if (MinEngine->IsNull())
				OutEntry.MinEngineVersion.Reset();
			else if (MinEngine->Type != EJson::String || !ExtCatParseMinEngineVersion(MinEngine->AsString(), Major, Minor))
				return false;
			else
				OutEntry.MinEngineVersion = MinEngine->AsString();
		}
		if (const TSharedPtr<FJsonValue> Experimental = Obj.TryGetField(TEXT("experimental")))
		{
			if (Experimental->Type != EJson::Boolean)
				return false;
			OutEntry.bExperimental = Experimental->AsBool();
		}
		return true;
	}
}

FString FUnrealMcpCatalogEntry::EngineIncompatibility(int32 EngineMajor, int32 EngineMinor) const
{
	int32 Major = 0, Minor = 0;
	if (!ExtCatParseMinEngineVersion(MinEngineVersion, Major, Minor)
		|| EngineMajor > Major || (EngineMajor == Major && EngineMinor >= Minor))
		return FString();
	return FString::Printf(TEXT("requires Unreal Engine %s or newer; this editor is %d.%d"), *MinEngineVersion, EngineMajor, EngineMinor);
}

FString FUnrealMcpCatalogEntry::EngineIncompatibility() const
{
	return EngineIncompatibility(FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor());
}

FString FUnrealMcpExtensionCatalog::DefaultCatalogUrl()
{
	return TEXT("https://github.com/IvanMurzak/Unreal-MCP/releases/download/extensions-index/extensions-index.json");
}

FString FUnrealMcpExtensionCatalog::LegacyCatalogUrl()
{
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
	if (!ExtCatReadGates(*Obj, OutEntry))
	{
		OutError = FString::Printf(TEXT("catalog entry '%s' has a malformed minEngineVersion or experimental field"), *OutEntry.ExtensionId);
		return false;
	}
	const TSharedPtr<FJsonObject>* Releases = nullptr;
	if (Obj->TryGetObjectField(TEXT("releases"), Releases) && Releases && Releases->IsValid())
	{
		const TSharedPtr<FJsonObject>* Latest = nullptr;
		if (!(*Releases)->TryGetObjectField(OutEntry.Version, Latest) || !Latest || !Latest->IsValid()
			|| !(*Latest)->TryGetStringField(TEXT("repo"), OutEntry.Repo)
			|| !(*Latest)->TryGetStringField(TEXT("tag"), OutEntry.ReleaseTag))
		{
			OutError = TEXT("the extension index is missing the selected release");
			return false;
		}
		const FRegexPattern TagPattern(TEXT("^[A-Za-z0-9.-]+$"));
		FRegexMatcher TagMatcher(TagPattern, OutEntry.ReleaseTag);
		if (!TagMatcher.FindNext() || !(OutEntry.ReleaseTag == TEXT("v") + OutEntry.Version
			|| OutEntry.ReleaseTag.EndsWith(TEXT("-v") + OutEntry.Version)))
		{
			OutError = TEXT("invalid extension release tag");
			return false;
		}
		// The selected release's own gates win; a record without them (published before they existed) keeps the entry's,
		// which describe this same latest version.
		if (!ExtCatReadGates(**Latest, OutEntry))
		{
			OutError = TEXT("the selected extension release has a malformed minEngineVersion or experimental field");
			return false;
		}
		(*Latest)->TryGetStringField(TEXT("sha256"), OutEntry.ArchiveSha256);
		const FRegexPattern HashPattern(TEXT("^[a-f0-9]{64}$"));
		FRegexMatcher HashMatcher(HashPattern, OutEntry.ArchiveSha256);
		if (!OutEntry.ArchiveSha256.IsEmpty() && !HashMatcher.FindNext())
		{
			OutError = TEXT("invalid extension archive checksum");
			return false;
		}
	}
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

void FUnrealMcpExtensionCatalog::PublishToAgents(const TArray<FUnrealMcpCatalogEntry>& Entries)
{
	TArray<FUnrealMcpKnownExtension> Known;
	for (const FUnrealMcpCatalogEntry& Entry : Entries)
	{
		if (!Entry.EngineIncompatibility().IsEmpty())
			continue; // this engine cannot build it: never suggest installing it
		FUnrealMcpKnownExtension& Extension = Known.AddDefaulted_GetRef();
		Extension.ExtensionId = Entry.ExtensionId;
		Extension.Name = Entry.Name;
		Extension.PluginName = Entry.PluginName;
		Extension.Adds = Entry.Description;
	}
	UnrealMcpAvailableExtensions::SetCatalog(MoveTemp(Known));
}
