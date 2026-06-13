// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfig.h"

/**
 * JSON read-merge-write implementation of FAiAgentConfig (docs/ARCHITECTURE.md §8), the C++/UE-Json analog of
 * Unity's JsonAiAgentConfig and Godot's AgentConfigJson. Handles every Phase-A agent (Claude Code's `.mcp.json`
 * and Cursor's `.cursor/mcp.json` both nest under "mcpServers").
 *
 * Desired-entry model: properties are accumulated via SetProperty (a value, whether it is required for the entry
 * to be considered "configured", and how it is compared) and SetPropertyToRemove (a key that must NOT be present
 * on the entry — used to drop the OTHER transport's keys, e.g. an HTTP config removes "command"/"args"). On
 * Configure(), the file is parsed, the body path is navigated/created, deprecated + duplicate sibling entries are
 * pruned, the server entry under DefaultMcpServerName is created-or-merged, the to-remove keys are deleted, and
 * every desired property is set (deterministic key order). Sibling servers + unrelated top-level keys survive.
 *
 * Robustness (the Unity/Godot lessons, spec-covered): a missing/empty/whitespace/invalid/non-object file is
 * treated as "no config" and replaced with the clean expected content rather than throwing; parent directories
 * are created. Identity keys (default {"command","url"}) let RemoveDuplicateServerEntries collapse the same
 * server written under a different name.
 *
 * All symbols are UNREALMCPEDITOR_API-exported so the UnrealMcpEditorTests Automation specs drive them directly
 * against temp files with no live editor / agent.
 */
class FJsonAiAgentConfig : public FAiAgentConfig
{
public:
	UNREALMCPEDITOR_API FJsonAiAgentConfig(const FString& InName, const FString& InConfigPath, const FString& InBodyPath = TEXT("mcpServers"));

	/** Add/replace a desired property on the server entry. @p bRequired gates IsConfigured(); @p Comparison gates match. */
	UNREALMCPEDITOR_API FJsonAiAgentConfig& SetProperty(const FString& Key, const TSharedPtr<FJsonValue>& Value, bool bRequired = false, EUnrealMcpValueComparison Comparison = EUnrealMcpValueComparison::Exact);
	/** Convenience string-value overload for SetProperty. */
	UNREALMCPEDITOR_API FJsonAiAgentConfig& SetStringProperty(const FString& Key, const FString& Value, bool bRequired = false, EUnrealMcpValueComparison Comparison = EUnrealMcpValueComparison::Exact);
	/** Mark a key that must be ABSENT from the server entry (drops the other transport's keys / stale headers). */
	UNREALMCPEDITOR_API FJsonAiAgentConfig& SetPropertyToRemove(const FString& Key);
	/** Add an identity key used to detect the same server written under a different sibling name (default command/url). */
	UNREALMCPEDITOR_API FJsonAiAgentConfig& AddIdentityKey(const FString& Key);

	virtual FString GetExpectedFileContent() const override;
	virtual bool Configure() override;
	virtual bool Remove() override;
	virtual bool IsDetected() const override;
	virtual bool IsConfigured() const override;

private:
	struct FDesiredProperty
	{
		TSharedPtr<FJsonValue> Value;
		bool bRequired = false;
		EUnrealMcpValueComparison Comparison = EUnrealMcpValueComparison::Exact;
	};

	// Insertion order is preserved for deterministic output; the map keys are the property names.
	TArray<FString> PropertyOrder;
	TMap<FString, FDesiredProperty> Properties;
	TArray<FString> PropertiesToRemove;
	TArray<FString> IdentityKeys;

	// Build the server entry object from the desired properties (deterministic, alphabetical key order).
	TSharedPtr<FJsonObject> BuildServerEntry() const;
	// Split a body path like "a/b" into segments; an empty/whitespace body path yields no segments (root).
	static TArray<FString> BodyPathSegments(const FString& InBodyPath);

	// Parse the file at ConfigPath into a root object; returns false (Out left null) for missing/empty/invalid.
	bool TryReadRoot(TSharedPtr<FJsonObject>& OutRoot) const;
	// Serialize @p Root to ConfigPath (creating parent dirs). Returns false on a genuine write failure.
	bool WriteRoot(const TSharedPtr<FJsonObject>& Root) const;

	// Navigate to the body-path object (read-only; null if any segment is missing/non-object).
	static TSharedPtr<FJsonObject> NavigateToBody(const TSharedPtr<FJsonObject>& Root, const TArray<FString>& Segments);
	// Navigate to the body-path object, CREATING intermediate objects as needed (for Configure()).
	static TSharedPtr<FJsonObject> EnsureBody(const TSharedPtr<FJsonObject>& Root, const TArray<FString>& Segments);

	// Sibling keys (≠ DefaultMcpServerName) whose entry matches one of our identity-key values (same server, alias).
	TArray<FString> FindDuplicateServerEntryKeys(const TSharedPtr<FJsonObject>& Body) const;

	bool AreRequiredPropertiesMatching(const TSharedPtr<FJsonObject>& Entry) const;
	bool HasAnyPropertyToRemove(const TSharedPtr<FJsonObject>& Entry) const;

	static bool AreValuesEquivalent(EUnrealMcpValueComparison Comparison, const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual);
	static FString NormalizeUrl(const FString& Url);
	static FString NormalizePath(const FString& Path);
	static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value);
};
