// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"

/** Tool registry + ping specs (docs/ARCHITECTURE.md §2.2, §3.3, §10). */
BEGIN_DEFINE_SPEC(FUnrealMcpToolRegistrySpec, "UnrealMcp.Tools.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpToolRegistrySpec)

void FUnrealMcpToolRegistrySpec::Define()
{
	Describe("ping registration", [this]()
	{
		It("registers the ping tool and bumps the revision", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpPingTool::Register(Registry);
			TestTrue(TEXT("has ping"), Registry.HasTool(TEXT("ping")));
			TestEqual(TEXT("one tool"), Registry.Num(), 1);
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);
		});

		It("executes ping with no args -> pong (structured result)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpPingTool::Register(Registry);

			FUnrealMcpToolCall Call;
			const FUnrealMcpToolResult Result = Registry.Execute(TEXT("ping"), Call);
			TestTrue(TEXT("success"), Result.bSuccess);
			TestEqual(TEXT("message"), Result.Message, FString(TEXT("pong")));
			TestTrue(TEXT("structured present"), Result.Structured.IsValid());
			FString StructResult;
			TestTrue(TEXT("has result field"), Result.Structured->TryGetStringField(TEXT("result"), StructResult));
			TestEqual(TEXT("result is pong"), StructResult, FString(TEXT("pong")));
		});

		It("echoes the message arg", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpPingTool::Register(Registry);

			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("message"), TEXT("hello-unreal"));
			FUnrealMcpToolCall Call(Args);

			const FUnrealMcpToolResult Result = Registry.Execute(TEXT("ping"), Call);
			TestEqual(TEXT("echoed"), Result.Message, FString(TEXT("hello-unreal")));
		});

		It("returns an error for an unknown tool", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const FUnrealMcpToolResult Result = Registry.Execute(TEXT("does-not-exist"), FUnrealMcpToolCall());
			TestFalse(TEXT("not success"), Result.bSuccess);
		});
	});

	Describe("manifest JSON", [this]()
	{
		It("contains a revision and the ping descriptor with a schema hash", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpPingTool::Register(Registry);

			TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
			TestEqual(TEXT("type"), Manifest->GetStringField(TEXT("type")), FString(TEXT("tool-manifest")));
			TestEqual(TEXT("revision"), (int32)Manifest->GetNumberField(TEXT("revision")), Registry.GetRevision());

			const TArray<TSharedPtr<FJsonValue>>* Tools;
			TestTrue(TEXT("tools array"), Manifest->TryGetArrayField(TEXT("tools"), Tools));
			TestEqual(TEXT("one tool"), Tools->Num(), 1);

			TSharedPtr<FJsonObject> Ping = (*Tools)[0]->AsObject();
			TestEqual(TEXT("name"), Ping->GetStringField(TEXT("name")), FString(TEXT("ping")));
			TestTrue(TEXT("has input schema"), Ping->HasField(TEXT("inputSchema")));
			TestFalse(TEXT("schema hash non-empty"), Ping->GetStringField(TEXT("schemaHash")).IsEmpty());
			TestTrue(TEXT("readOnlyHint true"), Ping->GetBoolField(TEXT("readOnlyHint")));
		});

		It("produces a stable schema hash for an unchanged tool", [this]()
		{
			FUnrealMcpToolRegistry A, B;
			UnrealMcpPingTool::Register(A);
			UnrealMcpPingTool::Register(B);
			TestEqual(TEXT("same hash"),
				A.Find(TEXT("ping"))->SchemaHash, B.Find(TEXT("ping"))->SchemaHash);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
