// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * Registration entry points for the EDITOR-only core tool families (docs/ARCHITECTURE.md §10). Exported
 * so the editor coordinator wires them on boot AND the Automation specs can register + exercise them in
 * isolation. The runtime-safe families moved to the runtime module (UnrealMcpRuntimeCoreTools.h): `ping`
 * (R1), and in R4 the actor/component family, the runtime console/reflection subset, the runtime
 * screenshot subset (screenshot-game-view + screenshot-camera), and read-only level-get-data. The
 * families below are the editor-only remainder.
 */

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
 * The editor-application / selection tool family (docs/ARCHITECTURE.md §10 "editor/reflection family"
 * editor-only subset, issue #19): four kebab-case EDITOR-ONLY tools — editor-application-get/set-state
 * (PIE control) and editor-selection-get/set (editor actor selection). The runtime-safe subset of this
 * family (console-get/clear-logs, console-run-command, reflection-method-find/-call) moved to the
 * runtime module's UnrealMcpConsoleReflectionTools in R4 (§12.7). Registered in the editor boot path;
 * also exercised in isolation by Automation specs.
 */
namespace UnrealMcpEditorTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The level / map tool family (docs/ARCHITECTURE.md §10 "level family", issue #16 — the Unity Scene.*
 * analog): level-create / level-open / level-save (save-as via optional path) / level-list-loaded
 * (persistent + streaming sublevels, World-Partition aware, read-only) / level-set-current /
 * level-unload-sublevel. 6 native C++ EDITOR-ONLY tools over the editor UWorld +
 * (read-only level-get-data moved to the runtime module's UnrealMcpRuntimeLevelTools in R4, §12.7).
 * UEditorLoadingAndSavingUtils + UEditorLevelUtils (Engine/UnrealEd only — no LevelEditor module, no
 * UEditorActorSubsystem, so every body is headless-safe under -nullrhi). Registered in the boot path
 * alongside the other core families; also exercised in isolation by Automation specs.
 */
namespace UnrealMcpLevelTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The editor screenshot / viewport-capture tool family (docs/ARCHITECTURE.md §10 "screenshot family",
 * issue #17). Two kebab-case EDITOR-ONLY tools that return a base64 PNG as MCP image content so an LLM
 * can directly inspect what the editor is rendering: `screenshot-viewport` (active editor viewport, via
 * GEditor->GetActiveViewport) and `screenshot-isolated` (transient SceneCapture2D + show-only list).
 * The runtime-safe subset (`screenshot-game-view`, `screenshot-camera`) moved to the runtime module's
 * UnrealMcpRuntimeScreenshotTools in R4 (§12.7). Captures are dimension-capped (default 1024, hard cap
 * 2048 per side, width/height clamped). Every handler runs ON the game thread (the dispatcher guarantees
 * it, §4). Exported so the editor coordinator wires it on boot AND the Automation specs can register +
 * exercise the GPU-free logic in isolation.
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
