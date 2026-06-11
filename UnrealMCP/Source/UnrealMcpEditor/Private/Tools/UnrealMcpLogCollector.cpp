// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpLogCollector.h"

#include "Algo/Reverse.h"

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

	FScopeLock ScopeLock(&Lock);
	Entries.Add(MoveTemp(Entry));
	if (Entries.Num() > MaxEntries)
	{
		// Evict the oldest overflow in one shot (keeps the newest MaxEntries). No shrink — the buffer
		// stays warm at its cap so steady-state logging does no reallocation.
		Entries.RemoveAt(0, Entries.Num() - MaxEntries, EAllowShrinking::No);
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

TArray<FUnrealMcpLogEntry> FUnrealMcpLogCollector::Snapshot(ELogVerbosity::Type MinVerbosity,
	const FString& CategoryFilter, const FString& Search, int32 Limit) const
{
	TArray<FUnrealMcpLogEntry> Out;

	FScopeLock ScopeLock(&Lock);
	// Walk newest-first so we can stop as soon as the requested Limit is satisfied — this bounds both
	// the copy work and the lock-hold time (a full 10k buffer with limit=100 copies 100 entries, not
	// 10k). Entries is oldest-first, so reverse the result below to restore the newest-last contract.
	for (int32 Idx = Entries.Num() - 1; Idx >= 0; --Idx)
	{
		const FUnrealMcpLogEntry& Entry = Entries[Idx];
		// Lower numeric verbosity == more severe (Fatal=1 … VeryVerbose=7). Keep at-or-more-severe.
		if (MinVerbosity != ELogVerbosity::All && Entry.Verbosity > MinVerbosity)
			continue;
		if (!CategoryFilter.IsEmpty() && !Entry.Category.ToString().Equals(CategoryFilter, ESearchCase::IgnoreCase))
			continue;
		if (!Search.IsEmpty() && !Entry.Message.Contains(Search, ESearchCase::IgnoreCase))
			continue;
		Out.Add(Entry);
		if (Limit > 0 && Out.Num() >= Limit)
			break;
	}

	// Out is currently newest-first; the documented contract is newest-last, so reverse in place.
	Algo::Reverse(Out);
	return Out;
}

FString FUnrealMcpLogCollector::VerbosityToString(ELogVerbosity::Type Verbosity)
{
	switch (Verbosity & ELogVerbosity::VerbosityMask)
	{
	case ELogVerbosity::Fatal:       return TEXT("Fatal");
	case ELogVerbosity::Error:       return TEXT("Error");
	case ELogVerbosity::Warning:     return TEXT("Warning");
	case ELogVerbosity::Display:     return TEXT("Display");
	case ELogVerbosity::Log:         return TEXT("Log");
	case ELogVerbosity::Verbose:     return TEXT("Verbose");
	case ELogVerbosity::VeryVerbose: return TEXT("VeryVerbose");
	default:                         return TEXT("Log");
	}
}

bool FUnrealMcpLogCollector::ParseVerbosity(const FString& Token, ELogVerbosity::Type& OutVerbosity)
{
	if (Token.Equals(TEXT("Fatal"), ESearchCase::IgnoreCase))            { OutVerbosity = ELogVerbosity::Fatal; return true; }
	if (Token.Equals(TEXT("Error"), ESearchCase::IgnoreCase))           { OutVerbosity = ELogVerbosity::Error; return true; }
	if (Token.Equals(TEXT("Warning"), ESearchCase::IgnoreCase))         { OutVerbosity = ELogVerbosity::Warning; return true; }
	if (Token.Equals(TEXT("Display"), ESearchCase::IgnoreCase))         { OutVerbosity = ELogVerbosity::Display; return true; }
	if (Token.Equals(TEXT("Log"), ESearchCase::IgnoreCase))             { OutVerbosity = ELogVerbosity::Log; return true; }
	if (Token.Equals(TEXT("Verbose"), ESearchCase::IgnoreCase))         { OutVerbosity = ELogVerbosity::Verbose; return true; }
	if (Token.Equals(TEXT("VeryVerbose"), ESearchCase::IgnoreCase))     { OutVerbosity = ELogVerbosity::VeryVerbose; return true; }
	if (Token.Equals(TEXT("All"), ESearchCase::IgnoreCase))             { OutVerbosity = ELogVerbosity::All; return true; }
	return false;
}
