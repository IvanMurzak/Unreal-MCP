// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * Registration entry points for the EDITOR-only core tool families (docs/ARCHITECTURE.md §10). Exported
 * so the editor coordinator wires them on boot AND the Automation specs can register + exercise them in
 * isolation. The runtime-safe `ping` family moved to the runtime module (UnrealMcpRuntimeCoreTools.h,
 * §12.1); the other families below are editor-only until their runtime subset migrates in R4.
 */

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
 * The editor / console / reflection tool family (docs/ARCHITECTURE.md §10 "editor/reflection family",
 * issue #19): ~9 kebab-case tools — editor-application-get/set-state (PIE), editor-selection-get/set,
 * console-get/clear-logs (backed by the module-startup FUnrealMcpLogCollector GLog ring buffer),
 * console-run-command, and reflection-method-find/call (safety-gated UFunction discovery + ProcessEvent).
 * Registered in the boot path alongside the other CORE families; also exercised in isolation by specs.
 */
namespace UnrealMcpEditorTools
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

/**
 * The screenshot / viewport-capture tool family (docs/ARCHITECTURE.md §10 "screenshot family", issue
 * #17). Four kebab-case CORE tools that return a base64 PNG as MCP image content so an LLM can directly
 * inspect what the editor/game is rendering: `screenshot-viewport` (active editor viewport),
 * `screenshot-game-view` (PIE/game view), `screenshot-camera` (render from a resolved camera actor via
 * USceneCaptureComponent2D), `screenshot-isolated` (transient SceneCapture2D + show-only list).
 * Captures are dimension-capped (default 1024, hard cap 2048 per side, width/height clamped). Every
 * handler runs ON the game thread (the dispatcher guarantees it, §4). Exported so the runtime wires it
 * on boot AND the Automation specs can register + exercise the GPU-free logic in isolation.
 *
 * Actual pixel capture needs a GPU-backed editor — headless `-nullrhi` cannot render, so the capture
 * paths return a structured error there and are LIVE-VERIFIED WINDOWED; the logic/clamp/error branches
 * (below) are GPU-free and covered by headless Automation specs.
 */
namespace UnrealMcpScreenshotTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);

	/** Hard cap (per side) and default for a single capture dimension (§10 — default 1024, hard cap 2048). */
	enum : int32 { MaxCaptureDimension = 2048, DefaultCaptureDimension = 1024 };

	/** Clamp a requested dimension to [1, MaxCaptureDimension]; a value <= 0 yields DefaultCaptureDimension. GPU-free (spec-testable). */
	UNREALMCPEDITOR_API int32 ResolveCaptureDimension(int64 Requested);

	/** Downscale (InW, InH) proportionally so the longest side is <= MaxCaptureDimension; a no-op when already within the cap. GPU-free. */
	UNREALMCPEDITOR_API void CapToMaxDimension(int32 InW, int32 InH, int32& OutW, int32& OutH);
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
