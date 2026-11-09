// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * The GRAPH-AUTHORING half of the Blueprint family (docs/ARCHITECTURE.md §10), split out of
 * UnrealMcpBlueprintTools.cpp because it edits inside graphs rather than the Blueprint's structure:
 * `blueprint-graph` (read a graph's nodes / pins / links), `blueprint-add-node` (create one node),
 * `blueprint-connect-pins` / `blueprint-disconnect-pins` (wire and unwire pins through the K2 schema).
 *
 * Called from UnrealMcpBlueprintTools::Register, so the family keeps ONE public entry point and every
 * existing caller (the coordinator, the Automation specs) picks the new tools up unchanged.
 */
namespace UnrealMcpBlueprintTools
{
	void RegisterGraphTools(FUnrealMcpToolRegistry& Registry);
}
