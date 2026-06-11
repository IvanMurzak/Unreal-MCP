// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "Misc/DateTime.h"
#include "HAL/CriticalSection.h"

/**
 * Captured log line (docs/ARCHITECTURE.md §10 editor/reflection family — the Godot `GodotLogCollector`
 * analog). One ring-buffer entry: severity + category + message + a wall-clock capture timestamp.
 */
struct FUnrealMcpLogEntry
{
	FDateTime Timestamp;                 // wall-clock (UTC) at capture
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FName Category;
	FString Message;
};

/**
 * Process-wide `GLog` listener feeding `console-get-logs` / `console-clear-logs`
 * (docs/ARCHITECTURE.md §10). An `FOutputDevice` registered with `GLog` at module startup; every
 * engine log line is mirrored into a capped ring buffer (default 10k entries) carrying severity,
 * category and timestamp. Tool handlers read filtered/paginated slices on the game thread.
 *
 * Thread-safety: `GLog->Serialize` is called from arbitrary threads, so every buffer mutation/read is
 * guarded by a critical section and `CanBeUsedOnAnyThread()` returns true. The capture path NEVER logs
 * (that would recurse through `GLog` back into `Serialize`).
 *
 * Lifecycle: a function-local singleton constructed during module startup (after `GLog` already
 * exists) and destroyed in reverse construction order — i.e. BEFORE `GLog` — so the destructor can
 * safely deregister itself from a still-live `GLog`. `Startup()`/`Shutdown()` register/deregister and
 * are idempotent — `Shutdown()` MUST still run at module teardown so no dangling listener lingers on
 * `GLog` while the editor keeps running (an orphaned device crashes the editor on exit).
 */
class UNREALMCPEDITOR_API FUnrealMcpLogCollector : public FOutputDevice
{
public:
	/** Max retained entries before the oldest are evicted (ring-buffer cap, §10 "strictly capped"). */
	static constexpr int32 MaxEntries = 10000;

	/** The process singleton. */
	static FUnrealMcpLogCollector& Get();

	virtual ~FUnrealMcpLogCollector() override;

	/** Register with `GLog` (idempotent — a second call is a no-op). */
	void Startup();

	/** Deregister from `GLog` (idempotent, safe at module shutdown). */
	void Shutdown();

	/** True once registered with `GLog`. */
	bool IsRegistered() const { return bRegistered; }

	// --- FOutputDevice ---------------------------------------------------------------------------
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category, double Time) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/** Drop every retained entry. Returns the number removed. */
	int32 Clear();

	/** Current retained entry count. */
	int32 Num() const;

	/**
	 * Copy a filtered, newest-last snapshot of the buffer under the lock.
	 * @param MinVerbosity   keep entries at this severity or MORE severe (lower numeric value); pass
	 *                       `ELogVerbosity::All` to keep everything.
	 * @param CategoryFilter case-insensitive exact category match; empty keeps all categories.
	 * @param Search         case-insensitive substring match on the message; empty keeps all.
	 * @param Limit          max entries returned (the most-recent @p Limit after filtering); <=0 → all.
	 */
	TArray<FUnrealMcpLogEntry> Snapshot(ELogVerbosity::Type MinVerbosity, const FString& CategoryFilter,
		const FString& Search, int32 Limit) const;

	/** Human-readable severity token for a verbosity ("Error", "Warning", "Display", "Log", …). */
	static FString VerbosityToString(ELogVerbosity::Type Verbosity);

	/** Parse a severity token (case-insensitive) into a verbosity; returns false on an unknown token. */
	static bool ParseVerbosity(const FString& Token, ELogVerbosity::Type& OutVerbosity);

private:
	FUnrealMcpLogCollector() = default;

	void Capture(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category);

	mutable FCriticalSection Lock;
	TArray<FUnrealMcpLogEntry> Entries;   // oldest-first; evicts from the front past MaxEntries
	bool bRegistered = false;
};
