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
#include "UnrealAIMeshTerrainWaterCommon.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include <limits>

#include "MeshPartition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionRectangleGenerator.h"
#include "MeshPartitionWaterModifier.h"

#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterSplineComponent.h"
#include "WaterZoneActor.h"

// ============================================================================================
//  UE Automation spec for water on Mesh Terrain (UnrealAIMeshTerrainWater): `meshTerrainWater | call CreateLake /
//  CreateRiver / CreateOcean / Attach / Detach / Delete / WaitForBuild`, the MeshTerrainWater status member and its
//  Problem texts, the meshTerrainWaterBodies source and the WaitForBuild / ground helpers.
//
//  Each test makes its own flat Mesh Terrain far from the origin with the engine's rectangle generator (this extension
//  does not depend on UnrealAIMeshTerrain) and deletes it, its water and the water zone / ocean it made on exit. Plans are
//  the QueryPlans Rql.Parser produces for the query in the comment above them.
//
//  Run via:  Automation RunTests UnrealAIMeshTerrainWater
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIMeshTerrainWaterSpec, "UnrealAIMeshTerrainWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	using AMeshPartition = UE::MeshPartition::AMeshPartition;

	static FString WaterSpecJson(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** `meshTerrainWater | call <Method>(<Args>)` as a plan; @p Args are plan argument JSON values. */
	static FString WaterSpecCall(const FString& Method, const FString& Args)
	{
		return FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainWater"]},"stages":[{"op":"call","method":"%s","args":[%s]}]})RQL"), *Method, *Args);
	}

	/** unreal.find("<Label>") as a plan argument. */
	static FString WaterSpecFind(const FString& Label)
	{
		return FString::Printf(TEXT(R"({"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]})"), *Label);
	}

	static FString WaterSpecLit(const FString& Text)
	{
		return FString::Printf(TEXT(R"({"lit":"%s"})"), *Text);
	}

	static FString WaterSpecNum(double Value)
	{
		return FString::Printf(TEXT(R"({"lit":%.17g})"), Value);
	}

	static FString WaterSpecVec(const FVector& V)
	{
		return FString::Printf(TEXT(R"({"list":[{"lit":%.17g},{"lit":%.17g},{"lit":%.17g}]})"), V.X, V.Y, V.Z);
	}

	static FString WaterSpecLabel(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Short));
	}

	static UWorld* WaterSpecWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/** The actor of class T labelled @p Label in the editor world (null when none). */
	template <typename T>
	static T* WaterSpecFindActor(const FString& Label)
	{
		for (TActorIterator<T> It(WaterSpecWorld()); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == Label)
				return *It;
		}
		return nullptr;
	}

	/**
	 * A flat Mesh Terrain the way Mesh Terrain Mode's Create tool makes one: @p SizeMeters square, one quad per metre, its
	 * ground at @p Center Z, built when it returns.
	 */
	static AMeshPartition* WaterSpecTerrain(const FString& Label, const FVector& Center, int32 SizeMeters)
	{
		UWorld* World = WaterSpecWorld();
		if (!World)
			return nullptr;
		UE::MeshPartition::FMegaMeshRectangleGeneratorParams Params;
		Params.MeshSize = FVector2D(SizeMeters * 100.0, SizeMeters * 100.0);
		Params.MeshResolution = FInt32Point(SizeMeters, SizeMeters);
		Params.SectionsResolution = UE::MeshPartition::FRectangleGeneratorUtils::ComputeSectionResolution(Params.MeshResolution, 500 * 500 * 2);
		Params.bSaveAndUnload = false;
		Params.LocationVolumesResolution = FInt32Point(0, 0);
		UE::MeshPartition::FMegaMeshRectangleGenerator Generator(Params, *World, FTransform(Center));
		if (!Generator.Generate(nullptr) || !Generator.GetMegaMesh())
			return nullptr;
		AMeshPartition* Terrain = Generator.GetMegaMesh();
		Terrain->SetActorLabel(Label);
		FString Pending;
		UnrealAIMeshTerrainWater::WaitForBuild(Terrain, 120.0, Pending);
		return Terrain;
	}

	/** Delete @p Terrain with its base meshes and every water body of the level labelled with one of @p Labels. */
	static void WaterSpecCleanup(const FString& TerrainLabel, TConstArrayView<FString> Labels)
	{
		UWorld* World = WaterSpecWorld();
		if (!World)
			return;
		TArray<AActor*> Doomed;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
				continue;
			const AActor* Parent = Actor->GetAttachParentActor();
			if (Actor->GetActorLabel() == TerrainLabel || (Parent && Parent->GetActorLabel() == TerrainLabel) || Labels.Contains(Actor->GetActorLabel()))
				Doomed.Add(Actor);
		}
		for (AActor* Actor : Doomed)
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
	}

	/** The ground of @p Terrain at X, Y (collision), NaN off it. */
	static double WaterSpecGround(const AMeshPartition* Terrain, double X, double Y)
	{
		const TOptional<double> Ground = UnrealAIMeshTerrainWater::GroundHeight(Terrain, X, Y);
		return Ground.Get(std::numeric_limits<double>::quiet_NaN());
	}

	/** Whether the editor has a water zone now (a test that made one deletes it). */
	static AWaterZone* WaterSpecZone()
	{
		return Cast<AWaterZone>(UnrealAIMeshTerrainWater::FindFirst(WaterSpecWorld(), AWaterZone::StaticClass()));
	}
END_DEFINE_SPEC(FUnrealAIMeshTerrainWaterSpec)

void FUnrealAIMeshTerrainWaterSpec::Define()
{
	Describe("RQL", [this]()
	{
		It("adds meshTerrainWater with its operations to the schema and its guide to the skill", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("MeshTerrainWaterOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Method : {
				TEXT("CreateLake(MeshPartition Terrain, string Label, Vector Center, double Radius, double Depth, MaterialInterface Material, double TimeoutSeconds)"),
				TEXT("CreateRiver(MeshPartition Terrain, string Label, List<Vector> Points, double Width, double Depth, MaterialInterface Material, double TimeoutSeconds)"),
				TEXT("CreateOcean(string Label, double Height, MaterialInterface Material)"),
				TEXT("Attach(WaterBody Body, MeshPartition Terrain, double TimeoutSeconds)"),
				TEXT("Detach(WaterBody Body, double TimeoutSeconds)"),
				TEXT("Delete(WaterBody Body, double TimeoutSeconds)"),
				TEXT("WaitForBuild(WaterBody Body, double TimeoutSeconds)"),
				TEXT("SetZone(double SizeMeters, Vector Center)"),
				TEXT("SetMaterial(WaterBody Body, MaterialInterface Material)"),
				TEXT("EnableMeshTerrainWater()") })
			{
				TestTrue(*FString::Printf(TEXT("schema lists %s (%s)"), Method, *Schema.Left(600)), Schema.Contains(Method));
			}
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			for (const TCHAR* Text : { TEXT("Water on Mesh Terrain"), TEXT("meshTerrainWater | call CreateLake("), TEXT("call CreateOcean("), TEXT("meshTerrainWaterBodies") })
				TestTrue(*FString::Printf(TEXT("skill mentions %s"), Text), Skill.Contains(Text));
		});

		It("finds the guide by the extension's short name", [this]()
		{
			bool bOk = false;
			const FString Guide = UnrealMcpRqlTools::Schema(FString(), TEXT("mesh-terrain-water"), bOk);
			TestTrue(TEXT("guide ok"), bOk);
			TestTrue(*FString::Printf(TEXT("guide text (%s)"), *Guide.Left(300)), Guide.Contains(TEXT("Water on Mesh Terrain")) && Guide.Contains(TEXT("CreateRiver")));
		});
	});

	Describe("CreateLake", [this]()
	{
		It("places a lake whose LakeModifier digs a bed Depth below its surface, the shore at the surface", [this]()
		{
			const FString TerrainLabel = WaterSpecLabel(TEXT("SpecLakeTerrain"));
			const FString Label = WaterSpecLabel(TEXT("SpecLake"));
			const bool bHadZone = WaterSpecZone() != nullptr;
			ON_SCOPE_EXIT
			{
				WaterSpecCleanup(TerrainLabel, { Label });
				if (!bHadZone && WaterSpecZone())
					WaterSpecWorld()->EditorDestroyActor(WaterSpecZone(), true);
			};
			const FVector Origin(900000.0, -900000.0, 500.0);
			AMeshPartition* Terrain = WaterSpecTerrain(TerrainLabel, Origin, 200);
			if (!TestNotNull(TEXT("terrain"), Terrain))
				return;
			TestTrue(TEXT("flat before"), FMath::IsNearlyEqual(WaterSpecGround(Terrain, Origin.X, Origin.Y), 500.0, 1.0));
			// meshTerrainWater | call CreateLake(unreal.find("<terrain>"), "<label>", [900000, -900000, 500], 3000, 800)
			TSharedPtr<FJsonObject> Result;
			const bool bOk = UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateLake"), FString::Join(TArray<FString>{
				WaterSpecFind(TerrainLabel), WaterSpecLit(Label), WaterSpecVec(Origin), WaterSpecNum(3000.0), WaterSpecNum(800.0) }, TEXT(","))), true, &Result);
			const FString Json = WaterSpecJson(Result);
			if (!TestTrue(*FString::Printf(TEXT("CreateLake (%s)"), *Json), bOk))
				return;
			AWaterBody* Lake = WaterSpecFindActor<AWaterBody>(Label);
			if (!TestNotNull(TEXT("lake labelled"), Lake))
				return;
			TestTrue(*FString::Printf(TEXT("answer names the terrain, the modifier and a built state (%s)"), *Json),
				Json.Contains(TerrainLabel) && Json.Contains(TEXT("LakeModifier")) && Json.Contains(TEXT("built")));
			TestEqual(TEXT("one water modifier"), UnrealAIMeshTerrainWater::WaterModifiers(Lake).Num(), 1);
			TestTrue(TEXT("bound to the terrain"), UnrealAIMeshTerrainWater::TerrainOf(Lake) == Terrain);
			TestTrue(TEXT("affects the terrain"), Lake->GetWaterBodyComponent()->bAffectsLandscape);
			TestEqual(TEXT("ChannelDepth is Depth"), static_cast<double>(Lake->GetWaterBodyComponent()->CurveSettings.ChannelDepth), 800.0);
			TestNotNull(TEXT("a water zone renders it"), Lake->GetWaterBodyComponent()->GetWaterZone());
			TestTrue(TEXT("terrain built"), UnrealAIMeshTerrainWater::BuildState(Terrain).IsReady());
			const double Centre = WaterSpecGround(Terrain, Origin.X, Origin.Y);
			const double Shore = WaterSpecGround(Terrain, Origin.X + 3000.0, Origin.Y);
			const double Outside = WaterSpecGround(Terrain, Origin.X + 6000.0, Origin.Y);
			TestTrue(*FString::Printf(TEXT("bed about Depth below the surface (%g)"), Centre), FMath::IsNearlyEqual(Centre, 500.0 - 800.0, 60.0));
			TestTrue(*FString::Printf(TEXT("shore at the surface (%g)"), Shore), FMath::IsNearlyEqual(Shore, 500.0, 30.0));
			TestTrue(*FString::Printf(TEXT("ground beyond untouched (%g)"), Outside), FMath::IsNearlyEqual(Outside, 500.0, 1.0));
			TestEqual(TEXT("the terrain lists it"), UnrealAIMeshTerrainWater::BodiesOf(Terrain).Num(), 1);
		});

		It("refuses bad arguments before placing anything", [this]()
		{
			const FString TerrainLabel = WaterSpecLabel(TEXT("SpecLakeBad"));
			const FString Label = WaterSpecLabel(TEXT("SpecBad"));
			ON_SCOPE_EXIT { WaterSpecCleanup(TerrainLabel, { Label }); };
			const FVector Origin(1000000.0, -900000.0, 0.0);
			if (!TestNotNull(TEXT("terrain"), WaterSpecTerrain(TerrainLabel, Origin, 100)))
				return;
			const auto Lake = [&](const FVector& Center, double Radius, double Depth, const FString& BodyLabel)
			{
				TSharedPtr<FJsonObject> Result;
				const bool bOk = UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateLake"), FString::Join(TArray<FString>{
					WaterSpecFind(TerrainLabel), WaterSpecLit(BodyLabel), WaterSpecVec(Center), WaterSpecNum(Radius), WaterSpecNum(Depth) }, TEXT(","))), true, &Result);
				return bOk ? FString() : WaterSpecJson(Result);
			};
			const TPair<FString, FString> Cases[] = {
				{ Lake(Origin + FVector(90000.0, 0.0, 0.0), 1000.0, 0.0, Label), TEXT("is off") },
				{ Lake(Origin, 50.0, 0.0, Label), TEXT("Radius 50 is outside") },
				{ Lake(Origin, 1000.0, 5.0, Label), TEXT("Depth 5 is outside") },
				{ Lake(Origin, 1000.0, 0.0, TEXT(" ")), TEXT("give the water body a label") },
			};
			for (const TPair<FString, FString>& Case : Cases)
				TestTrue(*FString::Printf(TEXT("refused with '%s' (%s)"), *Case.Value, *Case.Key), Case.Key.Contains(Case.Value));
			TestNull(TEXT("no lake placed"), WaterSpecFindActor<AWaterBody>(Label));
		});
	});

	Describe("CreateRiver", [this]()
	{
		It("carves the river's bed Depth below its surface, Width wide", [this]()
		{
			const FString TerrainLabel = WaterSpecLabel(TEXT("SpecRiverTerrain"));
			const FString Label = WaterSpecLabel(TEXT("SpecRiver"));
			const bool bHadZone = WaterSpecZone() != nullptr;
			ON_SCOPE_EXIT
			{
				WaterSpecCleanup(TerrainLabel, { Label });
				if (!bHadZone && WaterSpecZone())
					WaterSpecWorld()->EditorDestroyActor(WaterSpecZone(), true);
			};
			const FVector Origin(900000.0, -800000.0, 0.0);
			AMeshPartition* Terrain = WaterSpecTerrain(TerrainLabel, Origin, 200);
			if (!TestNotNull(TEXT("terrain"), Terrain))
				return;
			// meshTerrainWater | call CreateRiver(unreal.find("<terrain>"), "<label>", [[-8000, 0, -50], [0, 0, -100], [8000, 0, -150]] + origin, 1200, 400)
			const FString Points = FString::Printf(TEXT(R"({"list":[%s,%s,%s]})"), *WaterSpecVec(Origin + FVector(-8000.0, 0.0, -50.0)),
				*WaterSpecVec(Origin + FVector(0.0, 0.0, -100.0)), *WaterSpecVec(Origin + FVector(8000.0, 0.0, -150.0)));
			TSharedPtr<FJsonObject> Result;
			const bool bOk = UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateRiver"), FString::Join(TArray<FString>{
				WaterSpecFind(TerrainLabel), WaterSpecLit(Label), Points, WaterSpecNum(1200.0), WaterSpecNum(400.0) }, TEXT(","))), true, &Result);
			if (!TestTrue(*FString::Printf(TEXT("CreateRiver (%s)"), *WaterSpecJson(Result)), bOk))
				return;
			AWaterBody* River = WaterSpecFindActor<AWaterBody>(Label);
			if (!TestNotNull(TEXT("river labelled"), River))
				return;
			TestEqual(TEXT("a River"), UnrealAIMeshTerrainWater::WaterType(River), FString(TEXT("River")));
			TestTrue(TEXT("its RiverModifier shapes the terrain"), UnrealAIMeshTerrainWater::TerrainOf(River) == Terrain
				&& UnrealAIMeshTerrainWater::MatchingModifier(River)->GetClass()->GetName() == TEXT("RiverModifier"));
			const double Bed = WaterSpecGround(Terrain, Origin.X, Origin.Y);
			const double Bank = WaterSpecGround(Terrain, Origin.X, Origin.Y + 600.0);
			const double Beside = WaterSpecGround(Terrain, Origin.X, Origin.Y + 3000.0);
			TestTrue(*FString::Printf(TEXT("bed Depth below the surface (%g)"), Bed), FMath::IsNearlyEqual(Bed, -100.0 - 400.0, 40.0));
			TestTrue(*FString::Printf(TEXT("bank (half the width out) at the surface (%g)"), Bank), FMath::IsNearlyEqual(Bank, -100.0, 40.0));
			TestTrue(*FString::Printf(TEXT("ground beside it barely touched (%g)"), Beside), FMath::Abs(Beside) < 30.0);
		});

		It("refuses fewer than two points and a bad width", [this]()
		{
			const FString TerrainLabel = WaterSpecLabel(TEXT("SpecRiverBad"));
			ON_SCOPE_EXIT { WaterSpecCleanup(TerrainLabel, {}); };
			const FVector Origin(1000000.0, -800000.0, 0.0);
			if (!TestNotNull(TEXT("terrain"), WaterSpecTerrain(TerrainLabel, Origin, 100)))
				return;
			TSharedPtr<FJsonObject> Result;
			TestFalse(TEXT("one point refused"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateRiver"), FString::Join(TArray<FString>{
				WaterSpecFind(TerrainLabel), WaterSpecLit(TEXT("R")), FString::Printf(TEXT(R"({"list":[%s]})"), *WaterSpecVec(Origin)) }, TEXT(","))), true, &Result));
			TestTrue(*FString::Printf(TEXT("says why (%s)"), *WaterSpecJson(Result)), WaterSpecJson(Result).Contains(TEXT("2 .. 200 points")));
			TestFalse(TEXT("width 50 refused"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateRiver"), FString::Join(TArray<FString>{
				WaterSpecFind(TerrainLabel), WaterSpecLit(TEXT("R")), FString::Printf(TEXT(R"({"list":[%s,%s]})"), *WaterSpecVec(Origin), *WaterSpecVec(Origin + FVector(1000.0, 0.0, 0.0))),
				WaterSpecNum(50.0) }, TEXT(","))), true, &Result));
			TestTrue(*FString::Printf(TEXT("says why (%s)"), *WaterSpecJson(Result)), WaterSpecJson(Result).Contains(TEXT("Width 50 is outside")));
		});
	});

	Describe("CreateOcean", [this]()
	{
		It("places one ocean with no modifier and refuses a second", [this]()
		{
			if (UnrealAIMeshTerrainWater::FindFirst(WaterSpecWorld(), AWaterBodyOcean::StaticClass()))
			{
				AddInfo(TEXT("the level already has an ocean: skipped"));
				return;
			}
			const FString Label = WaterSpecLabel(TEXT("SpecOcean"));
			const bool bHadZone = WaterSpecZone() != nullptr;
			ON_SCOPE_EXIT
			{
				WaterSpecCleanup(FString(TEXT("-")), { Label });
				if (!bHadZone && WaterSpecZone())
					WaterSpecWorld()->EditorDestroyActor(WaterSpecZone(), true);
			};
			// meshTerrainWater | call SetZone(500, [0, 0, 0]): no terrain needed with a size
			TestTrue(TEXT("SetZone"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("SetZone"), WaterSpecNum(500.0)), true));
			// meshTerrainWater | call CreateOcean("<label>", -250)
			TSharedPtr<FJsonObject> Result;
			TestTrue(*FString::Printf(TEXT("CreateOcean (%s)"), *WaterSpecJson(Result)), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateOcean"),
				WaterSpecLit(Label) + TEXT(",") + WaterSpecNum(-250.0)), true, &Result));
			AWaterBody* Ocean = WaterSpecFindActor<AWaterBody>(Label);
			if (!TestNotNull(TEXT("ocean labelled"), Ocean))
				return;
			TestEqual(TEXT("an Ocean"), UnrealAIMeshTerrainWater::WaterType(Ocean), FString(TEXT("Ocean")));
			TestEqual(TEXT("no water modifier"), UnrealAIMeshTerrainWater::WaterModifiers(Ocean).Num(), 0);
			TestTrue(TEXT("its surface at Height"), FMath::IsNearlyEqual(Ocean->GetActorLocation().Z, -250.0, 0.5));
			TestFalse(TEXT("a second is refused"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateOcean"), WaterSpecLit(Label + TEXT("2"))), true, &Result));
			TestTrue(*FString::Printf(TEXT("one per world (%s)"), *WaterSpecJson(Result)), WaterSpecJson(Result).Contains(TEXT("already has an ocean")));
		});
	});

	Describe("Attach, Detach, Delete", [this]()
	{
		It("attaches a lake placed without a modifier, detaches it (ground back), attaches it again and deletes it", [this]()
		{
			const FString TerrainLabel = WaterSpecLabel(TEXT("SpecAttachTerrain"));
			const FString Label = WaterSpecLabel(TEXT("SpecHand"));
			const bool bHadZone = WaterSpecZone() != nullptr;
			ON_SCOPE_EXIT
			{
				WaterSpecCleanup(TerrainLabel, { Label });
				if (!bHadZone && WaterSpecZone())
					WaterSpecWorld()->EditorDestroyActor(WaterSpecZone(), true);
			};
			const FVector Origin(900000.0, -700000.0, 0.0);
			AMeshPartition* Terrain = WaterSpecTerrain(TerrainLabel, Origin, 200);
			if (!TestNotNull(TEXT("terrain"), Terrain))
				return;
			// A lake placed as the Place Actors panel does (the Water defaults), with its default spline, and no modifier.
			// meshTerrainWater | call CreateLake(...) is not used: the body comes from elsewhere.
			UWorld* World = WaterSpecWorld();
			AWaterBodyLake* Lake = World->SpawnActor<AWaterBodyLake>(Origin, FRotator::ZeroRotator);
			if (!TestNotNull(TEXT("lake"), Lake))
				return;
			Lake->SetActorLabel(Label);
			Lake->GetWaterBodyComponent()->CurveSettings.ChannelDepth = 500.f;
			const FString Before = UnrealAIMeshTerrainWater::Problem(Lake);
			TestTrue(*FString::Printf(TEXT("Problem names Attach (%s)"), *Before), Before.Contains(TEXT("does not shape any Mesh Terrain")) && Before.Contains(TEXT("call Attach(")));

			const FString Attach = WaterSpecCall(TEXT("Attach"), WaterSpecFind(Label) + TEXT(",") + WaterSpecFind(TerrainLabel));
			TSharedPtr<FJsonObject> Result;
			TestTrue(*FString::Printf(TEXT("Attach (%s)"), *WaterSpecJson(Result)), UnrealMcpRqlTools::RunPlan(Attach, true, &Result));
			TestTrue(TEXT("bound"), UnrealAIMeshTerrainWater::TerrainOf(Lake) == Terrain);
			// Inside the lake: the middle of its (default, off-centre) spline.
			FVector Inside = FVector::ZeroVector;
			const UWaterSplineComponent* Spline = Lake->GetWaterSpline();
			for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
				Inside += Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World) / Spline->GetNumberOfSplinePoints();
			const double Dug = WaterSpecGround(Terrain, Inside.X, Inside.Y);
			TestTrue(*FString::Printf(TEXT("the ground inside is dug (%g)"), Dug), Dug < -300.0);

			// meshTerrainWater | call Detach(unreal.find("<label>"))
			TestTrue(*FString::Printf(TEXT("Detach (%s)"), *WaterSpecJson(Result)), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("Detach"), WaterSpecFind(Label)), true, &Result));
			TestEqual(TEXT("no modifier left"), UnrealAIMeshTerrainWater::WaterModifiers(Lake).Num(), 0);
			const double Back = WaterSpecGround(Terrain, Inside.X, Inside.Y);
			TestTrue(*FString::Printf(TEXT("the ground is back (%g)"), Back), FMath::Abs(Back) < 1.0);

			// Attach again: the removed modifier may still hold its name until garbage collection.
			TestTrue(*FString::Printf(TEXT("Attach again (%s)"), *WaterSpecJson(Result)), UnrealMcpRqlTools::RunPlan(Attach, true, &Result));
			TestEqual(TEXT("one modifier"), UnrealAIMeshTerrainWater::WaterModifiers(Lake).Num(), 1);
			TestTrue(TEXT("dug again"), WaterSpecGround(Terrain, Inside.X, Inside.Y) < -300.0);

			// meshTerrainWater | call Delete(unreal.find("<label>"))
			TestTrue(*FString::Printf(TEXT("Delete (%s)"), *WaterSpecJson(Result)), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("Delete"), WaterSpecFind(Label)), true, &Result));
			TestNull(TEXT("the lake is gone"), WaterSpecFindActor<AWaterBody>(Label));
			const double Restored = WaterSpecGround(Terrain, Inside.X, Inside.Y);
			TestTrue(*FString::Printf(TEXT("the ground is back after Delete (%g)"), Restored), FMath::Abs(Restored) < 1.0);
		});

		It("refuses to attach an ocean and repairs a disabled or switched-off modifier", [this]()
		{
			const FString TerrainLabel = WaterSpecLabel(TEXT("SpecRepairTerrain"));
			const FString Label = WaterSpecLabel(TEXT("SpecRepair"));
			const bool bHadZone = WaterSpecZone() != nullptr;
			ON_SCOPE_EXIT
			{
				WaterSpecCleanup(TerrainLabel, { Label });
				if (!bHadZone && WaterSpecZone())
					WaterSpecWorld()->EditorDestroyActor(WaterSpecZone(), true);
			};
			const FVector Origin(900000.0, -600000.0, 0.0);
			AMeshPartition* Terrain = WaterSpecTerrain(TerrainLabel, Origin, 200);
			if (!TestNotNull(TEXT("terrain"), Terrain))
				return;
			// meshTerrainWater | call CreateLake(unreal.find("<terrain>"), "<label>", origin, 2000)
			TSharedPtr<FJsonObject> Result;
			if (!TestTrue(TEXT("CreateLake"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateLake"), FString::Join(TArray<FString>{
				WaterSpecFind(TerrainLabel), WaterSpecLit(Label), WaterSpecVec(Origin), WaterSpecNum(2000.0) }, TEXT(","))), true, &Result)))
				return;
			AWaterBody* Lake = WaterSpecFindActor<AWaterBody>(Label);
			if (!TestNotNull(TEXT("lake"), Lake))
				return;
			UnrealAIMeshTerrainWater::UWaterModifier* Modifier = UnrealAIMeshTerrainWater::MatchingModifier(Lake);
			Modifier->SetIsDisabledFlag(true);
			TestTrue(*FString::Printf(TEXT("Problem: disabled (%s)"), *UnrealAIMeshTerrainWater::Problem(Lake)), UnrealAIMeshTerrainWater::Problem(Lake).Contains(TEXT("is disabled")));
			Modifier->SetIsDisabledFlag(false);
			Lake->GetWaterBodyComponent()->bAffectsLandscape = false;
			TestTrue(*FString::Printf(TEXT("Problem: Affects Landscape off (%s)"), *UnrealAIMeshTerrainWater::Problem(Lake)), UnrealAIMeshTerrainWater::Problem(Lake).Contains(TEXT("bAffectsLandscape")));
			TestTrue(TEXT("Attach repairs it"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("Attach"), WaterSpecFind(Label) + TEXT(",") + WaterSpecFind(TerrainLabel)), true, &Result));
			TestTrue(TEXT("on again"), Lake->GetWaterBodyComponent()->bAffectsLandscape && !Modifier->GetIsDisabledFlag());
			const FString After = UnrealAIMeshTerrainWater::Problem(Lake);
			TestFalse(*FString::Printf(TEXT("no modifier problem left (%s)"), *After), After.Contains(TEXT("Attach(")));

			// An ocean (any in the level, else a spec one): Attach refuses it.
			AWaterBody* Ocean = Cast<AWaterBody>(UnrealAIMeshTerrainWater::FindFirst(WaterSpecWorld(), AWaterBodyOcean::StaticClass()));
			const FString OceanLabel = Label + TEXT("Ocean");
			ON_SCOPE_EXIT { WaterSpecCleanup(FString(TEXT("-")), { OceanLabel }); };
			if (!Ocean)
			{
				Ocean = WaterSpecWorld()->SpawnActor<AWaterBodyOcean>(Origin, FRotator::ZeroRotator);
				Ocean->SetActorLabel(OceanLabel);
			}
			TestFalse(TEXT("Attach refuses an ocean"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("Attach"),
				WaterSpecFind(Ocean->GetActorLabel()) + TEXT(",") + WaterSpecFind(TerrainLabel)), true, &Result));
			TestTrue(*FString::Printf(TEXT("only lakes and rivers (%s)"), *WaterSpecJson(Result)), WaterSpecJson(Result).Contains(TEXT("only lakes and rivers")));
		});
	});

	Describe("Members", [this]()
	{
		It("lists water bodies with their MeshTerrainWater status, and a terrain's WaterBodies", [this]()
		{
			const FString TerrainLabel = WaterSpecLabel(TEXT("SpecMembersTerrain"));
			const FString Label = WaterSpecLabel(TEXT("SpecMember"));
			const bool bHadZone = WaterSpecZone() != nullptr;
			ON_SCOPE_EXIT
			{
				WaterSpecCleanup(TerrainLabel, { Label });
				if (!bHadZone && WaterSpecZone())
					WaterSpecWorld()->EditorDestroyActor(WaterSpecZone(), true);
			};
			const FVector Origin(900000.0, -500000.0, 0.0);
			if (!TestNotNull(TEXT("terrain"), WaterSpecTerrain(TerrainLabel, Origin, 100)))
				return;
			TSharedPtr<FJsonObject> Result;
			if (!TestTrue(TEXT("CreateLake"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateLake"), FString::Join(TArray<FString>{
				WaterSpecFind(TerrainLabel), WaterSpecLit(Label), WaterSpecVec(Origin), WaterSpecNum(1500.0) }, TEXT(","))), true, &Result)))
				return;
			// meshTerrainWaterBodies | where Label == "<label>" | select MeshTerrainWater
			const FString Select = FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainWaterBodies"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"%s"}]}},{"op":"select","fields":[{"name":"MeshTerrainWater","expr":{"path":["MeshTerrainWater"]}}]}]})RQL"), *Label);
			TestTrue(TEXT("select ok"), UnrealMcpRqlTools::RunPlan(Select, false, &Result));
			const FString Json = WaterSpecJson(Result);
			for (const TCHAR* Field : { TEXT("Lake"), TEXT("LakeModifier"), TEXT("AffectsTerrain"), TEXT("ChannelDepth"), TEXT("Problem") })
				TestTrue(*FString::Printf(TEXT("status has %s (%s)"), Field, *Json), Json.Contains(Field));
			TestTrue(*FString::Printf(TEXT("names the terrain (%s)"), *Json), Json.Contains(TerrainLabel));
			// unreal.find("<terrain>").WaterBodies | select Label
			const FString Bodies = FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},"stages":[{"op":"get","expr":{"path":["WaterBodies"]}},{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"), *TerrainLabel);
			TestTrue(TEXT("WaterBodies ok"), UnrealMcpRqlTools::RunPlan(Bodies, false, &Result));
			TestTrue(*FString::Printf(TEXT("the terrain lists the lake (%s)"), *WaterSpecJson(Result)), WaterSpecJson(Result).Contains(Label));
		});

		It("reports an ocean that carries a water modifier, and a forced build timeout as still building", [this]()
		{
			const FString TerrainLabel = WaterSpecLabel(TEXT("SpecTimeoutTerrain"));
			const FString Label = WaterSpecLabel(TEXT("SpecTimeout"));
			const FString OceanLabel = WaterSpecLabel(TEXT("SpecModOcean"));
			const bool bHadZone = WaterSpecZone() != nullptr;
			ON_SCOPE_EXIT
			{
				UnrealAIMeshTerrainWater::BuildTimeoutForTests = 0.0;
				WaterSpecCleanup(TerrainLabel, { Label, OceanLabel });
				if (!bHadZone && WaterSpecZone())
					WaterSpecWorld()->EditorDestroyActor(WaterSpecZone(), true);
			};
			const FVector Origin(900000.0, -400000.0, 0.0);
			if (!TestNotNull(TEXT("terrain"), WaterSpecTerrain(TerrainLabel, Origin, 200)))
				return;
			AWaterBodyOcean* Ocean = WaterSpecWorld()->SpawnActor<AWaterBodyOcean>(Origin, FRotator::ZeroRotator);
			Ocean->SetActorLabel(OceanLabel);
			UClass* OceanModifier = FindObject<UClass>(nullptr, TEXT("/Script/MeshPartitionWater.OceanModifier"));
			if (!TestNotNull(TEXT("OceanModifier class"), OceanModifier))
				return;
			UActorComponent* Added = NewObject<UActorComponent>(Ocean, OceanModifier);
			Ocean->AddInstanceComponent(Added);
			Added->RegisterComponent();
			const FString Problem = UnrealAIMeshTerrainWater::Problem(Ocean);
			TestTrue(*FString::Printf(TEXT("Problem names Detach (%s)"), *Problem), Problem.Contains(TEXT("OceanModifier")) && Problem.Contains(TEXT("call Detach(")));
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("Detach"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("Detach"), WaterSpecFind(OceanLabel)), true, &Result));
			TestEqual(TEXT("the ocean has no modifier"), UnrealAIMeshTerrainWater::WaterModifiers(Ocean).Num(), 0);

			// A forced timeout: the lake is placed, the answer says it is still building and what to call.
			UnrealAIMeshTerrainWater::BuildTimeoutForTests = 0.001;
			TestTrue(TEXT("CreateLake"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("CreateLake"), FString::Join(TArray<FString>{
				WaterSpecFind(TerrainLabel), WaterSpecLit(Label), WaterSpecVec(Origin + FVector(3000.0, 0.0, 0.0)), WaterSpecNum(1500.0) }, TEXT(","))), true, &Result));
			const FString Json = WaterSpecJson(Result);
			TestTrue(*FString::Printf(TEXT("still building, WaitForBuild named (%s)"), *Json), Json.Contains(TEXT("still building")) && Json.Contains(TEXT("WaitForBuild")));
			UnrealAIMeshTerrainWater::BuildTimeoutForTests = 0.0;
			// meshTerrainWater | call WaitForBuild(unreal.find("<label>"))
			TestTrue(TEXT("WaitForBuild"), UnrealMcpRqlTools::RunPlan(WaterSpecCall(TEXT("WaitForBuild"), WaterSpecFind(Label)), false, &Result));
			TestTrue(*FString::Printf(TEXT("built after the wait (%s)"), *WaterSpecJson(Result)), WaterSpecJson(Result).Contains(TEXT("built")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
