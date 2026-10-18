// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;
struct FUnrealMcpToolResult;

/**
 * Registration entry points for the EDITOR-only core tool families (docs/ARCHITECTURE.md §10; the RQL family —
 * rql-query / rql-mutate / rql-schema — registers from the UnrealMcpRql module, UnrealMcpRqlTools.h). Exported
 * so the editor coordinator wires them on boot AND the Automation specs can register + exercise them in
 * isolation. The runtime-safe `ping` family moved to the runtime module (UnrealMcpRuntimeCoreTools.h,
 * §12.1); the other families below are editor-only until their runtime subset migrates in R4.
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
 * The console tool family: `console-run-command` (GEngine->Exec with captured output). The rest of the former editor /
 * console / reflection family is RQL now (docs/rql.md): the `editor` and `console` sources, `call` and rql-schema.
 * Registered in the boot path alongside the other CORE families; also exercised in isolation by specs.
 */
namespace UnrealMcpEditorTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * The Python tool family: `python-execute` runs agent Python inside the editor through PythonScriptPlugin and the
 * Code-Exec runtime vendored in Content/Python/unreal_mcp/_vendor (IvanMurzak/Code-Exec): last expression = result,
 * print / unreal.log = logs, errors in snippet.py coordinates, request and response travel as base64. Registered in the
 * boot path; also exercised in isolation by specs.
 */
namespace UnrealMcpPythonTools
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

	/**
	 * Build a screenshot tool's success result: one image content block plus one text block carrying the
	 * capture metadata (source, width, height, mimeType, encoded byte size); no structured content (see
	 * FUnrealMcpToolResult::SuccessWithImage). GPU-free (spec-testable).
	 */
	UNREALMCPEDITOR_API FUnrealMcpToolResult MakeImageResult(const FString& Source, int32 Width, int32 Height, const FString& Base64Png, int32 EncodedBytes);
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
