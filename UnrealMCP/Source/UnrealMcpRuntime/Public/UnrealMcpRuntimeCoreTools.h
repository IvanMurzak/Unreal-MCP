// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * Registration entry points for the RUNTIME-safe core tool families (docs/ARCHITECTURE.md §10, §12.7).
 *
 * R1 moved `ping` down. R4 (unreal-mcp-runtime-tool-families) moves the remaining runtime-safe families
 * here: the actor/component family (made runtime-safe with a World->SpawnActor spawn branch + WITH_EDITOR
 * guards), the runtime console/reflection subset (console-get/clear-logs, console-run-command,
 * reflection-method-find/-call EXCLUDING CallInEditor), the runtime screenshot subset
 * (screenshot-game-view + screenshot-camera), and read-only level-get-data. The editor-only families
 * (blueprint, asset, source, level WRITE, editor-application-*, editor-selection-*, viewport/isolated
 * screenshots, CallInEditor reflection) keep their Register declarations in the editor module's
 * UnrealMcpCoreTools.h.
 *
 * Exported with UNREALMCPRUNTIME_API so the editor coordinator wires them on boot (Model A — editor and
 * runtime register on top of the SAME registry) AND the Automation specs can register + exercise them in
 * isolation, and the runtime bootstrap subsystem (R3) registers them in a packaged game.
 */
namespace UnrealMcpPingTool
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The actor & component tool family (docs/ARCHITECTURE.md §10 "actor family", §12.7). ~13 native C++
 * tools — actor lifecycle (create/destroy/duplicate), scoped reads + FProperty writes (find/modify),
 * parent/attachment, component management (add/destroy/get/modify/list-all), and generic UObject access
 * (object-get-data/object-modify). RUNTIME-SAFE: actor-create spawns through the engine `World->SpawnActor`
 * path against the world resolved by FUnrealMcpWorldProvider (editor world in the editor, the live game
 * world at runtime); editor-only conveniences (`FScopedTransaction`, `FActorLabelUtilities::
 * SetActorLabelUnique`, `EditorDestroyActor`, `GetActorLabel`) are WITH_EDITOR-guarded so the family
 * compiles + links in the non-editor Game target while preserving today's editor behaviour byte-for-byte.
 */
namespace UnrealMcpActorTools
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The runtime console / reflection tool family (docs/ARCHITECTURE.md §10 "editor/reflection family"
 * runtime subset, §12.7). Five kebab-case tools, all runtime-safe: console-get-logs / console-clear-logs
 * (the FUnrealMcpLogCollector GLog ring buffer), console-run-command (GEngine->Exec on the resolved
 * world), and reflection-method-find / reflection-method-call (UFunction discovery + safety-gated
 * ProcessEvent, EXCLUDING CallInEditor — only FUNC_BlueprintCallable/FUNC_Static are accepted so the
 * gate needs no WITH_EDITORONLY_DATA CallInEditor metadata). The editor-only editor-application-* and
 * editor-selection-* tools stay in the editor module's UnrealMcpEditorTools.
 */
namespace UnrealMcpConsoleReflectionTools
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The runtime screenshot subset (docs/ARCHITECTURE.md §10 "screenshot family" runtime subset, §12.7).
 * Two kebab-case tools that return a base64 PNG as MCP image content: screenshot-game-view (the live
 * game viewport via GEngine->GameViewport, version-checked across 5.5→5.7) and screenshot-camera
 * (render from a resolved camera actor through a transient USceneCaptureComponent2D). Both need a
 * GPU-backed (windowed) game/editor — headless `-nullrhi` returns a structured "rendering unavailable"
 * error. The editor-only screenshot-viewport + screenshot-isolated stay in the editor module's
 * UnrealMcpScreenshotTools.
 */
namespace UnrealMcpRuntimeScreenshotTools
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry);

	/** Hard cap (per side) and default for a single capture dimension (§10 — default 1024, hard cap 2048). */
	enum : int32 { MaxCaptureDimension = 2048, DefaultCaptureDimension = 1024 };

	/** Clamp a requested dimension to [1, MaxCaptureDimension]; a value <= 0 yields DefaultCaptureDimension. GPU-free (spec-testable). */
	UNREALMCPRUNTIME_API int32 ResolveCaptureDimension(int64 Requested);

	/** Downscale (InW, InH) proportionally so the longest side is <= MaxCaptureDimension; a no-op when already within the cap. GPU-free. */
	UNREALMCPRUNTIME_API void CapToMaxDimension(int32 InW, int32 InH, int32& OutW, int32& OutH);
}

/**
 * The runtime level-data tool (docs/ARCHITECTURE.md §10 "level family" read-only runtime subset, §12.7).
 * A single read-only tool, level-get-data: an actor-tree snapshot of the live world resolved by
 * FUnrealMcpWorldProvider (each actor's identity, optionally scoped reflected data via the dotted
 * `paths` filter). All level WRITE tools (level-create/open/save/set-current/unload-sublevel) and
 * level-list-loaded stay editor-only in the editor module's UnrealMcpLevelTools.
 */
namespace UnrealMcpRuntimeLevelTools
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry);
}
