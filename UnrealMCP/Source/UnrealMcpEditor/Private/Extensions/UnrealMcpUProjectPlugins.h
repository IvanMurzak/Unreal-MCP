// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Result of enabling plugins in a `.uproject`'s JSON text (native mirror of the CLI's EnablePluginsResult). */
struct FUnrealMcpEnablePluginsResult
{
	/** True when the `.uproject` text was modified. */
	bool bChanged = false;
	/** The resulting `.uproject` text (unchanged when bChanged == false). */
	FString Text;
	/** The names enabled in the project after the operation (full enabled set). */
	TArray<FString> EnabledPlugins;
	/** The subset this call actually switched on — appended, or flipped from Enabled:false to true. */
	TArray<FString> AddedOrFlipped;
};

/**
 * Pure helpers for the two UE descriptor files the in-editor install touches — the native C++ port of
 * the CLI's `cli/src/utils/uproject-plugins.ts`, so the in-editor install channel (#3) edits the
 * `.uproject` Plugins[] array with behavioral parity to the CLI (#172):
 *
 *  - a plugin's `.uplugin` — its declared `Plugins[]` dependencies (the gating engine plugins, e.g.
 *    Niagara) and its `VersionName`.
 *  - the project's `.uproject` — ENABLE the extension + its gating plugins in `Plugins[]`, idempotently.
 *
 * No I/O here (callers read/write the files); these are pure JSON transforms so they are exhaustively
 * unit-testable in an Automation spec without a UE project on disk.
 *
 * NOTE (deliberate deviation from the TS): UE's FJsonObject does not preserve source key order, so the
 * re-serialized `.uproject` may reorder top-level keys vs the input. This is harmless — UE reads
 * `.uproject` keys order-independently — and the enable semantics (append / flip / idempotent no-op)
 * match the CLI exactly. The output uses tab indentation + preserves a trailing newline, matching the
 * UE descriptor convention.
 */
class FUnrealMcpUProjectPlugins
{
public:
	/**
	 * Extract the names of the plugin dependencies a `.uplugin` JSON declares as enabled (`Plugins[]`
	 * entries whose `Enabled` is not false) — the gating plugins that must also be enabled in the
	 * consuming `.uproject`. Tolerant of a missing/!array `Plugins` field (→ empty). Pure.
	 */
	static UNREALMCPEDITOR_API TArray<FString> ParseUPluginDependencies(const TSharedPtr<FJsonObject>& UpluginJson);

	/** Read a `.uplugin` JSON's `VersionName`, or empty when absent / not a non-empty string. Pure. */
	static UNREALMCPEDITOR_API FString ReadUPluginVersionName(const TSharedPtr<FJsonObject>& UpluginJson);

	/**
	 * Enable each plugin in @p PluginNames in a `.uproject`'s JSON text:
	 *  - a plugin already present and enabled is left untouched (idempotent);
	 *  - a plugin present but `Enabled: false` is flipped to true;
	 *  - a plugin absent is appended as `{ "Name": <n>, "Enabled": true }`.
	 * Names are de-duplicated case-insensitively (UE plugin names are case-insensitive). Returns false +
	 * @p OutError on malformed JSON (the caller turns it into a structured failure). Pure transform.
	 */
	static UNREALMCPEDITOR_API bool EnablePluginsInUProject(
		const FString& UprojectText, const TArray<FString>& PluginNames,
		FUnrealMcpEnablePluginsResult& OutResult, FString& OutError);
};
