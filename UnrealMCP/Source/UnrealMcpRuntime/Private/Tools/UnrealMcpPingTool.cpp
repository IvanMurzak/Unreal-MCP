// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntimeCoreTools.h"
#include "UnrealMcpToolRegistry.h"

/**
 * The `ping` tool (docs/ARCHITECTURE.md §10 — "ping ships in the sidecar-bridge task"). The end-to-end
 * smoke target: MCP server → SignalR → sidecar → IPC → UE plugin (game thread) → back. Returns the
 * input `message` echoed, or "pong" when omitted — matching the Unity/Godot ping contract
 * (structured `{ "result": <message-or-pong> }`).
 */
namespace UnrealMcpPingTool
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("ping"))
			.Title(TEXT("Ping"))
			.Description(TEXT("Lightweight readiness probe. Returns the input 'message' echoed back, or "
			                  "'pong' when omitted. Useful for connectivity smoke checks across the full "
			                  "MCP -> sidecar -> Unreal editor path."))
			.ParamString(TEXT("message"), TEXT("Optional message to echo back; defaults to 'pong'."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Runs ON the game thread (the dispatcher guarantees it, §4).
				const FString Result = Call.Has(TEXT("message")) ? Call.GetString(TEXT("message")) : TEXT("pong");

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("result"), Result);
				return FUnrealMcpToolResult::Success(Result, Structured);
			});
	}
}
