// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * Registration entry points for the RUNTIME-safe core tool families (docs/ARCHITECTURE.md §10, §12.7).
 *
 * In R1 the only runtime-safe family that has moved into the UnrealMcpRuntime module is `ping`. The
 * editor-only families (actor/component, blueprint, asset, editor/console/reflection, level, screenshot,
 * source) keep their Register declarations in the editor module's UnrealMcpCoreTools.h. The remaining
 * runtime-safe families (actor/component, the runtime console/reflection subset, the runtime screenshot
 * subset, level-get-data) migrate here in R4 (unreal-mcp-runtime-tool-families).
 *
 * Exported with UNREALMCPRUNTIME_API so the editor coordinator wires them on boot (Model A — editor and
 * runtime register on top of the SAME registry) AND the Automation specs can register + exercise them in
 * isolation, and (in R3+) the runtime bootstrap subsystem can register them in a packaged game.
 */
namespace UnrealMcpPingTool
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry);
}
