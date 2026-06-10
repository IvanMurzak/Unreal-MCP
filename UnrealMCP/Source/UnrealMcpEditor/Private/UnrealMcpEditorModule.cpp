// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "UnrealMcpLog.h"

/**
 * Editor module for the Unreal-MCP plugin.
 *
 * Scaffold stage: only proves the plugin compiles and loads. The real wiring
 * (docs/ARCHITECTURE.md) lands in later tasks:
 *  - FUnrealMcpToolRegistry        — core tool families + extension providers (§2, §5)
 *  - FUnrealMcpSchemaGenerator     — FProperty -> JSON Schema (§3)
 *  - FUnrealMcpGameThreadDispatcher— AsyncTask + TPromise (§4)
 *  - FUnrealMcpBridgeServer        — localhost TCP listener, NDJSON framing (§1)
 *  - FUnrealMcpSidecarManager      — download / spawn / watchdog / kill (§6)
 *  - Slate UI                      — main window + 4 aux tabs (§7)
 */
class FUnrealMcpEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The canonical boot line — the headless smoke test greps for it.
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] plugin loaded"));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] plugin shutting down"));
	}
};

IMPLEMENT_MODULE(FUnrealMcpEditorModule, UnrealMcpEditor)
