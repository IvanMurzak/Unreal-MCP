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

			const TArray<FUnrealMcpLogEntry> Found = Collector.Snapshot().FilterByPredicate(
				[&Needle](const FUnrealMcpLogEntry& Entry) { return Entry.Message.Contains(Needle); });
			TestTrue(TEXT("probe line captured"), Found.Num() >= 1);

			Collector.Clear();
			TestEqual(TEXT("buffer empty after clear"), Collector.Num(), 0);

			// Restore the registration state we found (don't leave a dangling GLog device across specs).
			if (!bWasRegistered)
				Collector.Shutdown();
		});
	});

	// The collector behind the RQL `console` source. TEST-LOCAL, never-registered collectors: the specs neither touch the
	// live shared buffer nor depend on what else the editor is logging.
	Describe("console snapshot", [this]()
	{
		auto Feed = [](FUnrealMcpLogCollector& C, const FString& Msg)
		{
			C.Serialize(*Msg, ELogVerbosity::Log, FName(TEXT("LogMcpSpec")));
		};

		It("assigns dense ascending sequences from 1 and keeps the counter across eviction and Clear", [this, Feed]()
		{
			FUnrealMcpLogCollector C;
			for (int32 I = 0; I < 5; ++I) Feed(C, FString::Printf(TEXT("m%d"), I));
			const TArray<FUnrealMcpLogEntry> First = C.Snapshot();
			TestEqual(TEXT("five entries"), First.Num(), 5);
			if (First.Num() == 5)
				TestTrue(TEXT("1..5 in capture order"), First[0].Sequence == 1 && First[4].Sequence == 5);

			const int32 Total = FUnrealMcpLogCollector::MaxEntries + 1;
			for (int32 I = 5; I < Total; ++I) Feed(C, FString::Printf(TEXT("m%d"), I));
			TestTrue(TEXT("capped"), C.Num() <= FUnrealMcpLogCollector::MaxEntries);
			C.Clear();
			Feed(C, TEXT("after"));
			const TArray<FUnrealMcpLogEntry> After = C.Snapshot();
			TestTrue(TEXT("the counter continues after eviction and Clear"), After.Num() == 1 && After[0].Sequence == Total + 1);
		});

		It("returns the most-recent N, oldest first", [this, Feed]()
		{
			FUnrealMcpLogCollector C;
			for (int32 I = 0; I < 5; ++I) Feed(C, FString::Printf(TEXT("r%d"), I));
			const TArray<FUnrealMcpLogEntry> Recent = C.Snapshot(2);
			TestTrue(TEXT("entries 4 and 5"), Recent.Num() == 2 && Recent[0].Sequence == 4 && Recent[1].Sequence == 5);
		});

		It("reads only the entries after a sequence, also after Clear", [this, Feed]()
		{
			FUnrealMcpLogCollector C;
			TestEqual(TEXT("no line yet"), C.GetLastSequence(), (int64)0);
			for (int32 I = 0; I < 5; ++I) Feed(C, FString::Printf(TEXT("a%d"), I));
			const TArray<FUnrealMcpLogEntry> New = C.SnapshotAfter(3);
			TestTrue(TEXT("entries 4 and 5"), New.Num() == 2 && New[0].Sequence == 4 && New[1].Sequence == 5);
			TestEqual(TEXT("everything after 0"), C.SnapshotAfter(0).Num(), 5);
			TestEqual(TEXT("nothing after the last"), C.SnapshotAfter(C.GetLastSequence()).Num(), 0);
			C.Clear();
			Feed(C, TEXT("after"));
			const TArray<FUnrealMcpLogEntry> AfterClear = C.SnapshotAfter(5);
			TestTrue(TEXT("a cursor older than the buffer reads it all"), AfterClear.Num() == 1 && AfterClear[0].Sequence == 6);
			TestEqual(TEXT("last sequence survives Clear"), C.GetLastSequence(), (int64)6);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
