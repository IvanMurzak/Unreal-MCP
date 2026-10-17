// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Console tool family specs: console-run-command registration + happy/error path, and the FUnrealMcpLogCollector ring
 * buffer behind the RQL `console` source (capture, filter, clear, sequence cursors). Executed in EditorContext so GEditor
 * and the editor world are live. The rest of the former editor / reflection family is RQL (UnrealMcpRqlSpec.cpp).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpEditorToolsSpec, "UnrealMcp.Tools.Console",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static TSharedPtr<FJsonObject> Args() { return MakeShared<FJsonObject>(); }

	FUnrealMcpToolResult Run(FUnrealMcpToolRegistry& Reg, const FString& Tool, const TSharedPtr<FJsonObject>& A)
	{
		return Reg.Execute(Tool, FUnrealMcpToolCall(A));
	}

END_DEFINE_SPEC(FUnrealMcpEditorToolsSpec)

void FUnrealMcpEditorToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers console-run-command as a core tool and bumps the revision", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpEditorTools::Register(Registry);

			TestTrue(TEXT("has console-run-command"), Registry.HasTool(TEXT("console-run-command")));
			TestEqual(TEXT("tool count"), Registry.Num(), 1);
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);
			const FUnrealMcpRegisteredTool* Tool = Registry.Find(TEXT("console-run-command"));
			TestTrue(TEXT("found"), Tool != nullptr);
			if (Tool)
			{
				TestEqual(TEXT("core extension id"), Tool->ExtensionId, FString(TEXT("core")));
				TestFalse(TEXT("schema hash non-empty"), Tool->SchemaHash.IsEmpty());
			}
		});
	});

	Describe("console", [this]()
	{
		It("runs a console command and captures handled/output", [this]()
		{
			FUnrealMcpToolRegistry Registry; UnrealMcpEditorTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("command"), TEXT("r.ScreenPercentage")); // read-only CVar query — prints its value
			const FUnrealMcpToolResult R = Run(Registry, TEXT("console-run-command"), A);
			TestTrue(TEXT("command runs"), R.bSuccess);
			TestTrue(TEXT("missing command is rejected"), !Run(Registry, TEXT("console-run-command"), Args()).bSuccess);
		});

		It("captures GLog lines into the ring buffer, filters them, and clears", [this]()
		{

			// Drive the collector directly (the runtime that normally Startup()s it is not running in specs).
			// NOTE: the collector is a process-wide singleton. Running this spec inside a LIVE editor session
			// (rather than a throwaway -nullrhi automation process) is DESTRUCTIVE to the runtime's buffered
			// logs — the Clear() below and console-clear-logs wipe the shared buffer. Registration state is
			// snapshotted/restored at the end, but buffer CONTENTS are not. Acceptable for automation; do not
			// run against a session whose captured logs you need to keep.
			FUnrealMcpLogCollector& Collector = FUnrealMcpLogCollector::Get();
			const bool bWasRegistered = Collector.IsRegistered();
			Collector.Startup();
			Collector.Clear();

			const FString Needle = TEXT("McpLogProbe_4d9f");
			UE_LOG(LogTemp, Display, TEXT("%s once"), *Needle);
			GLog->Flush();

			const TArray<FUnrealMcpLogEntry> Found = Collector.Snapshot(ELogVerbosity::All, FString(), Needle, 0);
			TestTrue(TEXT("probe line captured"), Found.Num() >= 1);

			Collector.Clear();
			TestEqual(TEXT("buffer empty after clear"), Collector.Num(), 0);

			// Restore the registration state we found (don't leave a dangling GLog device across specs).
			if (!bWasRegistered)
				Collector.Shutdown();
		});
	});

	// console-get-logs `sinceSequence` cursor (taskflow agent-context-efficiency, row e1). The collector specs
	// drive TEST-LOCAL, never-registered FUnrealMcpLogCollector instances, so they neither touch the live
	// shared buffer nor depend on what else the editor is logging. The one tool-level spec uses the shared
	// singleton but never calls Clear() and only matches its own unique needle.
	Describe("console cursor", [this]()
	{
		auto Feed = [](FUnrealMcpLogCollector& C, const TCHAR* Category, const FString& Msg)
		{
			C.Serialize(*Msg, ELogVerbosity::Log, FName(Category));
		};

		auto Seqs = [](const TArray<FUnrealMcpLogEntry>& Slice)
		{
			TArray<int64> Out;
			for (const FUnrealMcpLogEntry& E : Slice) Out.Add(E.Sequence);
			return Out;
		};

		// Array equality with the two arrays in the failure message (no reliance on a TArray TestEqual overload).
		auto SeqEq = [this](const TCHAR* What, const TArray<int64>& Actual, const TArray<int64>& Expected)
		{
			auto Join = [](const TArray<int64>& A)
			{
				return FString::JoinBy(A, TEXT(" "), [](int64 V) { return LexToString(V); });
			};
			TestTrue(*FString::Printf(TEXT("%s (got [%s] expected [%s])"), What, *Join(Actual), *Join(Expected)), Actual == Expected);
		};

		It("assigns dense ascending sequences from 1 in capture order", [this, Feed, Seqs]()
		{
			FUnrealMcpLogCollector C;
			TestEqual(TEXT("nothing captured yet"), C.HighestSequence(), (int64)0);
			for (int32 I = 0; I < 5; ++I) Feed(C, TEXT("LogMcpSeq"), FString::Printf(TEXT("line %d"), I));

			const TArray<int64> S = Seqs(C.SnapshotSince(0, ELogVerbosity::All, FString(), FString(), 0));
			TestEqual(TEXT("five entries"), S.Num(), 5);
			for (int32 I = 0; I < S.Num(); ++I)
				TestEqual(*FString::Printf(TEXT("entry %d sequence"), I), S[I], (int64)(I + 1));
			TestEqual(TEXT("high-water mark"), C.HighestSequence(), (int64)5);
		});

		It("pages with no gaps or duplicates: the union of pages equals the full filtered set", [this, Feed, Seqs, SeqEq]()
		{
			FUnrealMcpLogCollector C;
			// Interleave matching (LogMcpA) and non-matching (LogMcpB) entries, with a run of six consecutive
			// non-matching entries (longer than the page) in the middle and a non-matching tail. A newest-page
			// implementation drops matching entries; a filter-AFTER-limit implementation returns an empty page
			// on the long run (and the loop below stops there); a cursor that does not skip the filtered tail
			// on the last page ends below the high-water mark.
			TArray<int64> Expected;
			for (int32 I = 0; I < 40; ++I)
			{
				const bool bMatch = (I % 3) != 1 && (I < 10 || I > 15) && I < 37;
				Feed(C, bMatch ? TEXT("LogMcpA") : TEXT("LogMcpB"), FString::Printf(TEXT("page line %d"), I));
				if (bMatch) Expected.Add(I + 1);
			}

			TArray<int64> Union;
			int64 Cursor = 0;
			int32 Pages = 0;
			for (; Pages < 50; ++Pages)
			{
				int64 Next = 0;
				const TArray<FUnrealMcpLogEntry> Page = C.SnapshotSince(Cursor, ELogVerbosity::All, TEXT("LogMcpA"), FString(), 4, &Next);
				if (Page.Num() == 0) { Cursor = Next; break; }
				TestTrue(TEXT("page respects limit"), Page.Num() <= 4);
				if (Next < C.HighestSequence())
					TestEqual(TEXT("a page cut before the high-water mark is full"), Page.Num(), 4);
				Union.Append(Seqs(Page));
				TestTrue(TEXT("next cursor is at or past the page's last entry"), Next >= Page.Last().Sequence);
				Cursor = Next;
			}
			TestTrue(TEXT("paging terminated"), Pages < 50);
			SeqEq(TEXT("union of pages == the full matching set, oldest-first, no gaps/duplicates"), Union, Expected);
			TestEqual(TEXT("final cursor is the high-water mark"), Cursor, C.HighestSequence());
		});

		It("keeps the counter across eviction and Clear", [this, Feed, Seqs]()
		{
			FUnrealMcpLogCollector C;
			const int32 Total = FUnrealMcpLogCollector::MaxEntries + 5; // crosses the cap exactly once
			for (int32 I = 0; I < Total; ++I) Feed(C, TEXT("LogMcpEvict"), FString::Printf(TEXT("e%d"), I));

			TestTrue(TEXT("eviction happened"), C.Num() < Total);
			TestEqual(TEXT("counter did not reset on eviction"), C.HighestSequence(), (int64)Total);

			int64 Next = 0;
			const TArray<FUnrealMcpLogEntry> Oldest = C.SnapshotSince(0, ELogVerbosity::All, FString(), FString(), 1, &Next);
			TestEqual(TEXT("one entry"), Oldest.Num(), 1);
			if (Oldest.Num() == 1)
				TestEqual(TEXT("oldest retained is the first non-evicted sequence"), Oldest[0].Sequence, (int64)(FUnrealMcpLogCollector::EvictChunk + 1));

			Feed(C, TEXT("LogMcpEvict"), TEXT("after eviction"));
			TestEqual(TEXT("next entry continues the counter"), C.HighestSequence(), (int64)(Total + 1));

			C.Clear();
			Feed(C, TEXT("LogMcpEvict"), TEXT("after clear"));
			const TArray<int64> AfterClear = Seqs(C.SnapshotSince(0, ELogVerbosity::All, FString(), FString(), 0));
			TestEqual(TEXT("one entry after Clear"), AfterClear.Num(), 1);
			if (AfterClear.Num() == 1)
				TestEqual(TEXT("Clear does not reset the counter"), AfterClear[0], (int64)(Total + 2));
		});

		It("a cursor older than the retained history returns the oldest retained page", [this, Feed, Seqs, SeqEq]()
		{
			FUnrealMcpLogCollector C;
			const int32 Total = FUnrealMcpLogCollector::MaxEntries + 5;
			for (int32 I = 0; I < Total; ++I) Feed(C, TEXT("LogMcpEvict"), FString::Printf(TEXT("e%d"), I));

			const TArray<int64> S = Seqs(C.SnapshotSince(10, ELogVerbosity::All, FString(), FString(), 3));
			const int64 FirstRetained = (int64)FUnrealMcpLogCollector::EvictChunk + 1;
			SeqEq(TEXT("three oldest retained entries"), S, TArray<int64>({ FirstRetained, FirstRetained + 1, FirstRetained + 2 }));
		});

		It("a stale cursor above the high-water mark (counter restarted) returns entries, not nothing", [this, Feed, Seqs, SeqEq]()
		{
			FUnrealMcpLogCollector C;
			for (int32 I = 0; I < 5; ++I) Feed(C, TEXT("LogMcpStale"), FString::Printf(TEXT("s%d"), I));

			int64 Next = 0;
			const TArray<int64> S = Seqs(C.SnapshotSince(999, ELogVerbosity::All, FString(), FString(), 0, &Next));
			SeqEq(TEXT("stale cursor returns the whole (oldest) page"), S, TArray<int64>({ 1, 2, 3, 4, 5 }));
			TestEqual(TEXT("next cursor is the high-water mark"), Next, (int64)5);

			// A cursor AT the high-water mark is "caught up": nothing new, cursor unchanged.
			const TArray<FUnrealMcpLogEntry> None = C.SnapshotSince(5, ELogVerbosity::All, FString(), FString(), 0, &Next);
			TestEqual(TEXT("caught-up cursor returns nothing"), None.Num(), 0);
			TestEqual(TEXT("caught-up cursor stays put"), Next, (int64)5);
		});

		It("without a cursor, Snapshot still returns the most-recent N, newest last", [this, Feed, Seqs, SeqEq]()
		{
			FUnrealMcpLogCollector C;
			for (int32 I = 0; I < 5; ++I) Feed(C, TEXT("LogMcpRecent"), FString::Printf(TEXT("r%d"), I));

			int64 Next = 0;
			SeqEq(TEXT("the two most recent entries, newest last"),
				Seqs(C.Snapshot(ELogVerbosity::All, FString(), FString(), 2, &Next)), TArray<int64>({ 4, 5 }));
			TestEqual(TEXT("next cursor is the high-water mark"), Next, (int64)5);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
