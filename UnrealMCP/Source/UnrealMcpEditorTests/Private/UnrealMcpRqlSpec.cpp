// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"

#include "Components/PointLightComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * The RQL data layer (docs/rql.md): the rql-* tools, the Unreal reflection provider, the core sources / members /
 * methods and the `editor` operations, executed in EditorContext so GEditor and the editor world are live.
 *
 * The C++ runtime has no RQL text parser (the sidecar parses), so each case carries the canonical QueryPlan of the query
 * in its comment, produced by Rql.Parser (com.IvanMurzak.Rql 0.1.1, PlanCodec.Serialize). Every case works on actors
 * labelled "RqlSpecLamp*" and removes them afterwards.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlSpec, "UnrealMcp.Tools.Rql",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	/** Run a plan; the QueryResult JSON (null when it is not JSON). */
	TSharedPtr<FJsonObject> Exec(const FString& Plan, bool bMutate = false, bool bDryRun = false)
	{
		bool bOk = false;
		const FString Json = UnrealMcpRqlTools::ExecutePlan(Plan, bMutate, bDryRun, bOk);
		TSharedPtr<FJsonObject> Result = Parse(Json);
		if (Result.IsValid())
			TestEqual(TEXT("ok flag matches the result"), Result->GetBoolField(TEXT("ok")), bOk);
		else
			AddError(FString::Printf(TEXT("not JSON: %s"), *Json));
		return Result;
	}

	static TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
	}

	static bool IsOk(const TSharedPtr<FJsonObject>& Result)
	{
		return Result.IsValid() && Result->GetBoolField(TEXT("ok"));
	}

	/** The diagnostics codes of a result, joined (for Contains checks and failure messages). */
	static FString Codes(const TSharedPtr<FJsonObject>& Result)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		if (Result.IsValid() && Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics))
		{
			for (const TSharedPtr<FJsonValue>& Diagnostic : *Diagnostics)
				Out.Add(Diagnostic->AsObject()->GetStringField(TEXT("code")) + TEXT(": ") + Diagnostic->AsObject()->GetStringField(TEXT("message")));
		}
		return FString::Join(Out, TEXT("; "));
	}

	/** rows[Row][Column] as JSON (null when absent). */
	static TSharedPtr<FJsonValue> Cell(const TSharedPtr<FJsonObject>& Result, int32 Row, int32 Column)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("rows"), Rows) || !Rows->IsValidIndex(Row))
			return nullptr;
		const TArray<TSharedPtr<FJsonValue>>& Cells = (*Rows)[Row]->AsArray();
		return Cells.IsValidIndex(Column) ? Cells[Column] : nullptr;
	}

	static int32 RowCount(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		return Result.IsValid() && Result->TryGetArrayField(TEXT("rows"), Rows) ? Rows->Num() : 0;
	}

	static FString CellText(const TSharedPtr<FJsonObject>& Result, int32 Row, int32 Column)
	{
		const TSharedPtr<FJsonValue> Value = Cell(Result, Row, Column);
		if (!Value.IsValid())
			return FString();
		if (Value->Type == EJson::Object)
		{
			FString Ref;
			Value->AsObject()->TryGetStringField(TEXT("$ref"), Ref);
			return Ref;
		}
		FString Text;
		return Value->TryGetString(Text) ? Text : FString();
	}

	static UWorld* World()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	static TArray<AActor*> Lamps()
	{
		TArray<AActor*> Out;
		if (UWorld* W = World())
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				if (IsValid(*It) && It->GetActorLabel().StartsWith(TEXT("RqlSpecLamp")))
					Out.Add(*It);
			}
		}
		return Out;
	}

	static void Cleanup()
	{
		if (GEditor)
			GEditor->SelectNone(false, true);
		for (AActor* Actor : Lamps())
			Actor->GetWorld()->EditorDestroyActor(Actor, true);
	}

	/** editor | call SpawnActor("PointLight", "RqlSpecLamp", "(1, 2, 3)") */
	TSharedPtr<FJsonObject> SpawnLamp()
	{
		return Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLamp"},{"lit":"(1, 2, 3)"}]}]})RQL"), true);
	}

	/** actors | where Label == "RqlSpecLamp" | count */
	int32 CountLamps()
	{
		const TSharedPtr<FJsonObject> Result = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"count"}]})RQL"));
		return Result.IsValid() ? static_cast<int32>(Result->GetNumberField(TEXT("value"))) : -1;
	}

END_DEFINE_SPEC(FUnrealMcpRqlSpec)

void FUnrealMcpRqlSpec::Define()
{
	BeforeEach([this]() { Cleanup(); });
	AfterEach([this]() { Cleanup(); });

	Describe("tools", [this]()
	{
		It("registers rql-query, rql-mutate and rql-schema as standard core tools with the unreal-rql skill body", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpRqlTools::Register(Registry);
			TestEqual(TEXT("three tools"), Registry.Num(), 3);
			const FUnrealMcpRegisteredTool* Query = Registry.Find(TEXT("rql-query"));
			const FUnrealMcpRegisteredTool* Mutate = Registry.Find(TEXT("rql-mutate"));
			const FUnrealMcpRegisteredTool* Schema = Registry.Find(TEXT("rql-schema"));
			if (!TestTrue(TEXT("all registered"), Query && Mutate && Schema))
				return;
			TestTrue(TEXT("query is read-only"), Query->bReadOnlyHint);
			TestTrue(TEXT("mutate is destructive"), Mutate->bDestructiveHint);
			TestTrue(TEXT("standard surface"), Query->ToolType == EUnrealMcpToolType::Standard);
			TestTrue(TEXT("skill body is the RQL guide"), Query->SkillBody.Contains(TEXT("# RQL - query and change the Unreal Editor")));
			TestTrue(TEXT("skill lists the core sources"), Query->SkillBody.Contains(TEXT("| `actors` |")));
			TestTrue(TEXT("descriptor ships the skill body"), Query->ToDescriptorJson()->HasField(TEXT("skillBody")));
			TestFalse(TEXT("other tools ship none"), Schema->ToDescriptorJson()->HasField(TEXT("skillBody")));
		});

		It("answers a call without a parsed plan with an unsupported diagnostic", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpRqlTools::Register(Registry);
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("query"), TEXT("actors | count"));
			const FUnrealMcpToolResult Result = Registry.Execute(TEXT("rql-query"), FUnrealMcpToolCall(Args));
			TestFalse(TEXT("fails"), Result.bSuccess);
			TestTrue(TEXT("says why"), Codes(Parse(Result.Message)).Contains(TEXT("unsupported")));
		});

		It("rejects a malformed plan with invalid-plan", [this]()
		{
			const TSharedPtr<FJsonObject> Result = Exec(TEXT(R"RQL({"rql":"0.1","stages":[]})RQL"));
			TestFalse(TEXT("fails"), IsOk(Result));
			TestTrue(*FString::Printf(TEXT("invalid-plan (%s)"), *Codes(Result)), Codes(Result).Contains(TEXT("invalid-plan")));
		});
	});

	Describe("queries and mutations", [this]()
	{
		It("spawns an actor with a call and reads it back", [this]()
		{
			const TSharedPtr<FJsonObject> Spawned = SpawnLamp();
			TestTrue(*FString::Printf(TEXT("spawn ok (%s)"), *Codes(Spawned)), IsOk(Spawned));
			TestEqual(TEXT("one lamp in the world"), Lamps().Num(), 1);
			TestTrue(TEXT("receipt names the new actor"), CellText(Spawned, 0, 2).StartsWith(TEXT("PointLight#")));

			// actors | where Label == "RqlSpecLamp" | select $ref Label Location
			const TSharedPtr<FJsonObject> Read = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"select","fields":[{"name":"$ref","expr":{"path":[{"meta":"ref"}]}},{"name":"Label","expr":{"path":["Label"]}},{"name":"Location","expr":{"path":["Location"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("query ok (%s)"), *Codes(Read)), IsOk(Read));
			TestEqual(TEXT("one row"), RowCount(Read), 1);
			TestEqual(TEXT("label"), CellText(Read, 0, 1), FString(TEXT("RqlSpecLamp")));
			TestEqual(TEXT("location in the vector text form"), CellText(Read, 0, 2), FString(TEXT("(1, 2, 3)")));
		});

		It("sets members through the editor, and dry_run changes nothing", [this]()
		{
			SpawnLamp();
			TArray<AActor*> Found = Lamps();
			if (!TestEqual(TEXT("spawned"), Found.Num(), 1))
				return;
			AActor* Lamp = Found[0];

			// actors | where Label == "RqlSpecLamp" | set Location = "(-5, 0, 0)"   (dry run)
			const TSharedPtr<FJsonObject> Dry = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Location"]},"value":{"lit":"(-5, 0, 0)"}}]}]})RQL"), true, true);
			TestTrue(*FString::Printf(TEXT("dry run ok (%s)"), *Codes(Dry)), IsOk(Dry));
			TestTrue(TEXT("dry run flagged"), Dry->HasField(TEXT("dryRun")));
			TestTrue(TEXT("dry run left the location"), Lamp->GetActorLocation().Equals(FVector(1, 2, 3)));

			// actors | where Label == "RqlSpecLamp" | set Location = "(10, 20, 30)", HiddenInEditor = true
			const TSharedPtr<FJsonObject> Set = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Location"]},"value":{"lit":"(10, 20, 30)"}},{"target":{"path":["HiddenInEditor"]},"value":{"lit":true}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set ok (%s)"), *Codes(Set)), IsOk(Set));
			TestTrue(TEXT("location written"), Lamp->GetActorLocation().Equals(FVector(10, 20, 30)));
			TestTrue(TEXT("hidden in the editor"), Lamp->IsTemporarilyHiddenInEditor());
			TestTrue(TEXT("level marked dirty"), Lamp->GetOutermost()->IsDirty());
		});

		It("refuses writes in rql-query and to read-only members", [this]()
		{
			SpawnLamp();
			// The set plan above, but as rql-query (no mutation allowed).
			const TSharedPtr<FJsonObject> AsQuery = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Location"]},"value":{"lit":"(-5, 0, 0)"}}]}]})RQL"));
			TestFalse(TEXT("rql-query refuses set"), IsOk(AsQuery));
			TestTrue(*FString::Printf(TEXT("permission-denied (%s)"), *Codes(AsQuery)), Codes(AsQuery).Contains(TEXT("permission-denied")));

			// actors | where Label == "RqlSpecLamp" | set Name = "Renamed"
			const TSharedPtr<FJsonObject> ReadOnly = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Name"]},"value":{"lit":"Renamed"}}]}]})RQL"), true);
			TestFalse(TEXT("Name is read-only"), IsOk(ReadOnly));
			TestTrue(*FString::Printf(TEXT("readonly-member (%s)"), *Codes(ReadOnly)), Codes(ReadOnly).Contains(TEXT("readonly-member")));
		});

		It("walks components and writes reflected component properties", [this]()
		{
			SpawnLamp();
			// actors | where Label == "RqlSpecLamp" | get Components | select $type
			const TSharedPtr<FJsonObject> Types = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"select","fields":[{"name":"$type","expr":{"path":[{"meta":"type"}]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("components ok (%s)"), *Codes(Types)), IsOk(Types));
			bool bHasLight = false;
			for (int32 Row = 0; Row < RowCount(Types); ++Row)
				bHasLight |= CellText(Types, Row, 0) == TEXT("PointLightComponent");
			TestTrue(TEXT("the point light component is listed"), bHasLight);

			// actors | where Label == "RqlSpecLamp" | get Components | where $ is PointLightComponent | set Intensity = 1234
			const TSharedPtr<FJsonObject> Set = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"PointLightComponent"}]}},{"op":"set","assign":[{"target":{"path":["Intensity"]},"value":{"lit":1234}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set ok (%s)"), *Codes(Set)), IsOk(Set));
			TArray<AActor*> Found = Lamps();
			const APointLight* Lamp = Found.Num() == 1 ? Cast<APointLight>(Found[0]) : nullptr;
			TestTrue(TEXT("intensity written"), Lamp && Lamp->PointLightComponent && FMath::IsNearlyEqual(Lamp->PointLightComponent->Intensity, 1234.0f));

			// ... | select Intensity
			const TSharedPtr<FJsonObject> Read = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"PointLightComponent"}]}},{"op":"select","fields":[{"name":"Intensity","expr":{"path":["Intensity"]}}]}]})RQL"));
			const TSharedPtr<FJsonValue> Intensity = Cell(Read, 0, 0);
			TestTrue(TEXT("intensity reads back"), Intensity.IsValid() && FMath::IsNearlyEqual(Intensity->AsNumber(), 1234.0));
		});

		It("adds components, duplicates and destroys actors", [this]()
		{
			SpawnLamp();
			// actors | where Label == "RqlSpecLamp" | call AddComponent("StaticMeshComponent", "RqlSpecMesh")
			const TSharedPtr<FJsonObject> Added = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"call","method":"AddComponent","args":[{"lit":"StaticMeshComponent"},{"lit":"RqlSpecMesh"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("add ok (%s)"), *Codes(Added)), IsOk(Added));
			TArray<AActor*> Found = Lamps();
			TestTrue(TEXT("component added"), Found.Num() == 1 && Found[0]->GetInstanceComponents().Num() == 1);

			// actors | where Label == "RqlSpecLamp" | call Duplicate("(100, 0, 0)")
			const TSharedPtr<FJsonObject> Copied = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"call","method":"Duplicate","args":[{"lit":"(100, 0, 0)"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("duplicate ok (%s)"), *Codes(Copied)), IsOk(Copied));
			TestEqual(TEXT("two lamps"), Lamps().Num(), 2);

			// actors | where Label == "RqlSpecLamp" | call Destroy()
			const TSharedPtr<FJsonObject> Destroyed = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"call","method":"Destroy","args":[]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("destroy ok (%s)"), *Codes(Destroyed)), IsOk(Destroyed));
			TestEqual(TEXT("the original is gone"), CountLamps(), 0);
		});

		It("resolves stable refs and unreal.find", [this]()
		{
			SpawnLamp();
			TArray<AActor*> Found = Lamps();
			if (!TestEqual(TEXT("spawned"), Found.Num(), 1))
				return;
			// unreal.find("RqlSpecLamp") | select Label
			const TSharedPtr<FJsonObject> ByName = Exec(TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSpecLamp"}]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"));
			TestEqual(*FString::Printf(TEXT("unreal.find (%s)"), *Codes(ByName)), CellText(ByName, 0, 0), FString(TEXT("RqlSpecLamp")));
			// ref("PointLight#<id>") | select Label
			const FString RefPlan = FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"PointLight#%u"}]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"), Found[0]->GetUniqueID());
			const TSharedPtr<FJsonObject> ByRef = Exec(RefPlan);
			TestEqual(*FString::Printf(TEXT("ref (%s)"), *Codes(ByRef)), CellText(ByRef, 0, 0), FString(TEXT("RqlSpecLamp")));
		});

		It("writes and reads the editor selection", [this]()
		{
			SpawnLamp();
			// editor | set Selection = [unreal.find("RqlSpecLamp")]
			const TSharedPtr<FJsonObject> Set = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"set","assign":[{"target":{"path":["Selection"]},"value":{"list":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSpecLamp"}]}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("select ok (%s)"), *Codes(Set)), IsOk(Set));
			TestEqual(TEXT("one actor selected"), GEditor ? GEditor->GetSelectedActorCount() : 0, 1);
			// editor | select Selection
			const TSharedPtr<FJsonObject> Read = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"select","fields":[{"name":"Selection","expr":{"path":["Selection"]}}]}]})RQL"));
			const TSharedPtr<FJsonValue> Selection = Cell(Read, 0, 0);
			TestTrue(TEXT("selection reads back"), Selection.IsValid() && Selection->Type == EJson::Array && Selection->AsArray().Num() == 1);
		});

		It("lists levels, console entries and assets", [this]()
		{
			// levels | select Map IsPersistent IsCurrent
			const TSharedPtr<FJsonObject> Levels = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["levels"]},"stages":[{"op":"select","fields":[{"name":"Map","expr":{"path":["Map"]}},{"name":"IsPersistent","expr":{"path":["IsPersistent"]}},{"name":"IsCurrent","expr":{"path":["IsCurrent"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("levels ok (%s)"), *Codes(Levels)), IsOk(Levels));
			TestTrue(TEXT("the persistent level is listed"), RowCount(Levels) >= 1);

			FUnrealMcpLogCollector& Collector = FUnrealMcpLogCollector::Get();
			const bool bWasRegistered = Collector.IsRegistered();
			Collector.Startup();
			UE_LOG(LogTemp, Display, TEXT("RqlSpecMarker console line"));
			GLog->Flush();
			// console | where Message contains "RqlSpecMarker" | select Verbosity Message
			const TSharedPtr<FJsonObject> Console = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["console"]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Message"]},{"lit":"RqlSpecMarker"}]}},{"op":"select","fields":[{"name":"Verbosity","expr":{"path":["Verbosity"]}},{"name":"Message","expr":{"path":["Message"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("console ok (%s)"), *Codes(Console)), IsOk(Console) && RowCount(Console) >= 1);
			if (!bWasRegistered)
				Collector.Shutdown();

			// unreal.asset("/Engine/EngineMaterials/DefaultMaterial") | select Name Scalars
			const TSharedPtr<FJsonObject> Asset = Exec(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Engine/EngineMaterials/DefaultMaterial"}]},"stages":[{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Scalars","expr":{"path":["Scalars"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("asset ok (%s)"), *Codes(Asset)), IsOk(Asset));
			TestEqual(TEXT("asset name"), CellText(Asset, 0, 0), FString(TEXT("DefaultMaterial")));
		});
	});

	Describe("schema", [this]()
	{
		It("lists sources, functions, present classes and the Actor members", [this]()
		{
			bool bOk = false;
			const TSharedPtr<FJsonObject> Schema = Parse(UnrealMcpRqlTools::Schema(FString(), bOk));
			TestTrue(TEXT("ok"), bOk && Schema.IsValid());
			if (!Schema.IsValid())
				return;
			TArray<FString> Sources;
			for (const TSharedPtr<FJsonValue>& Source : Schema->GetArrayField(TEXT("sources")))
				Sources.Add(Source->AsObject()->GetStringField(TEXT("name")));
			for (const TCHAR* Expected : { TEXT("actors"), TEXT("levels"), TEXT("world"), TEXT("selection"), TEXT("editor"), TEXT("console") })
				TestTrue(FString::Printf(TEXT("source %s"), Expected), Sources.Contains(Expected));
			TestTrue(TEXT("functions listed"), Schema->GetArrayField(TEXT("functions")).Num() >= 4);
			const TArray<TSharedPtr<FJsonValue>>& Types = Schema->GetArrayField(TEXT("types"));
			TestTrue(TEXT("Actor first"), Types.Num() >= 1 && Types[0]->AsObject()->GetObjectField(TEXT("type"))->GetStringField(TEXT("name")) == TEXT("Actor"));
		});

		It("describes one type with its shared base, and reports unknown types", [this]()
		{
			bool bOk = false;
			const TSharedPtr<FJsonObject> Light = Parse(UnrealMcpRqlTools::Schema(TEXT("PointLight"), bOk));
			TestTrue(TEXT("ok"), bOk && Light.IsValid());
			if (Light.IsValid())
			{
				const TSharedPtr<FJsonObject> Entry = Light->GetArrayField(TEXT("types"))[0]->AsObject();
				TestEqual(TEXT("type"), Entry->GetObjectField(TEXT("type"))->GetStringField(TEXT("name")), FString(TEXT("PointLight")));
				TestEqual(TEXT("shared base"), Entry->GetObjectField(TEXT("shared"))->GetStringField(TEXT("name")), FString(TEXT("Actor")));
			}
			const TSharedPtr<FJsonObject> Vector = Parse(UnrealMcpRqlTools::Schema(TEXT("FVector"), bOk));
			TestTrue(TEXT("C++ spelling resolves the struct"), bOk && Vector.IsValid());

			const TSharedPtr<FJsonObject> Unknown = Parse(UnrealMcpRqlTools::Schema(TEXT("NoSuchRqlSpecType"), bOk));
			TestFalse(TEXT("unknown fails"), bOk);
			TestTrue(TEXT("unknown-type"), Unknown.IsValid() && Unknown->GetStringField(TEXT("error")) == TEXT("unknown-type"));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
