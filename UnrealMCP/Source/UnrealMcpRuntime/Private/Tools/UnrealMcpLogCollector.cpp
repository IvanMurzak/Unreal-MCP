// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpLogCollector.h"

#include "Misc/OutputDeviceRedirector.h"   // GLog is an FOutputDeviceRedirector*; Add/RemoveOutputDevice deref the complete type

FUnrealMcpLogCollector& FUnrealMcpLogCollector::Get()
{
	// Function-local static: constructed on first use during module startup, AFTER GLog's
	// FOutputDeviceRedirector singleton already exists. Statics are destroyed in reverse construction
	// order, so this collector is torn down BEFORE GLog — which is exactly why the destructor's
	// Shutdown() -> GLog->RemoveOutputDevice(this) is safe (GLog is still alive when we deregister).
	// Shutdown() still runs in the normal module-teardown path; the destructor only bounds the worst case.
	static FUnrealMcpLogCollector Instance;
	return Instance;
}

FUnrealMcpLogCollector::~FUnrealMcpLogCollector()
{
	// Defensive: never outlive our GLog registration. Normal teardown calls Shutdown() explicitly.
	Shutdown();
}

void FUnrealMcpLogCollector::Startup()
{
	if (bRegistered)
		return;
	if (GLog)
	{
		GLog->AddOutputDevice(this);
		bRegistered = true;
	}
}

void FUnrealMcpLogCollector::Shutdown()
{
	if (!bRegistered)
		return;
	// Clear the flag first so any concurrent Serialize() that already passed the GLog dispatch is the
	// last to touch us; GLog->RemoveOutputDevice flushes in-flight callers before returning.
	bRegistered = false;
	if (GLog)
		GLog->RemoveOutputDevice(this);
}

void FUnrealMcpLogCollector::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	Capture(Message, Verbosity, Category);
}

void FUnrealMcpLogCollector::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category, double /*Time*/)
{
	Capture(Message, Verbosity, Category);
}

void FUnrealMcpLogCollector::Capture(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	// SetColor / control verbosities carry no message payload — never buffer them.
	const ELogVerbosity::Type Type = (ELogVerbosity::Type)(Verbosity & ELogVerbosity::VerbosityMask);
	if (Type == ELogVerbosity::SetColor || Type == ELogVerbosity::NoLogging || Message == nullptr)
		return;

	FUnrealMcpLogEntry Entry;
	Entry.Timestamp = FDateTime::UtcNow();
	Entry.Verbosity = Type;
	Entry.Category = Category;
	Entry.Message = Message;
	Entry.bGameThread = IsInGameThread();

	FScopeLock ScopeLock(&Lock);
	// Assigned under the same lock that stores the entry, so sequences are dense and ascending in
	// buffer order even though GLog calls Serialize from many threads.
	Entry.Sequence = ++LastSequence;
	Entries.Add(MoveTemp(Entry));
	if (Entries.Num() > MaxEntries)
	{
		// Amortize the O(N) head-eviction: dropping a single entry per line at the cap would memmove the
		// whole ~MaxEntries buffer on every log call. Instead drop a whole EvictChunk from the front in
		// one shot, so eviction runs ~once per EvictChunk lines (O(1) amortized). The buffer never
		// exceeds MaxEntries — it floats between MaxEntries-EvictChunk and MaxEntries. No shrink keeps the
		// allocation warm so steady-state logging does no reallocation.
		Entries.RemoveAt(0, FMath::Min(EvictChunk, Entries.Num()), EAllowShrinking::No);
	}
}

int32 FUnrealMcpLogCollector::Clear()
{
	FScopeLock ScopeLock(&Lock);
	const int32 Removed = Entries.Num();
	Entries.Reset();
	return Removed;
}

int32 FUnrealMcpLogCollector::Num() const
{
	FScopeLock ScopeLock(&Lock);
	return Entries.Num();
}

TArray<FUnrealMcpLogEntry> FUnrealMcpLogCollector::Snapshot(int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	const int32 Start = Limit > 0 ? FMath::Max(0, Entries.Num() - Limit) : 0;
	return TArray<FUnrealMcpLogEntry>(Entries.GetData() + Start, Entries.Num() - Start);
}

TArray<FUnrealMcpLogEntry> FUnrealMcpLogCollector::SnapshotAfter(int64 AfterSequence) const
{
	FScopeLock ScopeLock(&Lock);
	// Sequences are dense and ascending in buffer order, so the first newer entry is found by arithmetic.
	const int32 Start = Entries.Num() == 0 ? 0 : (int32)FMath::Clamp<int64>(AfterSequence - Entries[0].Sequence + 1, 0, Entries.Num());
	return TArray<FUnrealMcpLogEntry>(Entries.GetData() + Start, Entries.Num() - Start);
}

int64 FUnrealMcpLogCollector::GetLastSequence() const
{
	FScopeLock ScopeLock(&Lock);
	return LastSequence;
}

FString FUnrealMcpLogCollector::VerbosityToString(ELogVerbosity::Type Verbosity)
{
	// Defer to the engine's verbosity→name mapping (Fatal/Error/Warning/Display/Log/Verbose/VeryVerbose), masking
	// off the flag bits first (the `&` yields an int, so cast back to the enum). A captured GLog entry always
	// carries one of those real verbosities.
	return ::ToString(static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask));
}
