// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfig.h"

/**
 * TOML read-merge-write implementation of FAiAgentConfig (docs/ARCHITECTURE.md §8), the C++/UE analog of Unity's
 * TomlAiAgentConfig. Codex is the Phase-B agent that uses TOML (`.codex/config.toml`), nesting its server entry
 * under a dotted-table header `[mcp_servers.unreal-mcp]`. This is the second concrete subclass of the FAiAgentConfig
 * seam Phase A left for exactly this purpose (FJsonAiAgentConfig is the first).
 *
 * Desired-entry model mirrors FJsonAiAgentConfig: typed properties (string / string-array / int / bool) accumulated
 * via SetProperty, with a `bRequired` gate (drives IsConfigured) and a comparison mode (Exact / Url / Path). Keys
 * marked via SetPropertyToRemove must NOT be present on the entry (drops the other transport's keys). On Configure()
 * the file is parsed line-wise, the dotted-table section is found/created, deprecated + duplicate-identity sibling
 * sections are pruned, the section's properties are merged (existing scalar lines re-emitted with the desired values
 * overriding, to-remove keys dropped), and the file is rewritten. Sibling sections + comments outside the section
 * survive (the merge is section-scoped). Robust to a missing file (writes clean expected content) — an empty/invalid
 * file degrades to "no section found" → append, never throws.
 *
 * Deliberately a focused TOML subset: dotted-table section headers, scalar `key = value` lines (string / int / bool),
 * and string arrays `["a","b"]`. That covers every shape the Codex config emits; richer TOML (inline tables, multi-
 * line arrays, datetimes) is out of scope and round-trips as opaque raw lines when encountered in an existing file.
 *
 * All symbols are UNREALMCPEDITOR_API-exported so the UnrealMcpEditorTests Automation specs drive them against temp
 * files with no live editor / agent.
 */
class FTomlAiAgentConfig : public FAiAgentConfig
{
public:
	UNREALMCPEDITOR_API FTomlAiAgentConfig(const FString& InName, const FString& InConfigPath, const FString& InBodyPath = TEXT("mcp_servers"));

	/** A typed desired value: exactly one of the variants is populated, selected by Kind. */
	enum class EValueKind : uint8 { String, StringArray, Int, Bool };

	/** Add/replace a desired string property. @p bRequired gates IsConfigured(); @p Comparison gates the match. */
	UNREALMCPEDITOR_API FTomlAiAgentConfig& SetStringProperty(const FString& Key, const FString& Value, bool bRequired = false, EUnrealMcpValueComparison Comparison = EUnrealMcpValueComparison::Exact);
	/** Add/replace a desired string-array property (e.g. Codex `args`). */
	UNREALMCPEDITOR_API FTomlAiAgentConfig& SetStringArrayProperty(const FString& Key, const TArray<FString>& Values, bool bRequired = false);
	/** Add/replace a desired integer property (e.g. Codex `tool_timeout_sec`). */
	UNREALMCPEDITOR_API FTomlAiAgentConfig& SetIntProperty(const FString& Key, int32 Value, bool bRequired = false);
	/** Add/replace a desired boolean property (e.g. Codex `enabled`). */
	UNREALMCPEDITOR_API FTomlAiAgentConfig& SetBoolProperty(const FString& Key, bool Value, bool bRequired = false);
	/** Mark a key that must be ABSENT from the section (drops the other transport's keys). */
	UNREALMCPEDITOR_API FTomlAiAgentConfig& SetPropertyToRemove(const FString& Key);
	/** Add an identity key used to detect the same server written under a different section name (default command/url). */
	UNREALMCPEDITOR_API FTomlAiAgentConfig& AddIdentityKey(const FString& Key);

	virtual FString GetExpectedFileContent() const override;
	virtual bool Configure() override;
	virtual bool Remove() override;
	virtual bool IsDetected() const override;
	virtual bool IsConfigured() const override;

private:
	struct FDesiredValue
	{
		EValueKind Kind = EValueKind::String;
		FString StringValue;
		TArray<FString> ArrayValue;
		int32 IntValue = 0;
		bool BoolValue = false;
		bool bRequired = false;
		EUnrealMcpValueComparison Comparison = EUnrealMcpValueComparison::Exact;
	};

	TArray<FString> PropertyOrder;             // insertion order (output is sorted alphabetically, like Unity)
	TMap<FString, FDesiredValue> Properties;
	TArray<FString> PropertiesToRemove;
	TArray<FString> IdentityKeys;

	FString SectionName() const;               // "<BodyPath>.<DefaultMcpServerName>"
	static FString FormatProperty(const FString& Key, const FDesiredValue& Value);
	static FString FormatRawProperty(const FString& Key, const FString& RawValue); // a value already in TOML literal form
	FString BuildSection(const FString& InSectionName, const TMap<FString, FString>& Props) const; // Props = key→raw TOML literal
	TMap<FString, FString> DesiredRawProperties() const; // desired props as key→raw TOML literal (deterministic order applied at emit)

	// --- File IO ---
	static bool TryReadLines(const FString& Path, TArray<FString>& OutLines);  // false for missing/unreadable
	bool WriteLines(const TArray<FString>& Lines) const;                       // creates parent dirs

	// --- Line-wise section navigation ---
	static int32 FindSection(const TArray<FString>& Lines, const FString& InSectionName); // index of `[<name>]` line, or INDEX_NONE
	static int32 FindSectionEnd(const TArray<FString>& Lines, int32 SectionStart);        // first line of the NEXT section, or Lines.Num()
	static TMap<FString, FString> ParseSectionProps(const TArray<FString>& Lines, int32 Start, int32 End); // key→raw literal
	TArray<TPair<int32, int32>> FindDuplicateSectionRanges(const TArray<FString>& Lines, const FString& OwnSection) const;

	// --- Matching ---
	bool AreRequiredPropertiesMatching(const TMap<FString, FString>& Existing) const;
	bool HasAnyPropertyToRemove(const TMap<FString, FString>& Existing) const;
	static bool RawMatchesDesired(const FDesiredValue& Desired, const FString& RawExisting);
	static FString StripQuotes(const FString& Raw);
	static FString NormalizeUrl(const FString& Url);
	static FString NormalizePath(const FString& Path);
};
