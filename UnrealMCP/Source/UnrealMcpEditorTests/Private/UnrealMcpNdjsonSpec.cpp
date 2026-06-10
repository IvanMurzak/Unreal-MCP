// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Bridge/UnrealMcpNdjson.h"

/** NDJSON framing codec specs (docs/ARCHITECTURE.md §1.2, §9.3 — the C++ peer of the sidecar tests). */
BEGIN_DEFINE_SPEC(FUnrealMcpNdjsonSpec, "UnrealMcp.Ipc.Ndjson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static TArray<FString> PushString(FUnrealMcpNdjsonAccumulator& Acc, const FString& Text, bool& bOk)
	{
		FTCHARToUTF8 Utf8(*Text);
		TArray<FString> Lines;
		bOk = Acc.Push(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Lines);
		return Lines;
	}

END_DEFINE_SPEC(FUnrealMcpNdjsonSpec)

void FUnrealMcpNdjsonSpec::Define()
{
	Describe("Encode", [this]()
	{
		It("appends a single trailing newline", [this]()
		{
			const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(TEXT("{\"type\":\"ping\"}"));
			TestEqual(TEXT("last byte is newline"), (int32)Framed.Last(), (int32)'\n');
		});
	});

	Describe("Push", [this]()
	{
		It("splits multiple lines in one chunk", [this]()
		{
			FUnrealMcpNdjsonAccumulator Acc;
			bool bOk = false;
			const TArray<FString> Lines = PushString(Acc, TEXT("a\nb\nc\n"), bOk);
			TestTrue(TEXT("ok"), bOk);
			TestEqual(TEXT("count"), Lines.Num(), 3);
			TestEqual(TEXT("first"), Lines[0], FString(TEXT("a")));
			TestEqual(TEXT("third"), Lines[2], FString(TEXT("c")));
		});

		It("buffers a partial line across chunks", [this]()
		{
			FUnrealMcpNdjsonAccumulator Acc;
			bool bOk = false;
			TestEqual(TEXT("no line yet"), PushString(Acc, TEXT("{\"par"), bOk).Num(), 0);
			TestEqual(TEXT("still none"), PushString(Acc, TEXT("tial\":1"), bOk).Num(), 0);
			const TArray<FString> Lines = PushString(Acc, TEXT("}\n"), bOk);
			TestEqual(TEXT("one line"), Lines.Num(), 1);
			TestEqual(TEXT("joined"), Lines[0], FString(TEXT("{\"partial\":1}")));
		});

		It("tolerates a trailing carriage return", [this]()
		{
			FUnrealMcpNdjsonAccumulator Acc;
			bool bOk = false;
			const TArray<FString> Lines = PushString(Acc, TEXT("hello\r\n"), bOk);
			TestEqual(TEXT("one line"), Lines.Num(), 1);
			TestEqual(TEXT("cr stripped"), Lines[0], FString(TEXT("hello")));
		});

		It("aborts when a line exceeds the cap", [this]()
		{
			FUnrealMcpNdjsonAccumulator Acc(4);
			bool bOk = true;
			PushString(Acc, TEXT("abcdefgh"), bOk);
			TestFalse(TEXT("push reports abort"), bOk);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
