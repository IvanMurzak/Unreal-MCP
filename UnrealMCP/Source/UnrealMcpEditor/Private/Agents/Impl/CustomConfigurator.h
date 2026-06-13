// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"

/**
 * Custom (Other) configurator (docs/ARCHITECTURE.md §7). The catch-all snippet-only entry for any MCP client not in
 * the built-in roster — mirrors Unity's CustomConfigurator. It has NO config-file path: GetConfigFilePath returns an
 * empty string, which makes the underlying FJsonAiAgentConfig refuse to Configure/Detect (every file op early-returns
 * on an empty path). The user copies the generated STDIO/HTTP snippet by hand into their own client config.
 *
 * The registry guarantees this entry is ALWAYS last regardless of name (the Custom-last invariant Phase A left a hook
 * for), so it sorts to the bottom of the dropdown.
 */
class FCustomConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Other - Custom"); }
	virtual FString GetAgentId() const override { return TEXT("other-custom"); }
	virtual FString GetDownloadUrl() const override { return FString(); }
	virtual FString GetIconFileName() const override { return FString(); }

	/** Snippet-only: no on-disk config file. An empty path makes Configure/Detect/IsConfigured all return false. */
	virtual FString GetConfigFilePath(const FString& /*InProjectRoot*/) const override { return FString(); }
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
};
