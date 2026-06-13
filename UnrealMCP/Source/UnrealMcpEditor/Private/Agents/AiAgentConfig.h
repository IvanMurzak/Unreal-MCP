// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * How two JSON scalar values are compared when deciding whether a config entry already matches the desired
 * state (Unity's ValueComparisonMode). Exact = byte-equal JSON; Url = scheme/host case-insensitive + trailing
 * slash trimmed (so "http://localhost:8080/mcp" matches "http://localhost:8080/mcp/").
 */
enum class EUnrealMcpValueComparison : uint8
{
	Exact,
	Url,
	/** Path-equivalence: separators unified to '/', trailing slash trimmed (Codex `command` comparison). */
	Path
};

/**
 * Abstract base for an AI-agent MCP-client config file writer (docs/ARCHITECTURE.md §8; the C++/Unreal analog
 * of Unity's AiAgentConfig and Godot's AgentConfigJson). One instance describes the desired server entry for a
 * single transport (STDIO or HTTP) of a single agent, plus the on-disk file it lives in and the body path the
 * server map nests under (e.g. "mcpServers"). Subclasses implement the format-specific read-merge-write
 * (FJsonAiAgentConfig for JSON; a TOML subclass would slot in here if a Phase-A agent ever needs it).
 *
 * The read-merge-write contract every subclass MUST honour (the robustness lessons carried from Unity + Godot):
 *  - Configure() preserves SIBLING server entries and unrelated top-level keys in the file; it only touches
 *    THIS server's identity entry under the body path.
 *  - It tolerates a missing / empty / invalid file by writing a fresh expected-content file (never throws).
 *  - It creates parent directories as needed.
 *  - It removes duplicate sibling entries that represent the SAME server under a different name (identity-key
 *    match) and migrates away deprecated server names.
 *
 * Token discipline (§8): a config object holds the REAL bearer token (it must — it writes it to the file). It is
 * NEVER logged. The on-screen masking lives one layer up in the Slate panel / view-model; this class is the
 * file-writer and deals in real values only.
 */
class FAiAgentConfig
{
public:
	/** The canonical server identity key written under the body path (shared with the cli's SERVER_KEY). */
	static UNREALMCPEDITOR_API const TCHAR* DefaultMcpServerName;
	/** The default body path a JSON agent nests its server map under. */
	static UNREALMCPEDITOR_API const TCHAR* DefaultBodyPath;
	/** Server names from earlier plugin generations that Configure()/Remove() clean up on sight. */
	static UNREALMCPEDITOR_API const TArray<FString>& DeprecatedMcpServerNames();

	FAiAgentConfig(const FString& InName, const FString& InConfigPath, const FString& InBodyPath = TEXT("mcpServers"))
		: Name(InName)
		, ConfigPath(InConfigPath)
		, BodyPath(InBodyPath)
	{
	}

	virtual ~FAiAgentConfig() = default;

	/** Display label for the agent (diagnostics / UI). */
	const FString& GetName() const { return Name; }
	/** Absolute path to the config file this entry is written into (empty = snippet-only, e.g. Custom). */
	const FString& GetConfigPath() const { return ConfigPath; }
	/** The body path the server map nests under (e.g. "mcpServers"; "servers" for VS Code). */
	const FString& GetBodyPath() const { return BodyPath; }

	/** The full file content the entry would produce when written into an otherwise-empty file (UI preview). */
	virtual FString GetExpectedFileContent() const = 0;

	// Auth injection is baked at build time by each configurator (FJsonAiAgentConfig writes the token into `args` /
	// the Authorization header; Codex's FTomlAiAgentConfig writes the `bearer_token_env_var` env-var-NAME indirection
	// directly in BuildStdio/BuildHttp). There is therefore no post-hoc Apply*Authorization seam — token discipline
	// lives entirely in the configurators, not here.

	/** Write/merge this server entry into the file, preserving siblings + unrelated keys. Returns IsConfigured(). */
	virtual bool Configure() = 0;
	/** Remove this server entry (and any deprecated/duplicate aliases) from the file. Returns true if it changed. */
	virtual bool Remove() = 0;
	/** Whether an entry for this server (or a deprecated/duplicate alias) is present in the file at all. */
	virtual bool IsDetected() const = 0;
	/** Whether the present entry matches every required property of the desired config (no reconfigure needed). */
	virtual bool IsConfigured() const = 0;

protected:
	FString Name;
	FString ConfigPath;
	FString BodyPath;
};
