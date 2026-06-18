// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpResourceRegistry;

/**
 * Registration entry point for the core RESOURCE family (docs/ARCHITECTURE.md §A.1 / §A.2) — the resource
 * sibling of UnrealMcpRuntimeCoreTools.h / UnrealMcpRuntimeCorePrompts.h. Exported with UNREALMCPRUNTIME_API
 * so the editor coordinator and the runtime bootstrap subsystem wire it on boot (Model A — editor + runtime
 * register on the SAME resource registry) AND the Automation specs can register + exercise it in isolation.
 *
 * The MVP registers two self-contained static-URI resources, both compiling+linking in the editor and the
 * non-editor Game target (Engine-only APIs, no UnrealEd symbols):
 *  - unreal://project/levels — a JSON (application/json) snapshot of the active world + its loaded levels.
 *  - unreal://project/icon   — a tiny base64 PNG blob (image/png), covering the Text-vs-Blob round-trip.
 */
namespace UnrealMcpCoreResources
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpResourceRegistry& Registry);
}
