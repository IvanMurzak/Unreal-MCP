// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UI/SUnrealMcpSerializationCheckWindow.h"
#include "Tools/UnrealMcpPropertyJson.h"

#include "GameFramework/Actor.h"

/**
 * Serialization Check window specs (docs/ARCHITECTURE.md §7, Unity's SerializationCheckWindow parity): the
 * HEADLESS-provable core of the window WITHOUT launching Slate — (1) the Recursive=OFF shallow filter
 * (SUnrealMcpSerializationCheckWindow::ShallowFilter, pure JSON→JSON), and (2) that the in-process
 * serialization path the window uses (FUnrealMcpPropertyJson::SerializeObject — the SAME converter the
 * object-get-data / actor-get-data tools use, NOT a sidecar/ReflectorNet round-trip) yields a non-empty
 * reflected object for a real UObject. The Slate window itself (target picker, output panel, Copy flash) is
 * windowed-verified by the operator; these specs lock the serialization contract underneath it.
 *
 * Helpers carry the spec-unique `SerChk` prefix (CLAUDE.md unity-build ODR rule).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpSerializationCheckSpec, "UnrealMcp.SerializationCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Build a full-serialization-shaped object with a mix of scalar and nested fields, the way
	// FJsonObjectConverter emits one for a reflected UObject.
	static TSharedRef<FJsonObject> SerChkMakeMixed()
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), TEXT("Cube"));
		Obj->SetNumberField(TEXT("count"), 3);
		Obj->SetBoolField(TEXT("hidden"), false);

		// A nested object field (e.g. rootComponent) — dropped by the shallow view.
		TSharedRef<FJsonObject> Nested = MakeShared<FJsonObject>();
		Nested->SetNumberField(TEXT("x"), 1.0);
		Obj->SetObjectField(TEXT("rootComponent"), Nested);

		// An array field — also dropped by the shallow view.
		TArray<TSharedPtr<FJsonValue>> Tags;
		Tags.Add(MakeShared<FJsonValueString>(TEXT("a")));
		Obj->SetArrayField(TEXT("tags"), Tags);
		return Obj;
	}

END_DEFINE_SPEC(FUnrealMcpSerializationCheckSpec)

void FUnrealMcpSerializationCheckSpec::Define()
{
	Describe("Shallow filter (Recursive OFF)", [this]()
	{
		It("keeps scalar fields and drops nested objects + arrays", [this]()
		{
			const TSharedRef<FJsonObject> Shallow = SUnrealMcpSerializationCheckWindow::ShallowFilter(SerChkMakeMixed());

			TestTrue("scalar string kept", Shallow->HasField(TEXT("name")));
			TestTrue("scalar number kept", Shallow->HasField(TEXT("count")));
			TestTrue("scalar bool kept", Shallow->HasField(TEXT("hidden")));
			TestFalse("nested object dropped", Shallow->HasField(TEXT("rootComponent")));
			TestFalse("array dropped", Shallow->HasField(TEXT("tags")));
			TestEqual("name value preserved", Shallow->GetStringField(TEXT("name")), FString(TEXT("Cube")));
		});

		It("returns an empty object for a null source (no crash)", [this]()
		{
			const TSharedRef<FJsonObject> Shallow = SUnrealMcpSerializationCheckWindow::ShallowFilter(nullptr);
			TestEqual("empty", Shallow->Values.Num(), 0);
		});
	});

	Describe("In-process serialization path", [this]()
	{
		It("serializes a real UObject to a non-empty reflected object (the window's path)", [this]()
		{
			// Use a transient AActor CDO-free instance is heavy; the transient package + a plain UObject is enough
			// to prove SerializeObject walks reflected FProperties without a sidecar. AActor has reflected props.
			AActor* Actor = NewObject<AActor>(GetTransientPackage());
			TestNotNull("actor created", Actor);
			if (!Actor)
				return;

			const TSharedPtr<FJsonObject> Json = FUnrealMcpPropertyJson::SerializeObject(Actor, /*Paths*/ {});
			TestTrue("json valid", Json.IsValid());
			if (Json.IsValid())
				TestTrue("non-empty reflected object", Json->Values.Num() > 0);
		});

		It("returns an empty object for a null target (no crash)", [this]()
		{
			const TSharedPtr<FJsonObject> Json = FUnrealMcpPropertyJson::SerializeObject(nullptr, /*Paths*/ {});
			TestTrue("json valid", Json.IsValid());
			if (Json.IsValid())
				TestEqual("empty", Json->Values.Num(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
