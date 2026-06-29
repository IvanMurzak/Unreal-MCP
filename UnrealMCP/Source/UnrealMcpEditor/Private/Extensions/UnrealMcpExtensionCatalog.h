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
	/** Minimum core Unreal-MCP plugin version required (`min-core-version`); empty = no floor. */
	FString MinCoreVersion;
	/** Gating engine plugins (e.g. `Niagara`) enabled in the `.uproject` alongside the extension. */
	TArray<FString> EnginePlugins;
	/** The MCP tools this extension contributes (advisory; for the panel's detail view). */
	TArray<FUnrealMcpCatalogTool> Tools;

	/** True when the descriptor carries a concrete version pin (drives the up-to-date / update decision). */
	bool HasVersion() const { return !Version.TrimStartAndEnd().IsEmpty(); }
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
	 * The default catalog source URL: the raw `cli/extensions.catalog.json` on the Unreal-MCP repo's
	 * default branch. This is the SINGLE source of truth shared with the CLI (a parity test keeps the
	 * TS mirror in lockstep with the JSON), so the panel never carries a second drifting copy.
	 */
	static UNREALMCPEDITOR_API FString DefaultCatalogUrl();

	/**
	 * Parse the shared catalog JSON `{ schemaVersion, extensions[] }` into typed entries. Returns false +
	 * a human-readable reason on malformed JSON, a missing/!array `extensions`, or an entry missing the
	 * required `extensionId` / `pluginName`. A schemaVersion newer than @ref SupportedSchemaVersion is a
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

private:
	static bool ParseEntry(const TSharedPtr<FJsonObject>& Obj, FUnrealMcpCatalogEntry& OutEntry, FString& OutError);
};
