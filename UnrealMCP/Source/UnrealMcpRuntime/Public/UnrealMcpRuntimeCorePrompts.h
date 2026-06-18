// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpPromptRegistry;

/**
 * Registration entry point for the core PROMPT family (docs/ARCHITECTURE.md §A.1 / §A.2) — the prompt
 * sibling of UnrealMcpRuntimeCoreTools.h. Exported with UNREALMCPRUNTIME_API so the editor coordinator and
 * the runtime bootstrap subsystem wire it on boot (Model A — editor + runtime register on the SAME prompt
 * registry) AND the Automation specs can register + exercise it in isolation.
 *
 * The MVP registers a single self-contained prompt (`level-design-brief`) with no engine calls, so the
 * family compiles + links in both the editor and the non-editor Game target.
 */
namespace UnrealMcpCorePrompts
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpPromptRegistry& Registry);
}
