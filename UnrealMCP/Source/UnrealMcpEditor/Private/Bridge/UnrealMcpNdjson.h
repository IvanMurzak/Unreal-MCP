// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

/**
 * NDJSON framing codec (docs/ARCHITECTURE.md §1.2): UTF-8 JSON, one message per '\n'-terminated line,
 * line length capped at MaxLineBytes. The C++ peer of the sidecar's NdjsonFramer. The decode side is a
 * stateful accumulator: feed it raw bytes off the socket and it yields complete UTF-8 lines, buffering
 * partial lines across reads. Pure (no socket dependency) so it is unit-tested by an Automation spec.
 */
class UNREALMCPEDITOR_API FUnrealMcpNdjsonAccumulator
{
public:
	static constexpr int32 DefaultMaxLineBytes = 64 * 1024 * 1024;

	explicit FUnrealMcpNdjsonAccumulator(int32 InMaxLineBytes = DefaultMaxLineBytes)
		: MaxLineBytes(InMaxLineBytes) {}

	/**
	 * Push raw bytes; append every completed line (newline + optional trailing '\r' stripped, decoded
	 * UTF-8) to @p OutLines. Returns false when a single un-terminated line grows past the cap — the
	 * caller must abort the connection (§1.2).
	 */
	bool Push(const uint8* Data, int32 Count, TArray<FString>& OutLines);

	/** Encode one JSON line into wire bytes (UTF-8 + trailing '\n'). */
	static TArray<uint8> Encode(const FString& JsonLine);

private:
	TArray<uint8> Buffer;
	int32 MaxLineBytes;

	FString DrainLine();
};
