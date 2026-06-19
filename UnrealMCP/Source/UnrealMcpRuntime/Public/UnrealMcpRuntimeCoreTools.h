// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * Registration entry point for the RUNTIME-safe built-in tool family (docs/ARCHITECTURE.md §10, §12).
 *
 * `ping` is the ONLY built-in tool the runtime module ships: a liveness probe + a non-empty manifest so a
 * runtime connection always has at least one tool. Every engine-development tool family — actor/component,
 * console/reflection, screenshot, level-read/write, blueprint, asset, source, editor-application/selection —
 * is EDITOR-ONLY and registered by the editor coordinator from the editor module's UnrealMcpCoreTools.h
 * (those tools drive the editor: undo transactions, the asset registry, Blueprint compilation, the editor
 * world — and several are RCE-class, e.g. reflection-method-call / console-run-command — so they do not
 * belong compiled into a shipped game by default).
 *
 * A game that wants runtime MCP tools brings its own: register an IUnrealMcpToolProvider via
 * UUnrealMcpRuntimeSubsystem::RegisterToolProvider (the §5 extension bus). The runtime module stays lean
 * infra (bridge / registry / dispatcher / sidecar / world-provider / extension-manager) + `ping`.
 *
 * Exported with UNREALMCPRUNTIME_API so the editor coordinator wires `ping` on boot (Model A — editor and
 * runtime register on top of the SAME registry), the runtime bootstrap subsystem registers it in a packaged
 * game, AND the Automation specs can register + exercise it in isolation.
 */
namespace UnrealMcpPingTool
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry);
}
