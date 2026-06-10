// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpToolRegistry.h"
#include "Async/Future.h"

/**
 * The Unreal-MCP game-thread dispatcher (docs/ARCHITECTURE.md §4) — the analog of Unity's
 * MainThread.Instance.Run() / Godot's GodotMainThread. Tool bodies touch the Unreal API, which is
 * only safe on the game thread, so the IPC reader thread NEVER runs a tool body directly: it calls
 * Dispatch(), which posts the body to the game thread via AsyncTask and returns a TFuture the bridge
 * attaches a continuation to. Slow tools therefore never stall the IPC heartbeat.
 *
 * A per-call timeout completes the future with a structured error if the body has not finished; the
 * body's late result is then dropped (the promise's single-completion guard prevents a double-set, §4).
 */
class UNREALMCPEDITOR_API FUnrealMcpGameThreadDispatcher
{
public:
	/**
	 * Run @p Body on the game thread (inline when already on it, §4) and return a future for its result.
	 * A parallel watcher completes the future with a timeout error after @p Timeout if the body has not
	 * finished. The future is always completed exactly once.
	 */
	TFuture<FUnrealMcpToolResult> Dispatch(
		FUnrealMcpToolCall Call,
		TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)> Body,
		FTimespan Timeout);
};
