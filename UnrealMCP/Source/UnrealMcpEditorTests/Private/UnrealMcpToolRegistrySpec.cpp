// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpRuntimeCoreTools.h" // UnrealMcpPingTool::Register moved to the runtime module (§12.1)
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

		It("folds structured content into the wire text unless the message covers it", [this]()
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("content"), TEXT("int32 X;"));
			FUnrealMcpToolResult Result = FUnrealMcpToolResult::Success(TEXT("Read 'A.h' (1 line(s))."), Data);
			TestEqual(TEXT("summary + condensed JSON"), Result.WireText(), FString(TEXT("Read 'A.h' (1 line(s)).\n{\"content\":\"int32 X;\"}")));
			TestEqual(TEXT("the message itself is unchanged"), Result.Message, FString(TEXT("Read 'A.h' (1 line(s)).")));
			Result.bMessageCoversStructured = true;
			TestEqual(TEXT("covered: message only"), Result.WireText(), Result.Message);
			TestEqual(TEXT("no structured: message only"), FUnrealMcpToolResult::Success(TEXT("ok")).WireText(), FString(TEXT("ok")));
			TestEqual(TEXT("empty message: JSON only"), FUnrealMcpToolResult::Success(FString(), Data).WireText(), FString(TEXT("{\"content\":\"int32 X;\"}")));
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

		It("refuses a call missing a required parameter before the handler runs, naming it and what was sent", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			bool bHandlerRan = false;
			Registry.Tool(TEXT("registry-spec-required"))
				.ParamString(TEXT("path"), TEXT("asset path"), EUnrealMcpParamRequirement::Required)
				.ParamString(TEXT("name"), TEXT("optional name"), EUnrealMcpParamRequirement::Optional)
				.Handle([&bHandlerRan](const FUnrealMcpToolCall&) { bHandlerRan = true; return FUnrealMcpToolResult::Success(TEXT("ran")); });

			TSharedPtr<FJsonObject> Misspelled = MakeShared<FJsonObject>();
			Misspelled->SetStringField(TEXT("blueprint"), TEXT("/Game/BP"));
			const FUnrealMcpToolResult Missing = Registry.Execute(TEXT("registry-spec-required"), FUnrealMcpToolCall(Misspelled));
			TestFalse(TEXT("refused"), Missing.bSuccess);
			TestFalse(TEXT("the handler did not run"), bHandlerRan);
			TestTrue(*FString::Printf(TEXT("names the parameter, the declared ones and the sent ones (%s)"), *Missing.Message),
				Missing.Message.Contains(TEXT("Missing required 'path'")) && Missing.Message.Contains(TEXT("parameters: path, name"))
				&& Missing.Message.Contains(TEXT("sent: blueprint")));

			TSharedPtr<FJsonObject> Null = MakeShared<FJsonObject>();
			Null->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
			TestTrue(TEXT("a JSON null counts as missing"), Registry.Execute(TEXT("registry-spec-required"), FUnrealMcpToolCall(Null)).Message.Contains(TEXT("Missing required 'path'")));
			TestFalse(TEXT("the handler still did not run"), bHandlerRan);

			TSharedPtr<FJsonObject> Given = MakeShared<FJsonObject>();
			Given->SetStringField(TEXT("path"), TEXT("/Game/BP"));
			TestTrue(TEXT("runs with the required parameter (optional ones may be absent)"),
				Registry.Execute(TEXT("registry-spec-required"), FUnrealMcpToolCall(Given)).bSuccess && bHandlerRan);
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
			// §7 SKILL.md front-matter source: the manifest carries a dedicated short skillDescription. Ping
			// declares no explicit one, so it falls back to the Title ("Ping") — NOT a truncation of the full
			// Description. The sidecar generator uses this for the YAML `description:`.
			TestEqual(TEXT("skillDescription falls back to Title"), Ping->GetStringField(TEXT("skillDescription")), FString(TEXT("Ping")));
		});

		It("carries an explicit skillDescription when a tool declares one", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			// The fluent builder commits the tool to the registry ONLY on Handle() — a tool with no bound
			// handler is never registered (and ValidateTool rejects it), so the manifest would be empty and
			// (*Tools)[0] would assert. Bind a trivial handler so demo-skill actually lands in the manifest.
			Registry.Tool(TEXT("demo-skill"))
				.Title(TEXT("Demo Skill"))
				.Description(TEXT("A long full description that becomes the SKILL.md body, not the front-matter."))
				.SkillDescription(TEXT("Short front-matter line."))
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });

			TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			// Guard the deref so any future regression surfaces as a clean test FAILURE rather than an
			// out-of-bounds assert that aborts the whole editor run (and drops the index.json report).
			if (TestTrue(TEXT("tools array"), Manifest->TryGetArrayField(TEXT("tools"), Tools))
				&& TestEqual(TEXT("one tool"), Tools->Num(), 1))
			{
				TSharedPtr<FJsonObject> Demo = (*Tools)[0]->AsObject();
				TestEqual(TEXT("explicit skillDescription"), Demo->GetStringField(TEXT("skillDescription")), FString(TEXT("Short front-matter line.")));
				// The full description is still carried separately (becomes the body).
				TestTrue(TEXT("full description present"), Demo->GetStringField(TEXT("description")).Contains(TEXT("SKILL.md body")));
			}
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
