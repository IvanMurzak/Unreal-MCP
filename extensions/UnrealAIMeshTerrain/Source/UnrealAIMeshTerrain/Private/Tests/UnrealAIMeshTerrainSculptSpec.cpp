// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "UnrealMcpRqlTools.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "MeshPartition.h"
#include "MeshPartitionModifierActor.h"
#include "MeshPartitionModifierComponent.h"
#include "Modifiers/MeshPartitionNoiseModifier.h"
#include "Modifiers/MeshPartitionProjectSculptLayersModifier.h"

// ============================================================================================
//  UE Automation spec for the Mesh Terrain brush strokes: `meshTerrainBrush | call Sculpt / Paint / Erase`
//  (UnrealAIMeshTerrainSculptApi). Every stroke is checked through the built terrain (SampleSurface: the collision
//  height and the evaluated paint channels), the brush modifier it is stored in, and undo / redo.
//
//  Plans are the QueryPlans Rql.Parser produces for the query in the comment above them. Terrains are made with
//  UnrealAIMeshTerrain::SpawnFlatTerrain (what Mesh Terrain Mode's Create tool runs), far from the other specs' terrains.
//
//  Run via:  Automation RunTests UnrealAIMeshTerrain.Sculpt
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIMeshTerrainSculptSpec, "UnrealAIMeshTerrain.Sculpt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	using AMeshPartition = UE::MeshPartition::AMeshPartition;

	static FString SculptSpecJson(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** The object in the "result" column of the first row of an RQL call result (null when there is none). */
	static TSharedPtr<FJsonObject> SculptSpecResult(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("columns"), Columns) || !Result->TryGetArrayField(TEXT("rows"), Rows) || Rows->IsEmpty())
			return nullptr;
		const int32 Column = Columns->IndexOfByPredicate([](const TSharedPtr<FJsonValue>& Name) { return Name->AsString() == TEXT("result"); });
		const TArray<TSharedPtr<FJsonValue>>& Row = (*Rows)[0]->AsArray();
		return Row.IsValidIndex(Column) && Row[Column]->Type == EJson::Object ? Row[Column]->AsObject() : nullptr;
	}

	/**
	 * `meshTerrainBrush | call <Method>(unreal.find("<Label>"), <Args>)` as a plan; @p Args is a comma-separated list of
	 * literals: strings quoted, numbers bare, a point as `[x, y]` (z = 0).
	 */
	static FString SculptSpecPlan(const FString& Method, const FString& Label, const TArray<FString>& Args)
	{
		TArray<FString> Items = { FString::Printf(TEXT(R"({"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]})"), *Label) };
		for (const FString& Arg : Args)
		{
			if (Arg.StartsWith(TEXT("[")))
			{
				FString X, Y;
				Arg.Mid(1, Arg.Len() - 2).Split(TEXT(","), &X, &Y);
				Items.Add(FString::Printf(TEXT(R"({"list":[{"lit":%s},{"lit":%s},{"lit":0}]})"), *X.TrimStartAndEnd(), *Y.TrimStartAndEnd()));
			}
			else
			{
				Items.Add(FString::Printf(TEXT(R"({"lit":%s})"), *Arg));
			}
		}
		return FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainBrush"]},"stages":[{"op":"call","method":"%s","args":[%s]}]})RQL"),
			*Method, *FString::Join(Items, TEXT(",")));
	}

	/** Run a stroke; its result object (null after a failure, the RQL error then in @p OutJson). */
	static TSharedPtr<FJsonObject> SculptSpecStroke(const FString& Method, const FString& Label, const TArray<FString>& Args, FString* OutJson = nullptr)
	{
		TSharedPtr<FJsonObject> Result;
		const bool bOk = UnrealMcpRqlTools::RunPlan(SculptSpecPlan(Method, Label, Args), true, &Result);
		if (OutJson)
			*OutJson = SculptSpecJson(Result);
		return bOk ? SculptSpecResult(Result) : nullptr;
	}

	static FString SculptSpecLabel(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Short));
	}

	static TArray<UE::MeshPartition::UProjectMeshLayersModifier*> SculptSpecBrushes(const AMeshPartition* Terrain)
	{
		TArray<UE::MeshPartition::UProjectMeshLayersModifier*> Out;
		for (UnrealAIMeshTerrain::UModifierComponent* Modifier : UnrealAIMeshTerrain::Modifiers(Terrain))
		{
			if (UE::MeshPartition::UProjectMeshLayersModifier* Brush = Cast<UE::MeshPartition::UProjectMeshLayersModifier>(Modifier))
				Out.Add(Brush);
		}
		return Out;
	}

	static double SculptSpecHeight(const AMeshPartition* Terrain, double X, double Y)
	{
		const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, X, Y, /*bChannels*/ false);
		return Sample.bInside ? Sample.Height : -1.0e9;
	}

	static float SculptSpecChannel(const AMeshPartition* Terrain, double X, double Y, const TCHAR* Channel)
	{
		const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, X, Y);
		const float* Value = Sample.Channels.Find(Channel);
		return Value ? *Value : 0.0f;
	}
END_DEFINE_SPEC(FUnrealAIMeshTerrainSculptSpec)

void FUnrealAIMeshTerrainSculptSpec::Define()
{
	It("lists Sculpt, Paint and Erase with their parameters in the schema and the guide", [this]()
	{
		bool bOk = false;
		const FString Schema = UnrealMcpRqlTools::Schema(TEXT("MeshTerrainBrushOperations"), bOk);
		TestTrue(TEXT("schema ok"), bOk);
		for (const TCHAR* Method : {
			TEXT("Sculpt(MeshPartition Terrain, string Mode, Vector Point, double Radius, double Height, double Strength, double Falloff)"),
			TEXT("Paint(MeshPartition Terrain, string Channel, Vector Point, double Radius, double Weight, double Falloff)"),
			TEXT("Erase(MeshPartition Terrain, string Channel, Vector Point, double Radius, double Falloff)") })
		{
			TestTrue(*FString::Printf(TEXT("schema lists %s (%s)"), Method, *Schema.Left(600)), Schema.Contains(Method));
		}
		const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
		TestTrue(TEXT("the guide shows a stroke"), Skill.Contains(TEXT("meshTerrainBrush | call Sculpt(")) && Skill.Contains(TEXT("meshTerrainBrush | call Paint(")));
	});

	It("refuses bad strokes with a clear error and changes nothing", [this]()
	{
		const FString Label = SculptSpecLabel(TEXT("SculptGuard"));
		AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-600000.0, 2000000.0, 0.0), 2000.0, 2000.0, 20, 20);
		ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
		if (!TestNotNull(TEXT("terrain spawned"), Terrain))
			return;
		struct FCase { const TCHAR* Method; TArray<FString> Args; const TCHAR* Expected; };
		const TArray<FCase> Cases = {
			{ TEXT("Sculpt"), { TEXT("\"Pinch\""), TEXT("[-600000, 2000000]"), TEXT("300") }, TEXT("not one of Raise, Lower, Flatten, Smooth") },
			{ TEXT("Sculpt"), { TEXT("\"Raise\""), TEXT("[-600000, 2000000]"), TEXT("0") }, TEXT("Radius 0 cm is outside 1 .. 25000") },
			{ TEXT("Sculpt"), { TEXT("\"Raise\""), TEXT("[-600000, 2000000]"), TEXT("30000") }, TEXT("Radius 30000 cm is outside 1 .. 25000") },
			{ TEXT("Sculpt"), { TEXT("\"Lower\""), TEXT("[-600000, 2000000]"), TEXT("300"), TEXT("-5") }, TEXT("Height -5 cm is outside") },
			{ TEXT("Sculpt"), { TEXT("\"Raise\""), TEXT("[-600000, 2000000]"), TEXT("300"), TEXT("100"), TEXT("1.5") }, TEXT("Strength 1.5 is outside") },
			{ TEXT("Sculpt"), { TEXT("\"Raise\""), TEXT("[-600000, 2000000]"), TEXT("300"), TEXT("100"), TEXT("1"), TEXT("2") }, TEXT("Falloff 2 is outside 0 .. 1") },
			{ TEXT("Sculpt"), { TEXT("\"Raise\""), TEXT("[-500000, 2000000]"), TEXT("300") }, TEXT("is off the terrain") },
			{ TEXT("Paint"), { TEXT("\"Purple\""), TEXT("[-600000, 2000000]"), TEXT("300") }, TEXT("'Purple' is not a paint channel") },
			{ TEXT("Paint"), { TEXT("\"Red\""), TEXT("[-600000, 2000000]"), TEXT("300"), TEXT("2") }, TEXT("Weight 2 is outside 0 .. 1") },
			{ TEXT("Paint"), { TEXT("\"\""), TEXT("[-600000, 2000000]"), TEXT("300") }, TEXT("Channel is empty") },
		};
		for (const FCase& Case : Cases)
		{
			FString Json;
			TestNull(*FString::Printf(TEXT("%s %s fails"), Case.Method, *FString::Join(Case.Args, TEXT(", "))), SculptSpecStroke(Case.Method, Label, Case.Args, &Json).Get());
			TestTrue(*FString::Printf(TEXT("names the problem: %s (%s)"), Case.Expected, *Json), Json.Contains(Case.Expected));
		}
		TestEqual(TEXT("no brush modifier was placed"), SculptSpecBrushes(Terrain).Num(), 0);
		TestTrue(TEXT("the ground is untouched"), FMath::IsNearlyZero(SculptSpecHeight(Terrain, -600000.0, 2000000.0), 0.5));
	});

	It("raises, lowers, flattens and smooths the ground; strokes in one area accumulate in one brush modifier", [this]()
	{
		const FString Label = SculptSpecLabel(TEXT("SculptShape"));
		const FVector Center(-700000.0, 2000000.0, 100.0);
		AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, Center, 6000.0, 6000.0, 60, 60);
		ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
		if (!TestNotNull(TEXT("terrain spawned"), Terrain))
			return;
		const FString Point = TEXT("[-700000, 2000000]");

		// meshTerrainBrush | call Sculpt(unreal.find("<label>"), "Raise", [-700000, 2000000, 0], 800, 300)
		FString Json;
		TSharedPtr<FJsonObject> Stroke = SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Raise\""), Point, TEXT("800"), TEXT("300") }, &Json);
		if (!TestTrue(*FString::Printf(TEXT("Raise (%s)"), *Json), Stroke.IsValid()))
			return;
		TestTrue(TEXT("a new brush modifier"), Stroke->GetBoolField(TEXT("NewBrush")) && Stroke->GetStringField(TEXT("Brush")) == Label + TEXT("_Brush"));
		TestTrue(TEXT("Ready"), Stroke->GetBoolField(TEXT("Ready")));
		TestTrue(TEXT("vertices moved"), Stroke->GetIntegerField(TEXT("Vertices")) > 10);
		TestTrue(*FString::Printf(TEXT("Before / After (%s)"), *Json), FMath::IsNearlyEqual(Stroke->GetNumberField(TEXT("Before")), 100.0, 0.01) && FMath::IsNearlyEqual(Stroke->GetNumberField(TEXT("After")), 400.0, 0.01));
		TestTrue(*FString::Printf(TEXT("the centre is 3 m up (%.2f)"), SculptSpecHeight(Terrain, Center.X, Center.Y)), FMath::IsNearlyEqual(SculptSpecHeight(Terrain, Center.X, Center.Y), 400.0, 1.0));
		TestTrue(TEXT("the soft edge is between"), SculptSpecHeight(Terrain, Center.X + 600.0, Center.Y) > 105.0 && SculptSpecHeight(Terrain, Center.X + 600.0, Center.Y) < 395.0);
		TestTrue(TEXT("outside the radius the ground is untouched"), FMath::IsNearlyEqual(SculptSpecHeight(Terrain, Center.X + 1500.0, Center.Y), 100.0, 0.5));

		// A second stroke in the same area adds to the same brush modifier (its active sculpt layer).
		Stroke = SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Raise\""), TEXT("[-699800, 2000000]"), TEXT("800"), TEXT("100") }, &Json);
		TestTrue(*FString::Printf(TEXT("second Raise into the same brush (%s)"), *Json), Stroke.IsValid() && !Stroke->GetBoolField(TEXT("NewBrush")) && Stroke->GetStringField(TEXT("Brush")) == Label + TEXT("_Brush"));
		TestTrue(*FString::Printf(TEXT("accumulated (%.2f)"), SculptSpecHeight(Terrain, Center.X + 200.0, Center.Y)), FMath::IsNearlyEqual(SculptSpecHeight(Terrain, Center.X + 200.0, Center.Y), 500.0, 1.0));
		TestEqual(TEXT("one brush modifier"), SculptSpecBrushes(Terrain).Num(), 1);

		// Lower with half strength: 400 * 0.5 = 2 m down at the centre.
		Stroke = SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"lower\""), Point, TEXT("500"), TEXT("400"), TEXT("0.5"), TEXT("0") }, &Json);
		TestTrue(*FString::Printf(TEXT("Lower (%s)"), *Json), Stroke.IsValid());
		const double Lowered = SculptSpecHeight(Terrain, Center.X, Center.Y);
		TestTrue(*FString::Printf(TEXT("lowered by 2 m (%.2f)"), Lowered), Lowered > 280.0 && Lowered < 320.0);

		// Flatten a wide disc (hard edge) to world Z 50 cm: below the base, so the whole disc moves.
		Stroke = SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Flatten\""), Point, TEXT("1200"), TEXT("50"), TEXT("1"), TEXT("0") }, &Json);
		TestTrue(*FString::Printf(TEXT("Flatten (%s)"), *Json), Stroke.IsValid() && FMath::IsNearlyEqual(Stroke->GetNumberField(TEXT("After")), 50.0, 0.01));
		for (const FVector2D Offset : { FVector2D(0.0, 0.0), FVector2D(450.0, 0.0), FVector2D(-300.0, 700.0), FVector2D(0.0, -1000.0) })
		{
			const double Height = SculptSpecHeight(Terrain, Center.X + Offset.X, Center.Y + Offset.Y);
			TestTrue(*FString::Printf(TEXT("flat at 50 cm at %s (%.2f)"), *Offset.ToString(), Height), FMath::IsNearlyEqual(Height, 50.0, 1.0));
		}

		// A step, then Smooth: the step's edge gets lower.
		SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Raise\""), Point, TEXT("600"), TEXT("400"), TEXT("1"), TEXT("0") });
		const double Edge = SculptSpecHeight(Terrain, Center.X + 500.0, Center.Y);
		Stroke = SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Smooth\""), TEXT("[-699400, 2000000]"), TEXT("500") }, &Json);
		TestTrue(*FString::Printf(TEXT("Smooth (%s)"), *Json), Stroke.IsValid() && Stroke->GetIntegerField(TEXT("Vertices")) > 0);
		const double Smoothed = SculptSpecHeight(Terrain, Center.X + 500.0, Center.Y);
		TestTrue(*FString::Printf(TEXT("the step edge is softer (%.2f -> %.2f)"), Edge, Smoothed), Edge > 400.0 && Smoothed < Edge - 20.0 && Smoothed > 50.0);
		TestEqual(TEXT("still one brush modifier"), SculptSpecBrushes(Terrain).Num(), 1);
	});

	It("paints and erases a channel in the brush modifier's weights; sculpt and paint share the brush", [this]()
	{
		const FString Label = SculptSpecLabel(TEXT("SculptPaint"));
		const FVector Center(-800000.0, 2000000.0, 0.0);
		AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, Center, 6000.0, 6000.0, 60, 60);
		ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
		if (!TestNotNull(TEXT("terrain spawned"), Terrain))
			return;
		const FString Point = TEXT("[-800000, 2000000]");
		FString Json;
		TSharedPtr<FJsonObject> Stroke = SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Raise\""), Point, TEXT("700"), TEXT("200") }, &Json);
		TestTrue(*FString::Printf(TEXT("Raise (%s)"), *Json), Stroke.IsValid());

		// meshTerrainBrush | call Paint(unreal.find("<label>"), "Red", [-800000, 2000000, 0], 1000, 1, 0.3)
		Stroke = SculptSpecStroke(TEXT("Paint"), Label, { TEXT("\"Red\""), Point, TEXT("1000"), TEXT("1"), TEXT("0.3") }, &Json);
		if (!TestTrue(*FString::Printf(TEXT("Paint (%s)"), *Json), Stroke.IsValid()))
			return;
		TestTrue(TEXT("into the sculpt's brush"), !Stroke->GetBoolField(TEXT("NewBrush")) && Stroke->GetStringField(TEXT("Brush")) == Label + TEXT("_Brush"));
		TestTrue(TEXT("After is the weight"), FMath::IsNearlyEqual(Stroke->GetNumberField(TEXT("After")), 1.0, 0.001));
		TestTrue(*FString::Printf(TEXT("Red 1 at the centre (%.3f)"), SculptSpecChannel(Terrain, Center.X + 30.0, Center.Y + 20.0, TEXT("Red"))),
			FMath::IsNearlyEqual(SculptSpecChannel(Terrain, Center.X + 30.0, Center.Y + 20.0, TEXT("Red")), 1.0f, 0.02f));
		TestTrue(TEXT("Red 0 outside the radius"), FMath::IsNearlyZero(SculptSpecChannel(Terrain, Center.X + 2000.0, Center.Y, TEXT("Red")), 0.01f));
		TestTrue(TEXT("the sculpt is kept"), FMath::IsNearlyEqual(SculptSpecHeight(Terrain, Center.X, Center.Y), 200.0, 1.0));
		const TArray<UE::MeshPartition::UProjectMeshLayersModifier*> Brushes = SculptSpecBrushes(Terrain);
		if (TestEqual(TEXT("one brush modifier"), Brushes.Num(), 1))
		{
			const TArray<UE::MeshPartition::FSculptLayerModifierWeightAttributeEntry> Entries = Brushes[0]->GetChannelEntries();
			TestTrue(TEXT("the brush writes the Red channel (Set), like the Paint tool"), Entries.Num() == 1 && Entries[0].ChannelName.GetName() == TEXT("Red")
				&& Entries[0].Method == UE::MeshPartition::ESculptLayerSetWeightChannelMethod::Set);
		}

		// Half weight elsewhere, then Erase the centre.
		Stroke = SculptSpecStroke(TEXT("Paint"), Label, { TEXT("\"Green\""), Point, TEXT("1000"), TEXT("0.5") }, &Json);
		TestTrue(*FString::Printf(TEXT("Paint Green 0.5 (%s)"), *Json), Stroke.IsValid() && FMath::IsNearlyEqual(SculptSpecChannel(Terrain, Center.X, Center.Y, TEXT("Green")), 0.5f, 0.02f));
		Stroke = SculptSpecStroke(TEXT("Erase"), Label, { TEXT("\"Red\""), Point, TEXT("400"), TEXT("0") }, &Json);
		TestTrue(*FString::Printf(TEXT("Erase (%s)"), *Json), Stroke.IsValid() && FMath::IsNearlyZero(Stroke->GetNumberField(TEXT("After"))));
		TestTrue(TEXT("Red erased at the centre"), FMath::IsNearlyZero(SculptSpecChannel(Terrain, Center.X, Center.Y, TEXT("Red")), 0.01f));
		TestTrue(TEXT("Red kept outside the erased disc"), SculptSpecChannel(Terrain, Center.X + 600.0, Center.Y, TEXT("Red")) > 0.5f);
		TestTrue(TEXT("Green kept"), FMath::IsNearlyEqual(SculptSpecChannel(Terrain, Center.X, Center.Y, TEXT("Green")), 0.5f, 0.02f));

		// Erasing where nothing is painted changes nothing: a no-op, not an error.
		Stroke = SculptSpecStroke(TEXT("Erase"), Label, { TEXT("\"Blue\""), Point, TEXT("300") }, &Json);
		TestTrue(*FString::Printf(TEXT("Erase of an unpainted channel is a no-op (%s)"), *Json), Stroke.IsValid() && Stroke->GetIntegerField(TEXT("Vertices")) == 0);
	});

	It("undoes and redoes a stroke (one undo step per call)", [this]()
	{
		const FString Label = SculptSpecLabel(TEXT("SculptUndo"));
		const FVector Center(-900000.0, 2000000.0, 0.0);
		AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, Center, 4000.0, 4000.0, 40, 40);
		ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
		if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestNotNull(TEXT("editor"), GEditor))
			return;
		const FString Point = TEXT("[-900000, 2000000]");
		TestTrue(TEXT("first stroke"), SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Raise\""), Point, TEXT("600"), TEXT("200") }).IsValid());
		TestTrue(TEXT("second stroke"), SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Raise\""), Point, TEXT("600"), TEXT("100") }).IsValid());
		TestTrue(TEXT("both strokes"), FMath::IsNearlyEqual(SculptSpecHeight(Terrain, Center.X, Center.Y), 300.0, 1.0));

		TestTrue(TEXT("undo the second stroke"), GEditor->UndoTransaction());
		TestTrue(TEXT("rebuilt after undo"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
		TestTrue(*FString::Printf(TEXT("back to the first stroke (%.2f)"), SculptSpecHeight(Terrain, Center.X, Center.Y)), FMath::IsNearlyEqual(SculptSpecHeight(Terrain, Center.X, Center.Y), 200.0, 1.0));
		TestTrue(TEXT("undo the first stroke"), GEditor->UndoTransaction());
		TestTrue(TEXT("rebuilt after the second undo"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
		TestTrue(TEXT("flat again"), FMath::IsNearlyZero(SculptSpecHeight(Terrain, Center.X, Center.Y), 1.0));
		TestEqual(TEXT("the brush modifier is gone with the first stroke"), SculptSpecBrushes(Terrain).Num(), 0);

		TestTrue(TEXT("redo"), GEditor->RedoTransaction() && GEditor->RedoTransaction());
		TestTrue(TEXT("rebuilt after redo"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
		TestTrue(*FString::Printf(TEXT("both strokes again (%.2f)"), SculptSpecHeight(Terrain, Center.X, Center.Y)), FMath::IsNearlyEqual(SculptSpecHeight(Terrain, Center.X, Center.Y), 300.0, 1.0));
	});

	It("flattens over a noise modifier: the brush applies after the terrain's other modifiers", [this]()
	{
		const FString Label = SculptSpecLabel(TEXT("SculptNoise"));
		const FVector Center(-1000000.0, 2000000.0, 0.0);
		AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, Center, 8000.0, 8000.0, 80, 80);
		ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
		if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestNotNull(TEXT("editor"), GEditor))
			return;
		UActorFactory* Factory = GEditor->FindActorFactoryByClass(UE::MeshPartition::UModifierActorFactory::StaticClass());
		AActor* Actor = Factory ? Factory->CreateActor(UE::MeshPartition::UNoiseModifier::StaticClass(), Terrain->GetWorld()->GetCurrentLevel(), FTransform(Center)) : nullptr;
		UE::MeshPartition::UNoiseModifier* Noise = Actor ? Actor->FindComponentByClass<UE::MeshPartition::UNoiseModifier>() : nullptr;
		if (!TestNotNull(TEXT("noise placed"), Noise))
			return;
		Actor->SetActorLabel(Label + TEXT("_Noise"));
		Noise->SetAffectedMeshPartition(Terrain);
		// meshTerrainModifiers | where Owner.Label == "<label>_Noise" | set Intensity = 600, UnscaledCoverage = {X: 8000, Y: 8000, Z: 50000}
		TestTrue(TEXT("shape the noise"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifiers"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Owner","Label"]},{"lit":"%s_Noise"}]}},{"op":"set","assign":[{"target":{"path":["Intensity"]},"value":{"lit":600}},{"target":{"path":["UnscaledCoverage"]},"value":{"object":[{"key":"X","value":{"lit":8000}},{"key":"Y","value":{"lit":8000}},{"key":"Z","value":{"lit":50000}}]}}]}]})RQL"), *Label), true));
		TestTrue(TEXT("noise built"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
		double Relief = 0.0;
		for (const FVector2D Offset : { FVector2D(130.0, 270.0), FVector2D(-410.0, 90.0), FVector2D(260.0, -330.0) })
			Relief = FMath::Max(Relief, FMath::Abs(SculptSpecHeight(Terrain, Center.X + Offset.X, Center.Y + Offset.Y)));
		TestTrue(*FString::Printf(TEXT("the noise shapes the ground (%.1f cm)"), Relief), Relief > 5.0);

		FString Json;
		const TSharedPtr<FJsonObject> Stroke = SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Flatten\""), TEXT("[-1000000, 2000000]"), TEXT("1000"), TEXT("120"), TEXT("1"), TEXT("0.2") }, &Json);
		TestTrue(*FString::Printf(TEXT("Flatten over the noise (%s)"), *Json), Stroke.IsValid());
		const UE::MeshPartition::UProjectMeshLayersModifier* Brush = SculptSpecBrushes(Terrain).Num() == 1 ? SculptSpecBrushes(Terrain)[0] : nullptr;
		TestTrue(TEXT("the brush applies after the noise"), Brush && Brush->GetPriority() > Noise->GetPriority());
		for (const FVector2D Offset : { FVector2D(130.0, 270.0), FVector2D(-410.0, 90.0), FVector2D(260.0, -330.0) })
		{
			const double Height = SculptSpecHeight(Terrain, Center.X + Offset.X, Center.Y + Offset.Y);
			// The collision is the simplified (Nanite fallback) mesh: flat ground stays within a few cm of the model.
			TestTrue(*FString::Printf(TEXT("flat at 120 cm at %s (%.2f)"), *Offset.ToString(), Height), FMath::IsNearlyEqual(Height, 120.0, 5.0));
		}
	});

	It("sculpts and paints across a section border into one brush modifier", [this]()
	{
		// 600 x 600 quads: more than the Create tool's 500 x 500 x 2 triangles per section, so two sections, split on X.
		const FString Label = SculptSpecLabel(TEXT("SculptSeam"));
		const FVector Center(-1100000.0, 2000000.0, 0.0);
		AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, Center, 12000.0, 12000.0, 600, 600);
		ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
		if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestTrue(TEXT("built"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 120.0)))
			return;
		const TArray<UnrealAIMeshTerrain::APreviewSection*> Sections = UnrealAIMeshTerrain::PreviewSections(Terrain);
		if (!TestEqual(TEXT("two sections"), Sections.Num(), 2))
			return;
		FString Json;
		TSharedPtr<FJsonObject> Stroke = SculptSpecStroke(TEXT("Sculpt"), Label, { TEXT("\"Raise\""), TEXT("[-1100000, 2000000]"), TEXT("600"), TEXT("250"), TEXT("1"), TEXT("0.3") }, &Json);
		TestTrue(*FString::Printf(TEXT("Raise across the seam (%s)"), *Json), Stroke.IsValid() && Stroke->GetBoolField(TEXT("Ready")));
		Stroke = SculptSpecStroke(TEXT("Paint"), Label, { TEXT("\"Yellow\""), TEXT("[-1100000, 2000000]"), TEXT("600"), TEXT("1"), TEXT("0.3") }, &Json);
		TestTrue(*FString::Printf(TEXT("Paint across the seam (%s)"), *Json), Stroke.IsValid() && !Stroke->GetBoolField(TEXT("NewBrush")));
		TestEqual(TEXT("one brush modifier"), SculptSpecBrushes(Terrain).Num(), 1);
		for (const double Dx : { -250.0, 250.0 })
		{
			// A few cm off the seam itself (traces exactly on a section seam can hit its edge).
			const double Height = SculptSpecHeight(Terrain, Center.X + Dx, Center.Y + 30.0);
			TestTrue(*FString::Printf(TEXT("raised on both sides (x %+.0f: %.2f)"), Dx, Height), FMath::IsNearlyEqual(Height, 250.0, 2.0));
			TestTrue(TEXT("painted on both sides"), SculptSpecChannel(Terrain, Center.X + Dx, Center.Y + 30.0, TEXT("Yellow")) > 0.95f);
		}
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
