// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

/**
 * An Unreal-MCP extension an agent can install: the six first-party ones are bundled (always known, offline too), the
 * online catalog the editor loads may add more (SetCatalog).
 */
struct FUnrealMcpKnownExtension
{
	/** Reverse-DNS id, the install identity: unreal-mcp-cli install-extension <id>. */
	FString ExtensionId;
	FString Name;
	/** The UE plugin it installs as (<project>/Plugins/<PluginName>). */
	FString PluginName;
	/** One line: what it adds (its RQL sources and operations). */
	FString Adds;
	/** Lower-case prefixes of the identifiers of its domain ("niagara" matches niagaraSystems, NiagaraComponent). */
	TArray<FString> Keywords;
	/** It ships an RQL module: an enabled plugin without its module means the module did not load yet. */
	bool bRqlModule = false;
};

/** Where an extension stands in this editor. */
enum class EUnrealMcpExtensionState : uint8
{
	Ready,          // its RQL module is loaded and enabled (or, for an extension without one, its plugin is enabled)
	TurnedOff,      // loaded, but turned off in the Unreal-MCP Extensions panel
	NotLoaded,      // its plugin is enabled, its RQL module is not loaded (installed this session, not built yet)
	PluginDisabled, // its plugin is in the project, disabled
	NotInstalled,
};

struct FUnrealMcpMissingExtension
{
	FUnrealMcpKnownExtension Extension;
	EUnrealMcpExtensionState State = EUnrealMcpExtensionState::NotInstalled;
};

/**
 * What agents are told about the extensions they do not have yet (unreal-tool-list, rql-schema, the unreal-rql skill), and
 * the hint an RQL error or empty result gets when the query touches the domain of one (niagaraSystems, PCGGraph, ...).
 * Never touches the network: the editor hands the online catalog over when it has one. Game thread.
 */
namespace UnrealMcpAvailableExtensions
{
	/** The bundled first-party extensions, then the catalog entries (SetCatalog) that are not among them. */
	UNREALMCPRQL_API TArray<FUnrealMcpKnownExtension> Known();

	/** Replace the extensions of the online catalog; the bundled first-party ones always stay as bundled. */
	UNREALMCPRQL_API void SetCatalog(TArray<FUnrealMcpKnownExtension> Entries);

	UNREALMCPRQL_API EUnrealMcpExtensionState State(const FUnrealMcpKnownExtension& Extension);

	/** The known extensions that are not Ready, in Known() order. */
	UNREALMCPRQL_API TArray<FUnrealMcpMissingExtension> Missing();

	/** "not installed", "plugin disabled", ... */
	UNREALMCPRQL_API FString StateLabel(EUnrealMcpExtensionState State);

	/** What makes @p Missing usable: install it, enable its plugin, restart the editor or turn it on. */
	UNREALMCPRQL_API FString HowToGet(const FUnrealMcpMissingExtension& Missing);

	/** One line for a list: "<id> (<name>): <adds> - <state>[: <what to do>]". */
	UNREALMCPRQL_API FString Line(const FUnrealMcpMissingExtension& Missing);

	/** The sentence above such a list: how to install one. */
	UNREALMCPRQL_API FString InstallNote();

	/** The extension of @p Candidates one of whose keywords prefixes an identifier of @p Text (case-insensitive); nullptr when none. */
	UNREALMCPRQL_API const FUnrealMcpMissingExtension* MatchDomain(const FString& Text, const TArray<FUnrealMcpMissingExtension>& Candidates);

	/** The hint naming the missing extension whose domain @p Text touches (MatchDomain over Missing()); empty when none. */
	UNREALMCPRQL_API FString DomainHint(const FString& Text);

	/** DomainHint's sentence for @p Missing. */
	UNREALMCPRQL_API FString Hint(const FUnrealMcpMissingExtension& Missing);
}
