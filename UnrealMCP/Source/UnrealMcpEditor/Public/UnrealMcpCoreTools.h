// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * Registration entry points for the core tool families (docs/ARCHITECTURE.md §10). Exported so the
 * runtime coordinator wires them on boot AND the Automation specs can register + exercise them in
 * isolation. `ping` ships with the sidecar-bridge task; later families add their own Register here.
 */
namespace UnrealMcpPingTool
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The actor & component tool family (docs/ARCHITECTURE.md §10 "actor family"): actor lifecycle
 * (create/destroy/duplicate), scoped reads + FProperty writes (find/modify), parent/attachment,
 * component management (add/destroy/get/modify/list-all), and generic UObject access
 * (object-get-data/object-modify). ~13 native C++ tools, all declared via the §3.3 builder and run
 * on the GameThread dispatcher. Exported so the runtime wires it on boot AND the Automation specs
 * can register + exercise it in isolation.
 */
namespace UnrealMcpActorTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The Blueprint tool family (docs/ARCHITECTURE.md §10 — FLAGSHIP, Unreal-unique). MVP floor: a closed
 * read -> structure-edit -> compile -> spawn loop (~11 kebab-case CORE tools). Every edit is pinned to
 * the public FKismetEditorUtilities / FBlueprintEditorUtils (+ the public SCS surface) and runs on the
 * game-thread dispatcher. Exported so the runtime wires it on boot AND specs exercise it in isolation.
 */
namespace UnrealMcpBlueprintTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The asset / Content-Browser tool family (docs/ARCHITECTURE.md §10, issue #10): ~11 kebab-case
 * tools over the AssetRegistry, UEditorAssetLibrary, AssetTools and the editor material APIs.
 * Registered in the boot path alongside `ping`; also exercised in isolation by Automation specs.
 */
namespace UnrealMcpAssetTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The C++ source / script tool family (docs/ARCHITECTURE.md §10 "source family", issue #18): six
 * kebab-case CORE tools — source-read / source-create-class / source-update / source-delete /
 * source-list / source-compile — that scaffold, read, edit, list and compile project C++. All file
 * operations are jailed to <Project>/Source; source-compile returns a structured §3
 * {file,line,severity,message} build report (the AI feedback loop). Registered in the boot path
 * alongside the other families; also exercised in isolation by Automation specs.
 */
namespace UnrealMcpSourceTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}
