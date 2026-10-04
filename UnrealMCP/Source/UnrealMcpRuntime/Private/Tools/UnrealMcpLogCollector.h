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
	/**
	 * Monotonic capture sequence, 1-based, assigned under the collector lock together with storing the
	 * entry. Never reset by eviction or `Clear()` (only a process/editor restart restarts it) — it is the
	 * cursor `console-get-logs` `sinceSequence` pages on.
	 */
	int64 Sequence = 0;
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
class UNREALMCPRUNTIME_API FUnrealMcpLogCollector : public FOutputDevice
{
public:
	/** Max retained entries before the oldest are evicted (ring-buffer cap, §10 "strictly capped"). */
	static constexpr int32 MaxEntries = 10000;

	/**
	 * Eviction batch size. Head-eviction shifts every surviving element, so trimming one entry per line
	 * at the cap would memmove the whole buffer on every log call. Instead, once the cap is reached the
	 * oldest `EvictChunk` entries are dropped in a single memmove, so eviction runs ~once per `EvictChunk`
	 * lines (O(1) amortized) rather than every line. The buffer never exceeds `MaxEntries`; it floats
	 * between `MaxEntries - EvictChunk` and `MaxEntries`.
	 */
	static constexpr int32 EvictChunk = MaxEntries / 10;

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
		const FString& Search, int32 Limit, int64* OutNextCursor = nullptr) const;

	/**
	 * Cursor read for `console-get-logs` `sinceSequence`: entries with `Sequence > SinceSequence`,
	 * **oldest-first**, in the order cursor -> filters -> ascending -> limit. When more matching entries
	 * exist than @p Limit, the OLDEST @p Limit are returned (so the caller can continue without a gap).
	 *
	 * A cursor ABOVE the current high-water mark means the counter restarted (editor restart) — it is
	 * treated as if it were below everything, returning the oldest page. A cursor below the oldest
	 * retained entry (evicted history) naturally returns the oldest retained page.
	 *
	 * @param OutNextCursor if non-null, receives the sequence the caller should pass next, computed under
	 *                      the same lock as the slice: the last returned sequence when the page was cut
	 *                      by @p Limit, otherwise the current high-water mark (everything newer than the
	 *                      cursor was either returned or filtered out).
	 */
	TArray<FUnrealMcpLogEntry> SnapshotSince(int64 SinceSequence, ELogVerbosity::Type MinVerbosity,
		const FString& CategoryFilter, const FString& Search, int32 Limit, int64* OutNextCursor = nullptr) const;

	/** Highest sequence assigned so far (0 when nothing was ever captured). */
	int64 HighestSequence() const;

	/** Human-readable severity token for a verbosity ("Error", "Warning", "Display", "Log", …). */
	static FString VerbosityToString(ELogVerbosity::Type Verbosity);

	/** Parse a severity token (case-insensitive) into a verbosity; returns false on an unknown token. */
	static bool ParseVerbosity(const FString& Token, ELogVerbosity::Type& OutVerbosity);

	/**
	 * Construct a standalone, UNREGISTERED collector (never attached to `GLog`). The process singleton
	 * is `Get()`; this exists so specs can drive `Serialize()` / eviction / cursor paging against a
	 * private buffer without touching the live shared one.
	 */
	FUnrealMcpLogCollector() = default;

private:
	void Capture(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category);

	/** The verbosity / category / search filter shared by `Snapshot` and `SnapshotSince`. */
	static bool MatchesFilters(const FUnrealMcpLogEntry& Entry, ELogVerbosity::Type MinVerbosity,
		const FString& CategoryFilter, const FString& Search);

	mutable FCriticalSection Lock;
	TArray<FUnrealMcpLogEntry> Entries;   // oldest-first; evicts from the front past MaxEntries
	int64 LastSequence = 0;               // high-water mark; see FUnrealMcpLogEntry::Sequence
	bool bRegistered = false;
};
