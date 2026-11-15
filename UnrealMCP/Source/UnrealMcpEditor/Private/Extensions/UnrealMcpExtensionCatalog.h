// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * One MCP tool an extension contributes — the native mirror of the CLI catalog's `tools[]` entry
 * (cli/src/utils/extensions-catalog.ts `ExtensionTool`).
 */
struct FUnrealMcpCatalogTool
{
	FString Name;
	FString Description;
};

/**
 * One installable extension — the native C++ mirror of the SHARED catalog descriptor
 * (`ExtensionDescriptor` in cli/src/utils/extensions-catalog.ts, source-of-truth JSON
 * `cli/extensions.catalog.json` `{ schemaVersion, extensions[] }`). The in-editor Extensions panel
 * (docs/ARCHITECTURE.md §7 item 10, install channel #3) reuses the SAME catalog format + install
 * contract as the CLI (#172) and the Niagara catalog entry (#174) so all three install channels are
 * behaviorally identical — this struct is the C++ value-equivalent of the TS descriptor, parsed from
 * the same JSON shape rather than a second hand-maintained copy.
 */
struct FUnrealMcpCatalogEntry
{
	/** Stable reverse-DNS id (the provider's `GetExtensionId()`). Primary resolve key. */
	FString ExtensionId;
	/** Human-readable display name shown in the Extensions panel. */
	FString Name;
	FString Description;
	/** The UE plugin folder + descriptor basename: installs into `<project>/Plugins/<PluginName>/`. */
	FString PluginName;
	/** `owner/repo` the extension zip is released from on GitHub Releases; empty for a source-only entry. */
	FString Repo;
	/** Catalog-pinned version, or empty for an unpinned entry. */
	FString Version;
	/** Component tag and optional archive digest from the online index. Empty for legacy catalogs. */
	FString ReleaseTag;
	FString ArchiveSha256;
	/** Minimum core Unreal-MCP plugin version required (`min-core-version`); empty = no floor. */
	FString MinCoreVersion;
	/** Gating engine plugins (e.g. `Niagara`) enabled in the `.uproject` alongside the extension. */
	TArray<FString> EnginePlugins;
	/** The MCP tools this extension contributes (advisory; for the panel's detail view). */
	TArray<FUnrealMcpCatalogTool> Tools;
	/**
	 * The oldest Unreal Engine the selected release builds on (`"5.8"`, extension.json `minEngineVersion`); empty = any
	 * (an index entry published before the field existed).
	 */
	FString MinEngineVersion;
	/** The selected release is experimental (extension.json `experimental`). */
	bool bExperimental = false;

	/** True when the descriptor carries a concrete version pin (drives the up-to-date / update decision). */
	bool HasVersion() const { return !Version.TrimStartAndEnd().IsEmpty(); }

	/**
	 * Why an engine @p EngineMajor.@p EngineMinor cannot build this extension ("requires Unreal Engine 5.8 or newer; this
	 * editor is 5.7"), or empty when it can. Pure.
	 */
	UNREALMCPEDITOR_API FString EngineIncompatibility(int32 EngineMajor, int32 EngineMinor) const;
	/** EngineIncompatibility for the running editor's engine. */
	UNREALMCPEDITOR_API FString EngineIncompatibility() const;
};

/**
 * Parse + lookup helpers for the shared extension catalog JSON, the native analog of the CLI's
 * `extensions-catalog.ts` (`findExtension` / `hasVersion`). Pure, no engine-process / no network — the
 * panel fetches the JSON text over HTTP (or reads a local copy) and hands it here. Keeping the parse +
 * lookup as pure transforms makes every decision unit-testable in an Automation spec with no live
 * download (mirrors the FUnrealMcpServerChecksum seam).
 */
class FUnrealMcpExtensionCatalog
{
public:
	/** The catalog schema version this parser understands (mirrors EXTENSION_CATALOG_SCHEMA_VERSION = 1). */
	static UNREALMCPEDITOR_API const int32 SupportedSchemaVersion;

	/**
	 * The same published online index the CLI uses. LegacyCatalogUrl remains the fallback during
	 * migration or when the live index cannot be fetched. Component coordinates are parsed below.
	 */
	static UNREALMCPEDITOR_API FString DefaultCatalogUrl();
	static UNREALMCPEDITOR_API FString LegacyCatalogUrl();

	/**
	 * Parse the shared catalog JSON `{ schemaVersion, extensions[] }` into typed entries. Returns false +
	 * a human-readable reason on malformed JSON, a missing/!array `extensions`, or an entry missing the
	 * required `extensionId` / `pluginName`, or a malformed `minEngineVersion` / `experimental` (as strict as the CLI's
	 * parseExtensionsIndex, so both channels accept the same index). A schemaVersion newer than @ref SupportedSchemaVersion is a
	 * WARNING returned via @p OutWarning (still parsed best-effort), not a hard failure — a newer catalog
	 * should degrade gracefully rather than block installs of the entries this client understands.
	 */
	static UNREALMCPEDITOR_API bool ParseCatalogJson(
		const FString& Json, TArray<FUnrealMcpCatalogEntry>& OutEntries, FString& OutError, FString& OutWarning);

	/**
	 * Resolve a user-supplied id to a catalog entry, case-insensitively, by `extensionId` first (the
	 * install identity), then `name`, then `pluginName` — exactly the CLI's `findExtension` precedence.
	 * Returns nullptr when absent or @p Id is empty.
	 */
	static UNREALMCPEDITOR_API const FUnrealMcpCatalogEntry* FindEntry(
		const TArray<FUnrealMcpCatalogEntry>& Entries, const FString& Id);

	/**
	 * Hand a fetched catalog to the list agents read of the extensions this editor does not have yet
	 * (UnrealMcpAvailableExtensions: unreal-tool-list, rql-schema); the bundled first-party entries stay as bundled.
	 * An entry this engine cannot build (EngineIncompatibility) is left out: agents are never told to install it.
	 */
	static UNREALMCPEDITOR_API void PublishToAgents(const TArray<FUnrealMcpCatalogEntry>& Entries);

	/**
	 * The gates a local install source declares in its `extension.json` (@p ExtensionJson; null = no such file) into
	 * @p OutEntry, replacing the catalog's: `minEngineVersion` (absent = none) and `experimental`. The CLI's
	 * `install-extension --source` engine gate (readSourceGates; it also reads the .uplugin's IsExperimentalVersion, which
	 * only feeds its warning). False on a malformed value.
	 */
	static UNREALMCPEDITOR_API bool ReadSourceGates(const TSharedPtr<FJsonObject>& ExtensionJson, FUnrealMcpCatalogEntry& OutEntry);

private:
	static bool ParseEntry(const TSharedPtr<FJsonObject>& Obj, FUnrealMcpCatalogEntry& OutEntry, FString& OutError);
};
