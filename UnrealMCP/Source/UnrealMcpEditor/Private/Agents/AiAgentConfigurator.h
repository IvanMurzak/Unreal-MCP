// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Config/UnrealMcpConfig.h"

class FAiAgentConfig;
class FJsonAiAgentConfig;

/**
 * The resolved connection facts an FAiAgentConfigurator needs to assemble its STDIO + HTTP MCP-client entries
 * (docs/ARCHITECTURE.md §8). Sourced from the live plugin config — NOT from any Unity/Godot path:
 *
 *  - ServerPath  — absolute path to the local `gamedev-mcp-server` binary the STDIO form launches (§6). May be
 *                  empty in the editor before the cli has downloaded it; the STDIO snippet is still useful as a
 *                  template and the file write still succeeds (the path is just not yet present).
 *  - Port        — the deterministic IPC port for this project (§1.1, FUnrealMcpBridgeServer::ComputeDeterministicPort).
 *  - HttpUrl     — the resolved MCP-client URL the HTTP form points at: <effective host>/mcp (Custom host or the
 *                  Cloud base URL), mirroring the cli's `${url}/mcp` contract.
 *  - bAuthRequired / Token — whether a bearer is sent and its real value. In Cloud mode auth is always required
 *                  (the cloud enforces it); in Custom mode it follows AuthOption. The Token is the REAL bearer —
 *                  the on-screen masking happens in the Slate panel, never here.
 *
 * Built by FromPluginConfig() from an FUnrealMcpConfig so the whole class is unit-testable with a hand-built
 * config and an injected ServerPath/Port (no live editor, bridge, or agent).
 */
struct FAiAgentConnectionInfo
{
	FString ServerPath;
	int32 Port = 0;
	FString HttpUrl;
	bool bAuthRequired = false;
	FString Token;

	/**
	 * Resolve the connection facts from the live plugin config. @p InServerPath is the §6 server binary path
	 * (resolved by the caller from the cli/install layout; may be empty). @p InPort is the deterministic IPC port.
	 */
	static UNREALMCPEDITOR_API FAiAgentConnectionInfo FromPluginConfig(const FUnrealMcpConfig& Config, const FString& InServerPath, int32 InPort);
};

/**
 * Abstract base for an AI-agent configurator (docs/ARCHITECTURE.md §7/§8) — the C++/Slate analog of Unity's
 * AiAgentConfigurator and Godot's GodotAgentConfigurator. Unlike Godot (HTTP-only, no server shipped), Unreal
 * ships a server, so — like Unity — every configurator emits BOTH a STDIO (launch-the-server) and an HTTP
 * (connect-to-URL) client config. A subclass supplies identity (name/id/download/tutorial) and the per-agent
 * config-file path + body path; this base assembles the two FJsonAiAgentConfig objects, injecting the bearer
 * token into the STDIO args and the HTTP Authorization header per the resolved auth state.
 *
 * Lifetime / threading: a configurator is a lightweight value object created on the game thread by the registry
 * and the Slate panel. It caches its two configs lazily and rebuilds them when the connection info changes
 * (Invalidate). All symbols are UNREALMCPEDITOR_API-exported so the specs drive them directly.
 */
class FAiAgentConfigurator
{
public:
	virtual ~FAiAgentConfigurator() = default;

	// --- Identity (subclass-supplied). ---
	virtual FString GetAgentName() const = 0;
	virtual FString GetAgentId() const = 0;
	virtual FString GetDownloadUrl() const = 0;
	virtual FString GetTutorialUrl() const { return FString(); }
	virtual FString GetIconFileName() const { return FString(); }

	/**
	 * Absolute path to this agent's MCP config file (project-relative resolved to absolute). Empty for a
	 * snippet-only configurator (e.g. a future Custom entry). @p ProjectRoot is the live project root.
	 */
	virtual FString GetConfigFilePath(const FString& ProjectRoot) const = 0;
	/** The JSON body path this agent nests its server map under (Claude Code / Cursor: "mcpServers"). */
	virtual FString GetBodyPath() const { return TEXT("mcpServers"); }

	// --- Config assembly. ---

	/** (Re)bind the configurator to the resolved connection info + project root; invalidates the cached configs. */
	UNREALMCPEDITOR_API void Initialize(const FAiAgentConnectionInfo& InConnection, const FString& InProjectRoot);
	/** Drop the cached configs so the next Get* call rebuilds them (after a connection/token change). */
	UNREALMCPEDITOR_API void Invalidate();

	/** The STDIO (launch-the-server) MCP-client config for this agent. Lazily built; never null after Initialize. */
	UNREALMCPEDITOR_API FJsonAiAgentConfig& GetConfigStdio();
	/** The HTTP (connect-to-URL) MCP-client config for this agent. Lazily built; never null after Initialize. */
	UNREALMCPEDITOR_API FJsonAiAgentConfig& GetConfigHttp();

	// --- Status helpers (across both transports, like Unity's RefreshConfigurationUI). ---

	/** Whether EITHER transport's entry is present in the file (drives the Remove-button enabled state). */
	UNREALMCPEDITOR_API bool IsAnyDetected();
	/** Whether the entry matching @p bStdio is fully configured (no reconfigure needed). */
	UNREALMCPEDITOR_API bool IsConfigured(bool bStdio);
	/** Configure the entry for the chosen transport (writes/merges the file). Returns success. */
	UNREALMCPEDITOR_API bool Configure(bool bStdio);
	/** Remove BOTH transports' entries from the file (a single Remove clears the agent regardless of transport). */
	UNREALMCPEDITOR_API bool RemoveAll();

protected:
	FAiAgentConnectionInfo Connection;
	FString ProjectRoot;

private:
	TSharedPtr<FJsonAiAgentConfig> ConfigStdio;
	TSharedPtr<FJsonAiAgentConfig> ConfigHttp;

	TSharedRef<FJsonAiAgentConfig> BuildStdio() const;
	TSharedRef<FJsonAiAgentConfig> BuildHttp() const;
};
