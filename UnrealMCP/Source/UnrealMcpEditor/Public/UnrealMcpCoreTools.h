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
 * The level / map tool family (docs/ARCHITECTURE.md §10 "level family", issue #16 — the Unity Scene.*
 * analog): level-create / level-open / level-save (save-as via optional path) / level-get-data (scoped
 * actor-tree reads) / level-list-loaded (persistent + streaming sublevels, World-Partition aware,
 * read-only) / level-set-current / level-unload-sublevel. 7 native C++ tools over the editor UWorld +
 * UEditorLoadingAndSavingUtils + UEditorLevelUtils (Engine/UnrealEd only — no LevelEditor module, no
 * UEditorActorSubsystem, so every body is headless-safe under -nullrhi). Registered in the boot path
 * alongside the other core families; also exercised in isolation by Automation specs.
 */
namespace UnrealMcpLevelTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}
