// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"

#include "Components/AudioComponent.h"
#include "Components/LODSyncComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/LevelStreamingVolume.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "EditorLevelUtils.h"
#include "Engine/Level.h"
#include "Layers/LayersSubsystem.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UObject/UObjectGlobals.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * The RQL data layer (docs/rql.md): the rql-* tools, the Unreal reflection provider, the core sources / members /
 * methods and the `editor` operations, executed in EditorContext so GEditor and the editor world are live.
 *
 * The C++ runtime has no RQL text parser (the sidecar parses), so each case carries the canonical QueryPlan of the query
 * in its comment, produced by Rql.Parser (com.IvanMurzak.Rql 0.2.0, PlanCodec.Serialize). Every case works on actors
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
		It("registers rql-query, rql-mutate and rql-schema as standard core tools; rql-query ships the unreal-rql skill", [this]()
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
			TestEqual(TEXT("rql roles"), Query->Rql + Mutate->Rql + Schema->Rql, FString(TEXT("querymutateschema")));
			if (!TestEqual(TEXT("one skill"), Query->Skills.Num(), 1))
				return;
			TestEqual(TEXT("skill name"), Query->Skills[0].Name, FString(TEXT("unreal-rql")));
			TestTrue(TEXT("skill is the RQL guide"), Query->Skills[0].Markdown.Contains(TEXT("# RQL - query and change the Unreal Editor")));
			TestTrue(TEXT("skill lists the core sources"), Query->Skills[0].Markdown.Contains(TEXT("| `actors` |")));
			const TSharedPtr<FJsonObject> Descriptor = Query->ToDescriptorJson();
			TestTrue(TEXT("descriptor ships the skill and the role"), Descriptor->HasField(TEXT("skills")) && Descriptor->GetStringField(TEXT("rql")) == TEXT("query"));
			TestFalse(TEXT("other tools ship no skill"), Schema->ToDescriptorJson()->HasField(TEXT("skills")));
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

		It("duplicates an actor at its own world transform plus the offset", [this]()
		{
			SpawnLamp();
			TArray<AActor*> Found = Lamps();
			if (!TestEqual(TEXT("one lamp"), Found.Num(), 1))
				return;
			// A root transform that is not identity: SpawnActor with a template would apply it twice.
			Found[0]->SetActorLocationAndRotation(FVector(0, 0, 150), FRotator(0, 45, 0));
			Found[0]->SetActorScale3D(FVector(2, 2, 0.5));
			// actors | where Label == "RqlSpecLamp" | call Duplicate("(300, 0, 0)")
			const TSharedPtr<FJsonObject> Copied = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"call","method":"Duplicate","args":[{"lit":"(300, 0, 0)"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("duplicate ok (%s)"), *Codes(Copied)), IsOk(Copied));
			AActor* Copy = nullptr;
			for (AActor* Actor : Lamps())
				Copy = Actor != Found[0] ? Actor : Copy;
			if (!TestNotNull(TEXT("the copy"), Copy))
				return;
			TestTrue(*FString::Printf(TEXT("location + offset (%s)"), *Copy->GetActorLocation().ToString()), Copy->GetActorLocation().Equals(FVector(300, 0, 150), 0.01));
			TestTrue(*FString::Printf(TEXT("same scale (%s)"), *Copy->GetActorScale3D().ToString()), Copy->GetActorScale3D().Equals(FVector(2, 2, 0.5), 0.001));
			TestTrue(*FString::Printf(TEXT("same rotation (%s)"), *Copy->GetActorRotation().ToString()), Copy->GetActorRotation().Equals(FRotator(0, 45, 0), 0.01));
		});

		It("spawns with the given rotation as the final one, and keeps the class default without one", [this]()
		{
			// editor | call SpawnActor("SpotLight", "RqlSpecLampSpot", "(0, 0, 300)", "(-90, 0, 0)")
			const TSharedPtr<FJsonObject> Rotated = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"SpotLight"},{"lit":"RqlSpecLampSpot"},{"lit":"(0, 0, 300)"},{"lit":"(-90, 0, 0)"}]}]})RQL"), true);
			// editor | call SpawnActor("SpotLight", "RqlSpecLampSpotDefault", "(0, 0, 300)")
			const TSharedPtr<FJsonObject> Default = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"SpotLight"},{"lit":"RqlSpecLampSpotDefault"},{"lit":"(0, 0, 300)"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("spawned (%s %s)"), *Codes(Rotated), *Codes(Default)), IsOk(Rotated) && IsOk(Default));
			TestEqual(TEXT("two spot lights"), Lamps().Num(), 2);
			for (AActor* Actor : Lamps())
			{
				// Both point down: the given -90 pitch, and the SpotLight's own default.
				TestTrue(*FString::Printf(TEXT("%s points down (%s)"), *Actor->GetActorLabel(), *Actor->GetActorForwardVector().ToString()),
					Actor->GetActorForwardVector().Equals(FVector(0, 0, -1), 0.001));
			}
		});

		It("writes Layers through the Layers subsystem: missing layers are created, removed ones leave the actor, undo restores", [this]()
		{
			SpawnLamp();
			TArray<AActor*> Found = Lamps();
			ULayersSubsystem* LayersSubsystem = GEditor ? GEditor->GetEditorSubsystem<ULayersSubsystem>() : nullptr;
			if (!TestEqual(TEXT("one lamp"), Found.Num(), 1) || !TestNotNull(TEXT("layers subsystem"), LayersSubsystem))
				return;
			AActor* Lamp = Found[0];
			// A raw name already on the actor (what a plain property write used to leave) gets its layer created too.
			Lamp->Layers.Add(TEXT("RqlSpecLayerA"));
			// actors | where Label == "RqlSpecLamp" | set Layers = ["RqlSpecLayerA", "RqlSpecLayerB"]
			const TSharedPtr<FJsonObject> Both = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Layers"]},"value":{"list":[{"lit":"RqlSpecLayerA"},{"lit":"RqlSpecLayerB"}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set (%s)"), *Codes(Both)), IsOk(Both));
			TestTrue(TEXT("layer A exists in the world (the Layers panel lists it)"), LayersSubsystem->IsLayer(TEXT("RqlSpecLayerA")));
			TestTrue(TEXT("layer B exists"), LayersSubsystem->IsLayer(TEXT("RqlSpecLayerB")));
			TestTrue(TEXT("the actor is in layer A"), LayersSubsystem->GetActorsFromLayer(TEXT("RqlSpecLayerA")).Contains(Lamp));
			// actors | where Label == "RqlSpecLamp" | set Layers = ["RqlSpecLayerB"]
			const TSharedPtr<FJsonObject> OnlyB = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Layers"]},"value":{"list":[{"lit":"RqlSpecLayerB"}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("replaced (%s)"), *Codes(OnlyB)), IsOk(OnlyB));
			TestFalse(TEXT("left layer A"), LayersSubsystem->GetActorsFromLayer(TEXT("RqlSpecLayerA")).Contains(Lamp));
			TestTrue(TEXT("still in layer B"), LayersSubsystem->GetActorsFromLayer(TEXT("RqlSpecLayerB")).Contains(Lamp));
			// actors | where Label == "RqlSpecLamp" | select Layers
			const TSharedPtr<FJsonObject> Read = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"select","fields":[{"name":"Layers","expr":{"path":["Layers"]}}]}]})RQL"));
			const TSharedPtr<FJsonValue> Names = Cell(Read, 0, 0);
			TestTrue(*FString::Printf(TEXT("reads the names (%s)"), *Codes(Read)), Names.IsValid() && Names->Type == EJson::Array
				&& Names->AsArray().Num() == 1 && Names->AsArray()[0]->AsString() == TEXT("RqlSpecLayerB"));
			// actors | where Label == "RqlSpecLamp" | set Layers = [""]
			const TSharedPtr<FJsonObject> Empty = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Layers"]},"value":{"list":[{"lit":""}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("an empty name is refused (%s)"), *Codes(Empty)), !IsOk(Empty) && Codes(Empty).Contains(TEXT("cannot be empty")));
			GEditor->UndoTransaction();
			TestTrue(TEXT("undo puts the actor back in layer A"), Lamp->Layers.Contains(FName(TEXT("RqlSpecLayerA"))) && Lamp->Layers.Contains(FName(TEXT("RqlSpecLayerB"))));
			LayersSubsystem->DeleteLayer(TEXT("RqlSpecLayerA"));
			LayersSubsystem->DeleteLayer(TEXT("RqlSpecLayerB"));
		});

		It("reads Folder as null without a folder, and compares and prints transforms cleanly", [this]()
		{
			SpawnLamp();
			TArray<AActor*> Found = Lamps();
			if (!TestEqual(TEXT("one lamp"), Found.Num(), 1))
				return;
			// A quaternion round trip leaves double noise (-46.000000000000036) in the rotator.
			Found[0]->SetActorRotation(FQuat(FRotator(-46, 0, 0)));
			// actors | where Label == "RqlSpecLamp" | select Folder Rotation
			const TSharedPtr<FJsonObject> Read = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"select","fields":[{"name":"Folder","expr":{"path":["Folder"]}},{"name":"Rotation","expr":{"path":["Rotation"]}}]}]})RQL"));
			const TSharedPtr<FJsonValue> Folder = Cell(Read, 0, 0);
			TestTrue(*FString::Printf(TEXT("folder null (%s)"), *Codes(Read)), Folder.IsValid() && Folder->IsNull());
			TestEqual(TEXT("rotation without noise"), CellText(Read, 0, 1), FString(TEXT("(-46, 0, 0)")));
			// actors | where Label == "RqlSpecLamp" and Folder is null | count
			const TSharedPtr<FJsonObject> NoFolder = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"and","args":[{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]},{"op":"==","args":[{"path":["Folder"]},{"lit":null}]}]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("is null finds it"), UnrealMcpRqlSpecHelpers::Number(NoFolder), 1.0);
			// actors | where Label == "RqlSpecLamp" and Location == {X: 1, Y: 2, Z: 3} | count
			const TSharedPtr<FJsonObject> ByObject = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"and","args":[{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]},{"op":"==","args":[{"path":["Location"]},{"object":[{"key":"X","value":{"lit":1}},{"key":"Y","value":{"lit":2}},{"key":"Z","value":{"lit":3}}]}]}]}},{"op":"count"}]})RQL"));
			TestEqual(*FString::Printf(TEXT("Location == object literal (%s)"), *Codes(ByObject)), UnrealMcpRqlSpecHelpers::Number(ByObject), 1.0);
		});

		It("refuses deleting an asset unsaved assets use, and names abstract classes and existing imports", [this]()
		{
			// A fresh folder per run: dropped assets stay in the asset registry until the editor restarts.
			const FString Folder = FString::Printf(TEXT("/Game/RqlSpecDelete/Run%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			auto Plan = [&Folder](const TCHAR* Json) { return FString(Json).Replace(TEXT("/Game/RqlSpecDelete"), *Folder); };
			auto Drop = [&Folder]()
			{
				for (const TCHAR* Name : { TEXT("MI_Child"), TEXT("M_Base"), TEXT("RqlSpecPixel") })
					UnrealMcpRqlSpecHelpers::DropObject(FindObject<UObject>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), *Folder, Name, Name)));
			};
			// editor | call CreateAsset("/Game/RqlSpecDelete/M_Base", "Material")
			const TSharedPtr<FJsonObject> Base = Exec(Plan(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateAsset","args":[{"lit":"/Game/RqlSpecDelete/M_Base"},{"lit":"Material"}]}]})RQL")), true);
			// editor | call CreateMaterialInstance("/Game/RqlSpecDelete/MI_Child", "/Game/RqlSpecDelete/M_Base")
			const TSharedPtr<FJsonObject> Child = Exec(Plan(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateMaterialInstance","args":[{"lit":"/Game/RqlSpecDelete/MI_Child"},{"lit":"/Game/RqlSpecDelete/M_Base"}]}]})RQL")), true);
			TestTrue(*FString::Printf(TEXT("created (%s %s)"), *Codes(Base), *Codes(Child)), IsOk(Base) && IsOk(Child));
			// editor | call DeleteAsset("/Game/RqlSpecDelete/M_Base")
			const TSharedPtr<FJsonObject> Deleted = Exec(Plan(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"DeleteAsset","args":[{"lit":"/Game/RqlSpecDelete/M_Base"}]}]})RQL")), true);
			TestFalse(TEXT("delete refused"), IsOk(Deleted));
			TestTrue(*FString::Printf(TEXT("names the unsaved referencer (%s)"), *Codes(Deleted)), Codes(Deleted).Contains(Folder + TEXT("/MI_Child")));
			TestNotNull(TEXT("the material is still there"), FindObject<UObject>(nullptr, *(Folder + TEXT("/M_Base.M_Base"))));

			// editor | call CreateAsset("/Game/RqlSpecDelete/DA_Abstract", "PrimaryDataAsset")
			const TSharedPtr<FJsonObject> Abstract = Exec(Plan(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateAsset","args":[{"lit":"/Game/RqlSpecDelete/DA_Abstract"},{"lit":"PrimaryDataAsset"}]}]})RQL")), true);
			TestTrue(*FString::Printf(TEXT("abstract named (%s)"), *Codes(Abstract)), !IsOk(Abstract) && Codes(Abstract).Contains(TEXT("is abstract")));

			// A 1x1 PNG, imported twice: the second time it exists.
			static const uint8 Png[] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
				0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,0xDE,0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,0x00,0x03,0x01,0x01,
				0x00,0x18,0xDD,0x8D,0xB0,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82 };
			const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("RqlSpec"), TEXT("RqlSpecPixel.png")));
			TestTrue(TEXT("png written"), FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Png, UE_ARRAY_COUNT(Png)), *File));
			// editor | call ImportAsset("@FILE@", "/Game/RqlSpecDelete")
			const FString Import = Plan(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"ImportAsset","args":[{"lit":"@FILE@"},{"lit":"/Game/RqlSpecDelete"}]}]})RQL")).Replace(TEXT("@FILE@"), *File);
			const TSharedPtr<FJsonObject> First = Exec(Import, true);
			TestTrue(*FString::Printf(TEXT("first import (%s)"), *Codes(First)), IsOk(First));
			const TSharedPtr<FJsonObject> Second = Exec(Import, true);
			TestTrue(*FString::Printf(TEXT("second import says it exists (%s)"), *Codes(Second)), !IsOk(Second) && Codes(Second).Contains(TEXT("already exists")));
			IFileManager::Get().Delete(*File, false, true);
			Drop();
		});

		It("accepts object references as text (an actor label for an Actor parameter)", [this]()
		{
			SpawnLamp();
			// editor | call SpawnActor("PointLight", "RqlSpecLamp2", "(0, 0, 0)", "(0, 0, 0)", "RqlSpecLamp")
			const TSharedPtr<FJsonObject> Child = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLamp2"},{"lit":"(0, 0, 0)"},{"lit":"(0, 0, 0)"},{"lit":"RqlSpecLamp"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("spawn with a parent ok (%s)"), *Codes(Child)), IsOk(Child));
			AActor* Parent = nullptr;
			AActor* Attached = nullptr;
			for (AActor* Actor : Lamps())
			{
				if (Actor->GetActorLabel() == TEXT("RqlSpecLamp")) Parent = Actor;
				if (Actor->GetActorLabel() == TEXT("RqlSpecLamp2")) Attached = Actor;
			}
			TestTrue(TEXT("attached to the actor named by its label"), Parent && Attached && Attached->GetAttachParentActor() == Parent);
		});

		It("gives a spawned class without a root component a scene root, so it keeps its location and can be a parent", [this]()
		{
			// editor | call SpawnActor("Actor", "RqlSpecLampRig", "(100, 200, 0)")
			const TSharedPtr<FJsonObject> Rig = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"Actor"},{"lit":"RqlSpecLampRig"},{"lit":"(100, 200, 0)"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("spawn a plain Actor ok (%s)"), *Codes(Rig)), IsOk(Rig));
			// editor | call SpawnActor("PointLight", "RqlSpecLamp", "(0, 0, 0)", "(0, 0, 0)", unreal.find("RqlSpecLampRig"))
			const TSharedPtr<FJsonObject> Child = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLamp"},{"lit":"(0, 0, 0)"},{"lit":"(0, 0, 0)"},{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSpecLampRig"}]}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("spawn under it ok (%s)"), *Codes(Child)), IsOk(Child));
			AActor* Parent = nullptr;
			AActor* Attached = nullptr;
			for (AActor* Actor : Lamps())
			{
				if (Actor->GetActorLabel() == TEXT("RqlSpecLampRig")) Parent = Actor;
				if (Actor->GetActorLabel() == TEXT("RqlSpecLamp")) Attached = Actor;
			}
			if (!TestTrue(TEXT("both spawned"), Parent && Attached))
				return;
			TestTrue(TEXT("the plain Actor has a root"), Parent->GetRootComponent() != nullptr);
			TestEqual(TEXT("and kept its location"), Parent->GetActorLocation(), FVector(100, 200, 0));
			TestTrue(TEXT("the child is attached"), Attached->GetAttachParentActor() == Parent);
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
			// unreal.find("RqlSpecLamp") | select $ref
			const TSharedPtr<FJsonObject> RefRow = Exec(TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSpecLamp"}]},"stages":[{"op":"select","fields":[{"name":"$ref","expr":{"path":[{"meta":"ref"}]}}]}]})RQL"));
			const FString Ref = CellText(RefRow, 0, 0);
			TestTrue(*FString::Printf(TEXT("a PointLight ref (%s)"), *Ref), Ref.StartsWith(TEXT("PointLight#")));
			// ref("PointLight#<id>") | select Label
			const FString RefPlan = FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"%s"}]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"), *Ref);
			const TSharedPtr<FJsonObject> ByRef = Exec(RefPlan);
			TestEqual(*FString::Printf(TEXT("ref (%s)"), *Codes(ByRef)), CellText(ByRef, 0, 0), FString(TEXT("RqlSpecLamp")));

			// A destroyed actor's ref never resolves to another object (the id carries the slot's serial number).
			Found[0]->GetWorld()->EditorDestroyActor(Found[0], true);
			SpawnLamp();
			const TSharedPtr<FJsonObject> Stale = Exec(RefPlan);
			TestEqual(TEXT("a stale ref finds nothing"), RowCount(Stale), 0);
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

		It("selects actors by query through Selected, without marking the level unsaved", [this]()
		{
			SpawnLamp();
			Exec(/* editor | call SpawnActor("Actor", "RqlSpecLampGroup") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"Actor"},{"lit":"RqlSpecLampGroup"}]}]})RQL"), true);
			UPackage* LevelPackage = World() ? World()->GetOutermost() : nullptr;
			if (LevelPackage)
				LevelPackage->SetDirtyFlag(false);
			const TSharedPtr<FJsonObject> Set = Exec(/* actors | where Label starts-with "RqlSpecLamp" | set Selected = true */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"starts-with","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Selected"]},"value":{"lit":true}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("select ok (%s)"), *Codes(Set)), IsOk(Set));
			TestEqual(TEXT("both selected"), GEditor ? GEditor->GetSelectedActorCount() : 0, 2);
			TestEqual(TEXT("Selected reads back"), static_cast<int32>(Exec(/* actors | where Label starts-with "RqlSpecLamp" and Selected | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"and","args":[{"op":"starts-with","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]},{"path":["Selected"]}]}},{"op":"count"}]})RQL"))->GetNumberField(TEXT("value"))), 2);
			TestFalse(TEXT("selection is not a level edit"), LevelPackage && LevelPackage->IsDirty());
		});

		It("places an asset: a StaticMesh path spawns a StaticMeshActor showing it; other assets are refused", [this]()
		{
			const TSharedPtr<FJsonObject> Box = Exec(/* editor | call SpawnActor("/Engine/BasicShapes/Cube", "RqlSpecLampBox", "(300, 0, 0)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"/Engine/BasicShapes/Cube"},{"lit":"RqlSpecLampBox"},{"lit":"(300, 0, 0)"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("spawn from a mesh ok (%s)"), *Codes(Box)), IsOk(Box));
			AActor* Placed = nullptr;
			for (AActor* Actor : Lamps())
			{
				if (Actor->GetActorLabel() == TEXT("RqlSpecLampBox"))
					Placed = Actor;
			}
			if (!TestNotNull(TEXT("placed"), Placed))
				return;
			const UStaticMeshComponent* Mesh = Placed->FindComponentByClass<UStaticMeshComponent>();
			TestTrue(TEXT("a StaticMeshActor"), Placed->IsA<AStaticMeshActor>());
			TestTrue(TEXT("showing the mesh"), Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetName() == TEXT("Cube"));
			TestEqual(TEXT("at the location"), Placed->GetActorLocation(), FVector(300, 0, 0));
			const TSharedPtr<FJsonObject> Material = Exec(/* editor | call SpawnActor("/Engine/EngineMaterials/DefaultMaterial", "RqlSpecLampNo") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"/Engine/EngineMaterials/DefaultMaterial"},{"lit":"RqlSpecLampNo"}]}]})RQL"), true);
			TestFalse(TEXT("a material is not placeable"), IsOk(Material));
			TestTrue(*FString::Printf(TEXT("says why (%s)"), *Codes(Material)), Codes(Material).Contains(TEXT("cannot be placed")));
		});

		It("explains a refused attach (a Static actor under a Movable parent) and suggests asset paths", [this]()
		{
			Exec(/* editor | call SpawnActor("/Engine/BasicShapes/Cube", "RqlSpecLampBox", "(300, 0, 0)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"/Engine/BasicShapes/Cube"},{"lit":"RqlSpecLampBox"},{"lit":"(300, 0, 0)"}]}]})RQL"), true);
			Exec(/* editor | call SpawnActor("Actor", "RqlSpecLampGroup") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"Actor"},{"lit":"RqlSpecLampGroup"}]}]})RQL"), true);
			const TSharedPtr<FJsonObject> Attach = Exec(/* unreal.find("RqlSpecLampBox") | set Parent = unreal.find("RqlSpecLampGroup") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSpecLampBox"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Parent"]},"value":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSpecLampGroup"}]}}]}]})RQL"), true);
			TestFalse(TEXT("refused"), IsOk(Attach));
			TestTrue(*FString::Printf(TEXT("names the mobility (%s)"), *Codes(Attach)), Codes(Attach).Contains(TEXT("Mobility"))
				&& !Codes(Attach).Contains(TEXT("no root component")));
			const TSharedPtr<FJsonObject> Short = Exec(/* unreal.find("RqlSpecLampBox") | get Components | where $ is StaticMeshComponent | set StaticMesh = "Cube" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSpecLampBox"}]},"stages":[{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"StaticMeshComponent"}]}},{"op":"set","assign":[{"target":{"path":["StaticMesh"]},"value":{"lit":"Cube"}}]}]})RQL"), true);
			TestFalse(TEXT("a bare mesh name does not resolve"), IsOk(Short));
			TestTrue(*FString::Printf(TEXT("suggests the path (%s)"), *Codes(Short)), Codes(Short).Contains(TEXT("did you mean /Engine/BasicShapes/Cube")));
		});

		It("names a level by its map name, refuses bad sublevels and shows / hides a level", [this]()
		{
			UWorld* W = World();
			if (!TestNotNull(TEXT("editor world"), W))
				return;
			const FString Package = W->PersistentLevel->GetOutermost()->GetName();
			auto With = [&Package](const TCHAR* Plan)
			{
				return FString(Plan).Replace(TEXT("@MAP@"), *FPackageName::GetShortName(Package)).Replace(TEXT("@PKG@"), *Package);
			};
			// editor | call UnloadSublevel("@MAP@") — the map name resolves to the level (it used to be "no Level '<name>'")
			const TSharedPtr<FJsonObject> Unload = Exec(With(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"UnloadSublevel","args":[{"lit":"@MAP@"}]}]})RQL")), true);
			TestTrue(*FString::Printf(TEXT("resolved, then refused as the persistent level (%s)"), *Codes(Unload)), !IsOk(Unload) && Codes(Unload).Contains(TEXT("persistent level cannot be unloaded")));
			// editor | call AddSublevel("/Game/RqlSpecNoSuchLevel")
			const TSharedPtr<FJsonObject> Missing = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddSublevel","args":[{"lit":"/Game/RqlSpecNoSuchLevel"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("an unsaved level is refused (%s)"), *Codes(Missing)), !IsOk(Missing) && Codes(Missing).Contains(TEXT("no saved level")));
			// editor | call AddSublevel("/Engine/BasicShapes/Cube") — a package that is not a map leaves no broken streaming entry
			const int32 StreamingBefore = W->GetStreamingLevels().Num();
			const TSharedPtr<FJsonObject> NotMap = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddSublevel","args":[{"lit":"/Engine/BasicShapes/Cube"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("a non-map package is refused (%s)"), *Codes(NotMap)), !IsOk(NotMap) && Codes(NotMap).Contains(TEXT("no saved level (map)")));
			TestEqual(TEXT("no streaming entry added"), W->GetStreamingLevels().Num(), StreamingBefore);
			// editor | call AddSublevel("@PKG@")
			const TSharedPtr<FJsonObject> Self = Exec(With(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddSublevel","args":[{"lit":"@PKG@"}]}]})RQL")), true);
			TestTrue(*FString::Printf(TEXT("the world's own level is refused (%s)"), *Codes(Self)), !IsOk(Self) && Codes(Self).Contains(TEXT("its own sublevel")));
			// levels | where IsPersistent | set IsVisible = false
			const TSharedPtr<FJsonObject> Hide = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["levels"]},"stages":[{"op":"where","pred":{"path":["IsPersistent"]}},{"op":"set","assign":[{"target":{"path":["IsVisible"]},"value":{"lit":false}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("hidden (%s)"), *Codes(Hide)), IsOk(Hide) && !W->PersistentLevel->bIsVisible);
			// levels | where IsPersistent | set IsVisible = true
			const TSharedPtr<FJsonObject> Show = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["levels"]},"stages":[{"op":"where","pred":{"path":["IsPersistent"]}},{"op":"set","assign":[{"target":{"path":["IsVisible"]},"value":{"lit":true}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("shown again (%s)"), *Codes(Show)), IsOk(Show) && W->PersistentLevel->bIsVisible);
			if (!W->PersistentLevel->bIsVisible)
				UEditorLevelUtils::SetLevelVisibility(W->PersistentLevel, true, false);
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

	Describe("write rules", [this]()
	{
		It("writes container elements through the owner's setter (notified) and refuses read-only containers", [this]()
		{
			SpawnLamp();
			TArray<AActor*> Found = Lamps();
			if (!TestEqual(TEXT("spawned"), Found.Num(), 1))
				return;
			AActor* Lamp = Found[0];
			// actors | where Label == "RqlSpecLamp" | set Tags = ["a", "b"]
			TestTrue(TEXT("tags set"), IsOk(Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Tags"]},"value":{"list":[{"lit":"a"},{"lit":"b"}]}}]}]})RQL"), true)));

			int32 TagNotifications = 0;
			const FDelegateHandle Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
				[Lamp, &TagNotifications](UObject* Object, FPropertyChangedEvent& Event)
				{
					if (Object == Lamp && Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(AActor, Tags))
						++TagNotifications;
				});
			// actors | where Label == "RqlSpecLamp" | set Tags[0] = "z"
			const TSharedPtr<FJsonObject> Element = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Tags",{"index":0}]},"value":{"lit":"z"}}]}]})RQL"), true);
			FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);
			TestTrue(*FString::Printf(TEXT("element set ok (%s)"), *Codes(Element)), IsOk(Element));
			TestTrue(TEXT("element written"), Lamp->Tags.Num() == 2 && Lamp->Tags[0] == FName(TEXT("z")) && Lamp->Tags[1] == FName(TEXT("b")));
			TestTrue(TEXT("the owner got PostEditChangeProperty"), TagNotifications >= 1);

			// A VisibleAnywhere / BlueprintReadOnly array: its elements are read-only too.
			UWorld* W = World();
			ALevelStreamingVolume* Volume = W ? W->SpawnActor<ALevelStreamingVolume>() : nullptr;
			if (!TestNotNull(TEXT("volume"), Volume))
				return;
			Volume->SetActorLabel(TEXT("RqlSpecLampRO"));
			Volume->StreamingLevelNames.Add(TEXT("Keep"));
			// actors | where Label == "RqlSpecLampRO" | set StreamingLevelNames[0] = "X"
			const TSharedPtr<FJsonObject> ReadOnly = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLampRO"}]}},{"op":"set","assign":[{"target":{"path":["StreamingLevelNames",{"index":0}]},"value":{"lit":"X"}}]}]})RQL"), true);
			TestFalse(TEXT("read-only element refused"), IsOk(ReadOnly));
			TestTrue(*FString::Printf(TEXT("readonly-member (%s)"), *Codes(ReadOnly)), Codes(ReadOnly).Contains(TEXT("readonly-member")));
			TestEqual(TEXT("unchanged"), Volume->StreamingLevelNames[0], FName(TEXT("Keep")));
		});

		It("collapses duplicate set elements and map keys", [this]()
		{
			SpawnLamp();
			// actors | where Label == "RqlSpecLamp" | call AddComponent("AudioComponent", "RqlSpecAudio")
			TestTrue(TEXT("audio added"), IsOk(Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"call","method":"AddComponent","args":[{"lit":"AudioComponent"},{"lit":"RqlSpecAudio"}]}]})RQL"), true)));
			// actors | where Label == "RqlSpecLamp" | call AddComponent("LODSyncComponent", "RqlSpecSync")
			TestTrue(TEXT("lod sync added"), IsOk(Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"call","method":"AddComponent","args":[{"lit":"LODSyncComponent"},{"lit":"RqlSpecSync"}]}]})RQL"), true)));
			// actors | where Label == "RqlSpecLamp" | get Components | where $ is AudioComponent | set ConcurrencySet = [null, null]
			const TSharedPtr<FJsonObject> Set = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"AudioComponent"}]}},{"op":"set","assign":[{"target":{"path":["ConcurrencySet"]},"value":{"list":[{"lit":null},{"lit":null}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set write ok (%s)"), *Codes(Set)), IsOk(Set));
			// actors | where Label == "RqlSpecLamp" | get Components | where $ is LODSyncComponent | set CustomLODMapping = {a: {}, A: {}}
			const TSharedPtr<FJsonObject> Map = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"LODSyncComponent"}]}},{"op":"set","assign":[{"target":{"path":["CustomLODMapping"]},"value":{"object":[{"key":"a","value":{"object":[]}},{"key":"A","value":{"object":[]}}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("map write ok (%s)"), *Codes(Map)), IsOk(Map));
			TArray<AActor*> Found = Lamps();
			if (!TestEqual(TEXT("one lamp"), Found.Num(), 1))
				return;
			const UAudioComponent* Audio = Found[0]->FindComponentByClass<UAudioComponent>();
			const ULODSyncComponent* Sync = Found[0]->FindComponentByClass<ULODSyncComponent>();
			TestTrue(TEXT("[null, null] is one element"), Audio && Audio->ConcurrencySet.Num() == 1);
			TestTrue(TEXT("keys a / A (one FName) are one entry"), Sync && Sync->CustomLODMapping.Num() == 1);
		});

		It("rejects integers out of range instead of wrapping", [this]()
		{
			SpawnLamp();
			TArray<AActor*> Found = Lamps();
			if (!TestEqual(TEXT("spawned"), Found.Num(), 1))
				return;
			// actors | where Label == "RqlSpecLamp" | set InputPriority = 7
			TestTrue(TEXT("in range ok"), IsOk(Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["InputPriority"]},"value":{"lit":7}}]}]})RQL"), true)));
			// actors | where Label == "RqlSpecLamp" | set InputPriority = 3000000000
			const TSharedPtr<FJsonObject> Big = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["InputPriority"]},"value":{"lit":3000000000}}]}]})RQL"), true);
			TestFalse(TEXT("out of range fails"), IsOk(Big));
			TestTrue(*FString::Printf(TEXT("says out of range (%s)"), *Codes(Big)), Codes(Big).Contains(TEXT("out of range")));
			TestEqual(TEXT("value kept"), Found[0]->InputPriority, 7);
		});

		It("refuses level saves outside writable content and invalid component names", [this]()
		{
			// editor | call SaveLevel("/Engine/RqlSpecNope")
			const TSharedPtr<FJsonObject> Save = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SaveLevel","args":[{"lit":"/Engine/RqlSpecNope"}]}]})RQL"), true);
			TestFalse(TEXT("SaveLevel to /Engine refused"), IsOk(Save));
			TestTrue(*FString::Printf(TEXT("not writable (%s)"), *Codes(Save)), Codes(Save).Contains(TEXT("not a writable level path")));
			// editor | call NewLevel("/Engine/RqlSpecNope")
			UWorld* Before = World();
			const TSharedPtr<FJsonObject> New = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"NewLevel","args":[{"lit":"/Engine/RqlSpecNope"}]}]})RQL"), true);
			TestFalse(TEXT("NewLevel at /Engine refused"), IsOk(New));
			TestTrue(TEXT("the editor world is kept"), World() == Before);

			SpawnLamp();
			// actors | where Label == "RqlSpecLamp" | call AddComponent("StaticMeshComponent", "bad name.x")
			const TSharedPtr<FJsonObject> Bad = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"call","method":"AddComponent","args":[{"lit":"StaticMeshComponent"},{"lit":"bad name.x"}]}]})RQL"), true);
			TestFalse(TEXT("invalid name refused"), IsOk(Bad));
			TestTrue(*FString::Printf(TEXT("says why (%s)"), *Codes(Bad)), Codes(Bad).Contains(TEXT("not a valid component name")));
			TArray<AActor*> Found = Lamps();
			TestTrue(TEXT("no component added"), Found.Num() == 1 && Found[0]->GetInstanceComponents().Num() == 0);
		});

		It("refuses null for an object parameter without a C++ default", [this]()
		{
			// editor | call SaveAsset(null)
			const TSharedPtr<FJsonObject> Result = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SaveAsset","args":[{"lit":null}]}]})RQL"), true);
			TestFalse(TEXT("refused"), IsOk(Result));
			TestTrue(*FString::Printf(TEXT("null is not allowed (%s)"), *Codes(Result)), Codes(Result).Contains(TEXT("null is not allowed")));
		});

		It("refuses writes to class default objects (operations objects stay usable)", [this]()
		{
			const bool bBefore = GetDefault<APointLight>()->IsHidden();
			// unreal.asset("/Script/Engine.Default__PointLight") | set bHidden = true
			const TSharedPtr<FJsonObject> Result = Exec(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Script/Engine.Default__PointLight"}]},"stages":[{"op":"set","assign":[{"target":{"path":["bHidden"]},"value":{"lit":true}}]}]})RQL"), true);
			TestFalse(TEXT("refused"), IsOk(Result));
			TestTrue(*FString::Printf(TEXT("says read-only (%s)"), *Codes(Result)), Codes(Result).Contains(TEXT("class default object")));
			TestEqual(TEXT("the CDO is unchanged"), GetDefault<APointLight>()->IsHidden(), bBefore);
		});

		It("accepts #rrggbb colors and rejects non-hex ones", [this]()
		{
			SpawnLamp();
			// actors | where Label == "RqlSpecLamp" | get Components | where $ is PointLightComponent | set LightColor = "#ff0000"
			const TSharedPtr<FJsonObject> Red = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"PointLightComponent"}]}},{"op":"set","assign":[{"target":{"path":["LightColor"]},"value":{"lit":"#ff0000"}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("hex ok (%s)"), *Codes(Red)), IsOk(Red));
			// actors | where Label == "RqlSpecLamp" | get Components | where $ is PointLightComponent | set LightColor = "#zzzzzz"
			const TSharedPtr<FJsonObject> Bad = Exec(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"PointLightComponent"}]}},{"op":"set","assign":[{"target":{"path":["LightColor"]},"value":{"lit":"#zzzzzz"}}]}]})RQL"), true);
			TestFalse(TEXT("non-hex refused"), IsOk(Bad));
			TArray<AActor*> Found = Lamps();
			const UPointLightComponent* Light = Found.Num() == 1 ? Found[0]->FindComponentByClass<UPointLightComponent>() : nullptr;
			TestTrue(TEXT("still red"), Light && Light->LightColor == FColor(255, 0, 0, 255));
		});
	});

	// RQL 602ce7c: target scope in set values, string functions, multi-key group-by, batch checks and host hints. Plans
	// from Rql.Parser 0.3.0-dev.602ce7c.
	Describe("target scope, functions, group-by and batch checks", [this]()
	{
		It("spreads targets with $index, renames with replace and groups by two keys", [this]()
		{
			Exec(/* editor | call SpawnActor("PointLight", "RqlSpecLampA", "(0, 0, 500)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLampA"},{"lit":"(0, 0, 500)"}]}]})RQL"), true);
			Exec(/* editor | call SpawnActor("PointLight", "RqlSpecLampC", "(0, 0, 500)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLampC"},{"lit":"(0, 0, 500)"}]}]})RQL"), true);
			Exec(/* editor | call SpawnActor("PointLight", "RqlSpecLampB", "(0, 0, 500)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLampB"},{"lit":"(0, 0, 500)"}]}]})RQL"), true);
			if (!TestEqual(TEXT("three lamps"), Lamps().Num(), 3))
				return;

			const TSharedPtr<FJsonObject> Spread = Exec(/* actors | where Label starts-with "RqlSpecLamp" | sort Label | set Location.X = $index * 100 */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"starts-with","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"sort","keys":[{"expr":{"path":["Label"]}}]},{"op":"set","assign":[{"target":{"path":["Location","X"]},"value":{"op":"*","args":[{"path":[{"meta":"index"}]},{"lit":100}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("spread ok (%s)"), *Codes(Spread)), IsOk(Spread));
			for (AActor* Lamp : Lamps())
			{
				// A, B, C in label order (spawned A, C, B): $index is the position in the sorted stream entering set.
				const FString Label = Lamp->GetActorLabel();
				TestEqual(*FString::Printf(TEXT("%s X"), *Label), Lamp->GetActorLocation().X, 100.0 * (Label[Label.Len() - 1] - TEXT('A')));
			}

			const TSharedPtr<FJsonObject> Groups = Exec(/* actors | where Label starts-with "RqlSpecLamp" | group-by $type, far=(Location.X > 50) */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"starts-with","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"group-by","fields":[{"name":"$type","expr":{"path":[{"meta":"type"}]}},{"name":"far","expr":{"op":">","args":[{"path":["Location","X"]},{"lit":50}]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("group-by ok (%s)"), *Codes(Groups)), IsOk(Groups));
			TArray<FString> Columns;
			const TArray<TSharedPtr<FJsonValue>>* ColumnValues = nullptr;
			if (Groups.IsValid() && Groups->TryGetArrayField(TEXT("columns"), ColumnValues))
			{
				for (const TSharedPtr<FJsonValue>& Column : *ColumnValues)
					Columns.Add(Column->AsString());
			}
			TestEqual(TEXT("one column per key, then count"), FString::Join(Columns, TEXT(",")), FString(TEXT("$type,far,count")));
			TestEqual(TEXT("two groups (near A; far B and C)"), RowCount(Groups), 2);
			double FarCount = -1.0;
			for (int32 Row = 0; Row < RowCount(Groups); ++Row)
			{
				const TSharedPtr<FJsonValue> Far = Cell(Groups, Row, 1);
				if (Far.IsValid() && Far->Type == EJson::Boolean && Far->AsBool() && Cell(Groups, Row, 2).IsValid())
					FarCount = Cell(Groups, Row, 2)->AsNumber();
			}
			TestEqual(TEXT("the far group counts two"), FarCount, 2.0);

			const TSharedPtr<FJsonObject> Renamed = Exec(/* actors | where Label starts-with "RqlSpecLamp" | set Label = Label.replace("RqlSpecLamp", "RqlSpecLampRenamed_") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"starts-with","args":[{"path":["Label"]},{"lit":"RqlSpecLamp"}]}},{"op":"set","assign":[{"target":{"path":["Label"]},"value":{"call":"replace","on":{"path":["Label"]},"args":[{"lit":"RqlSpecLamp"},{"lit":"RqlSpecLampRenamed_"}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("rename ok (%s)"), *Codes(Renamed)), IsOk(Renamed));
			TArray<FString> Labels;
			for (AActor* Lamp : Lamps())
				Labels.Add(Lamp->GetActorLabel());
			Labels.Sort();
			TestEqual(TEXT("renamed labels"), FString::Join(Labels, TEXT(",")), FString(TEXT("RqlSpecLampRenamed_A,RqlSpecLampRenamed_B,RqlSpecLampRenamed_C")));
		});

		It("refuses a call that would reach one object twice, on a dry run too, and accepts it after distinct", [this]()
		{
			Exec(/* editor | call SpawnActor("PointLight", "RqlSpecLampA", "(0, 0, 500)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLampA"},{"lit":"(0, 0, 500)"}]}]})RQL"), true);
			Exec(/* editor | call SpawnActor("PointLight", "RqlSpecLampB", "(0, 0, 500)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLampB"},{"lit":"(0, 0, 500)"}]}]})RQL"), true);
			Exec(/* editor | call SpawnActor("PointLight", "RqlSpecLampC", "(0, 0, 500)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSpecLampC"},{"lit":"(0, 0, 500)"}]}]})RQL"), true);
			const TSharedPtr<FJsonObject> Attach = Exec(/* actors | where Label == "RqlSpecLampB" or Label == "RqlSpecLampC" | set Parent = unreal.find("RqlSpecLampA") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"or","args":[{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLampB"}]},{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLampC"}]}]}},{"op":"set","assign":[{"target":{"path":["Parent"]},"value":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSpecLampA"}]}}]}]})RQL"), true);
			if (!TestTrue(*FString::Printf(TEXT("attached (%s)"), *Codes(Attach)), IsOk(Attach)))
				return;

			for (const bool bDryRun : { true, false })
			{
				const TSharedPtr<FJsonObject> Twice = Exec(/* actors | where Label == "RqlSpecLampB" or Label == "RqlSpecLampC" | get Parent | call Duplicate() */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"or","args":[{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLampB"}]},{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLampC"}]}]}},{"op":"get","expr":{"path":["Parent"]}},{"op":"call","method":"Duplicate","args":[]}]})RQL"), true, bDryRun);
				TestFalse(*FString::Printf(TEXT("refused (dry run %d)"), bDryRun), IsOk(Twice));
				TestTrue(*FString::Printf(TEXT("invalid-value naming the duplicate (%s)"), *Codes(Twice)),
					Codes(Twice).Contains(TEXT("invalid-value")) && Codes(Twice).Contains(TEXT("Duplicate would run twice")) && Codes(Twice).Contains(TEXT("distinct")));
				TestEqual(TEXT("nothing duplicated"), Lamps().Num(), 3);
			}
			const TSharedPtr<FJsonObject> Once = Exec(/* actors | where Label == "RqlSpecLampB" or Label == "RqlSpecLampC" | get Parent | distinct | call Duplicate() */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"or","args":[{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLampB"}]},{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSpecLampC"}]}]}},{"op":"get","expr":{"path":["Parent"]}},{"op":"distinct"},{"op":"call","method":"Duplicate","args":[]}]})RQL"), true, true);
			TestTrue(*FString::Printf(TEXT("distinct passes the batch check (%s)"), *Codes(Once)), IsOk(Once));
			TestEqual(TEXT("one receipt row"), RowCount(Once), 1);
		});

		It("hints without budgets.* and names unreal.find for a ref given a name", [this]()
		{
			// More than 20 objects reach set (the limit is checked as targets arrive, before members are resolved).
			const TSharedPtr<FJsonObject> Limit = Exec(/* unreal.assets("Material", "/Engine") | set Name = "x" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"assets","on":{"path":["unreal"]},"args":[{"lit":"Material"},{"lit":"/Engine"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Name"]},"value":{"lit":"x"}}]}]})RQL"), true, true);
			TestTrue(*FString::Printf(TEXT("mutation-limit (%s)"), *Codes(Limit)), Codes(Limit).Contains(TEXT("mutation-limit")));
			const FString LimitHints = UnrealMcpRqlSpecHelpers::Hints(Limit);
			TestTrue(*FString::Printf(TEXT("suggests batches (%s)"), *LimitHints), LimitHints.Contains(TEXT("first 20")));
			TestFalse(*FString::Printf(TEXT("no budgets advice (%s)"), *LimitHints), LimitHints.Contains(TEXT("budgets")));

			SpawnLamp();
			const TSharedPtr<FJsonObject> ByName = Exec(/* ref("RqlSpecLamp") | select Label */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"RqlSpecLamp"}]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"));
			TestFalse(TEXT("a name is not a ref"), IsOk(ByName));
			const FString ByNameHints = UnrealMcpRqlSpecHelpers::Hints(ByName);
			TestTrue(*FString::Printf(TEXT("the hint names the finder (%s)"), *ByNameHints), ByNameHints.Contains(TEXT("unreal.find(")));
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
				TestEqual(TEXT("shared = the base"), Entry->GetObjectField(TEXT("shared"))->GetStringField(TEXT("name")),
					APointLight::StaticClass()->GetSuperClass()->GetName());
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
