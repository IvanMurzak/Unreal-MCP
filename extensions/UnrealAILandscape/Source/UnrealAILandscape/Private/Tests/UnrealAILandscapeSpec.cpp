// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include <limits>
#include "Dom/JsonObject.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"

#include "Engine/CollisionProfile.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Containers/Ticker.h"
#include "LandscapeSettings.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "WaterZoneActor.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions").
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::RunPlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it.
//
//  Run via:  Automation RunTests UnrealAILandscape
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAILandscapeSpec, "UnrealAILandscape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	/** The compact JSON text of an RQL result (empty when there is none). */
	static FString LandscapeSpecJson(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** The object in the "result" column of the first row of an RQL call result (null when there is none). */
	static TSharedPtr<FJsonObject> LandscapeSpecCallResult(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("columns"), Columns) || !Result->TryGetArrayField(TEXT("rows"), Rows) || Rows->IsEmpty())
			return nullptr;
		const int32 Column = Columns->IndexOfByPredicate([](const TSharedPtr<FJsonValue>& Name) { return Name->AsString() == TEXT("result"); });
		const TArray<TSharedPtr<FJsonValue>>& Row = (*Rows)[0]->AsArray();
		return Row.IsValidIndex(Column) && Row[Column]->Type == EJson::Object ? Row[Column]->AsObject() : nullptr;
	}

	/** Destroy every proxy of @p Landscape (on a World Partition map the components live in streaming proxies). */
	static void LandscapeSpecDestroy(ALandscape* Landscape)
	{
		TArray<ALandscapeProxy*> Proxies;
		if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
			Info->ForEachLandscapeProxy([&Proxies](ALandscapeProxy* Proxy) { Proxies.Add(Proxy); return true; });
		for (ALandscapeProxy* Proxy : Proxies)
			Proxy->Destroy();
	}

	/** The actor of class T labelled @p Label in the editor world (null when none). */
	template <typename T>
	static T* LandscapeSpecFind(const FString& Label)
	{
		for (TActorIterator<T> It(UnrealMcpRql::EditorWorld()); It; ++It)
		{
			if (It->GetActorLabel() == Label)
				return *It;
		}
		return nullptr;
	}

	/** How many actors of class @p Class the editor world has (0 for a null class). */
	static int32 LandscapeSpecCount(UClass* Class)
	{
		int32 Count = 0;
		if (!Class)
			return 0;
		for (TActorIterator<AActor> It(UnrealMcpRql::EditorWorld(), Class); It; ++It)
			++Count;
		return Count;
	}

	/** The editor world's actors now: LandscapeSpecDestroyNewWater removes the water actors added after it. */
	static TSet<AActor*> LandscapeSpecSnapshot()
	{
		TSet<AActor*> Actors;
		for (TActorIterator<AActor> It(UnrealMcpRql::EditorWorld()); It; ++It)
			Actors.Add(*It);
		return Actors;
	}

	/** Destroy the water bodies, water zones and water brush managers added since @p Before (a test's own and the Water editor's). */
	static void LandscapeSpecDestroyNewWater(const TSet<AActor*>& Before)
	{
		UClass* Manager = UnrealMcpRql::ResolveClass(TEXT("WaterBrushManager"));
		TArray<AActor*> Added;
		for (TActorIterator<AActor> It(UnrealMcpRql::EditorWorld()); It; ++It)
		{
			if (!Before.Contains(*It) && (It->IsA<AWaterBody>() || It->IsA<AWaterZone>() || (Manager && It->IsA(Manager))))
				Added.Add(*It);
		}
		for (AActor* Actor : Added)
			Actor->Destroy();
	}

	/** Run a plan with @LABEL@ / @LAND@ replaced (rql-mutate when @p bMutate). */
	static bool LandscapeSpecRun(const FString& Plan, const FString& Label, const FString& Land, bool bMutate, TSharedPtr<FJsonObject>* Out = nullptr)
	{
		return UnrealMcpRqlTools::RunPlan(Plan.Replace(TEXT("@LABEL@"), *Label).Replace(TEXT("@LAND@"), *Land), bMutate, Out);
	}
END_DEFINE_SPEC(FUnrealAILandscapeSpec)

void FUnrealAILandscapeSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-landscape"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("Landscape")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("ships the Water collision profile the Water plugin it enables requires (else every load errors and a cook fails)", [this]()
		{
			// Config/Engine.ini of this plugin: without it the WaterEditor module logs a LoadErrors error and
			// BuildCookRun fails with "Cook failed" (Error_UnknownCookFailure) in every project that installs the extension.
			FCollisionResponseTemplate Water;
			TestTrue(TEXT("WaterBodyCollision profile exists"), UCollisionProfile::Get()->GetProfileTemplate(TEXT("WaterBodyCollision"), Water));
			TestEqual(TEXT("query-only"), Water.CollisionEnabled.GetValue(), ECollisionEnabled::QueryOnly);
			TestEqual(TEXT("overlaps pawns"), Water.ResponseToChannels.GetResponse(ECC_Pawn), ECR_Overlap);
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `landscapes` |"), Skill.Contains(TEXT("| `landscapes` |")));
			TestTrue(TEXT("| `waterBodies` |"), Skill.Contains(TEXT("| `waterBodies` |")));
			TestTrue(TEXT("| `waterZones` |"), Skill.Contains(TEXT("| `waterZones` |")));
			TestTrue(TEXT("## Landscape and Water"), Skill.Contains(TEXT("## Landscape and Water")));
		});
	});

	Describe("sources", [this]()
	{
		It("landscapes answers a query", [this]()
		{
			// landscapes | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscapes"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("waterBodies answers a query", [this]()
		{
			// waterBodies | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["waterBodies"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("waterZones answers a query", [this]()
		{
			// waterZones | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["waterZones"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});
	});

	Describe("operations", [this]()
	{
		It("lists the landscape operations", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("LandscapeOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Method : { TEXT("Create"), TEXT("Sculpt"), TEXT("Paint"), TEXT("CreateLayerMaterial"), TEXT("Surface"), TEXT("Rebuild") })
				TestTrue(*FString::Printf(TEXT("lists %s"), Method), Schema.Contains(FString(Method) + TEXT("(")));
		});

		It("creates a landscape with a layer material, raises a hill and paints a layer; refuses bad input", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Label = TEXT("RqlSpecTerrain_") + Id;
			const FString MaterialPath = TEXT("/Game/RqlSpec/M_Terrain_") + Id;
			auto Run = [&Label, &MaterialPath](const TCHAR* Plan, bool bMutate, TSharedPtr<FJsonObject>* Out = nullptr)
			{
				return UnrealMcpRqlTools::RunPlan(FString(Plan).Replace(TEXT("@LABEL@"), *Label).Replace(TEXT("@MAT@"), *MaterialPath), bMutate, Out);
			};
			// landscape | call CreateLayerMaterial("@MAT@", "Grass #3a7d2c, Rock")
			TestTrue(TEXT("layer material created"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"CreateLayerMaterial","args":[{"lit":"@MAT@"},{"lit":"Grass #3a7d2c, Rock"}]}]})RQL"), true));
			UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
			UMaterialExpressionLandscapeLayerBlend* Blend = nullptr;
			if (TestNotNull(TEXT("the material exists"), Material))
			{
				for (UMaterialExpression* Expression : Material->GetExpressions())
					Blend = Blend ? Blend : Cast<UMaterialExpressionLandscapeLayerBlend>(Expression);
			}
			if (TestNotNull(TEXT("a Landscape Layer Blend"), Blend))
			{
				TestEqual(TEXT("two layers"), Blend->Layers.Num(), 2);
				TestEqual(TEXT("first layer"), Blend->Layers.Num() > 0 ? Blend->Layers[0].LayerName : FName(NAME_None), FName(TEXT("Grass")));
				TestTrue(TEXT("Grass is green"), Blend->Layers.Num() > 0 && Blend->Layers[0].ConstLayerInput.Y > Blend->Layers[0].ConstLayerInput.X);
			}
			// landscape | call Create("@LABEL@", [0, 0, 0], 20, "@MAT@")
			TestTrue(TEXT("landscape created"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Create","args":[{"lit":"@LABEL@"},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"lit":20},{"lit":"@MAT@"}]}]})RQL"), true));
			ALandscape* Landscape = nullptr;
			for (TActorIterator<ALandscape> It(UnrealMcpRql::EditorWorld()); It; ++It)
				Landscape = It->GetActorLabel() == Label ? *It : Landscape;
			if (!TestNotNull(TEXT("the landscape is in the editor world"), Landscape))
				return;
			TestTrue(TEXT("the material is assigned"), Landscape->LandscapeMaterial.Get() == Material);
			// On a World Partition map the components live in streaming proxies: measure the whole landscape through its info.
			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			FIntRect Whole;
			if (!TestTrue(TEXT("it has components (landscape extent)"), Info && Info->GetLandscapeExtent(Whole)))
				return;
			const FTransform ToWorld = Landscape->LandscapeActorToWorld();
			const FVector Min = ToWorld.TransformPosition(FVector(Whole.Min.X, Whole.Min.Y, 0.0));
			const FVector Max = ToWorld.TransformPosition(FVector(Whole.Max.X, Whole.Max.Y, 0.0));
			TestTrue(*FString::Printf(TEXT("20 m wide, centred (%.0f .. %.0f, %.0f .. %.0f cm)"), Min.X, Max.X, Min.Y, Max.Y),
				FMath::IsNearlyEqual(Max.X - Min.X, 2000.0, 1.0) && FMath::IsNearlyEqual(Max.Y - Min.Y, 2000.0, 1.0)
				&& FMath::Abs(Min.X + Max.X) < 1.0 && FMath::Abs(Min.Y + Max.Y) < 1.0);

			// landscape | call Sculpt(unreal.find("@LABEL@"), [0, 0, 0], 500, 300)
			TestTrue(TEXT("hill raised"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Sculpt","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"lit":500},{"lit":300}]}]})RQL"), true));
			{
				FHeightmapAccessor<false> Heights(Info);
				if (const ULandscapeEditLayerBase* EditLayer = Landscape->GetEditLayerConst(0))
					Heights.SetEditLayer(EditLayer->GetGuid());
				TArray<uint16> Data;
				Data.SetNumZeroed(Whole.Width() + 1);
				const int32 Row = (Whole.Min.Y + Whole.Max.Y) / 2;
				Heights.GetDataFast(Whole.Min.X, Row, Whole.Max.X, Row, Data.GetData());
				const double ZScale = Landscape->GetActorScale3D().Z * LANDSCAPE_ZSCALE;
				const double Centre = (static_cast<double>(Data[Data.Num() / 2]) - LandscapeDataAccess::MidValue) * ZScale;
				const double Edge = (static_cast<double>(Data[0]) - LandscapeDataAccess::MidValue) * ZScale;
				TestTrue(*FString::Printf(TEXT("the centre rose about 3 m (%.1f cm)"), Centre), FMath::IsNearlyEqual(Centre, 300.0, 25.0));
				TestTrue(*FString::Printf(TEXT("the edge stayed flat (%.1f cm)"), Edge), FMath::Abs(Edge) < 1.0);
			}

			// landscape | call Paint(unreal.find("@LABEL@"), "Grass", [0, 0, 0], 0)
			TestTrue(TEXT("layer painted"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Paint","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]},{"lit":"Grass"},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"lit":0}]}]})RQL"), true));
			// landscapes | where Label == "@LABEL@" | select PaintLayers ComponentCount LandscapeMaterial
			TSharedPtr<FJsonObject> Layers;
			Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscapes"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"@LABEL@"}]}},{"op":"select","fields":[{"name":"PaintLayers","expr":{"path":["PaintLayers"]}},{"name":"ComponentCount","expr":{"path":["ComponentCount"]}},{"name":"LandscapeMaterial","expr":{"path":["LandscapeMaterial"]}}]}]})RQL"), false, &Layers);
			const FString LayersText = LandscapeSpecJson(Layers);
			TestTrue(*FString::Printf(TEXT("PaintLayers lists Grass (%s)"), *LayersText.Left(300)), LayersText.Contains(TEXT("Grass")));
			const int32 GrassIndex = Info ? Info->GetLayerInfoIndex(FName(TEXT("Grass"))) : INDEX_NONE;
			ULandscapeLayerInfoObject* Grass = GrassIndex != INDEX_NONE ? Info->Layers[GrassIndex].LayerInfoObj.Get() : nullptr;
			if (TestNotNull(TEXT("the Grass layer info"), Grass))
			{
				TAlphamapAccessor<false> Weights(Info, Grass);
				if (const ULandscapeEditLayerBase* EditLayer = Landscape->GetEditLayerConst(0))
					Weights.SetEditLayer(EditLayer->GetGuid());
				uint8 Weight = 0;
				const int32 X = (Whole.Min.X + Whole.Max.X) / 2;
				const int32 Y = (Whole.Min.Y + Whole.Max.Y) / 2;
				Weights.GetDataFast(X, Y, X, Y, &Weight);
				TestEqual(TEXT("full Grass weight in the middle"), static_cast<int32>(Weight), 255);
				// landscape | call Paint(unreal.find("@LABEL@"), "Grass", [0, 0, 0], 0) again: the layer info is reused, not re-created
				TestTrue(TEXT("painted again"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Paint","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]},{"lit":"Grass"},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"lit":0}]}]})RQL"), true));
				const int32 Again = Info->GetLayerInfoIndex(FName(TEXT("Grass")));
				TestTrue(TEXT("the same Grass layer info"), Again != INDEX_NONE && Info->Layers[Again].LayerInfoObj.Get() == Grass);
			}

			// landscape | call Create("@LABEL@x", [0, 0, 0], 3)
			TestFalse(TEXT("a 3 m landscape is refused"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Create","args":[{"lit":"@LABEL@x"},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"lit":3}]}]})RQL"), true));
			// landscape | call CreateLayerMaterial("@MAT@x", "Grass #zz")
			TestFalse(TEXT("a bad colour is refused"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"CreateLayerMaterial","args":[{"lit":"@MAT@x"},{"lit":"Grass #zz"}]}]})RQL"), true));
			TestNull(TEXT("no material for the refused call"), FindObject<UMaterial>(nullptr, *(MaterialPath + TEXT("x.M_Terrain_") + Id + TEXT("x"))));
			// landscape | call Sculpt(unreal.find("@LABEL@"), [0, 0, 0], 0, 300)
			TestFalse(TEXT("a zero radius is refused"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Sculpt","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"lit":0},{"lit":300}]}]})RQL"), true));

			LandscapeSpecDestroy(Landscape);
		});
	});

	Describe("ground", [this]()
	{
		It("creates a 2 km landscape in 8 x 8 large components (32 x 32 small ones took minutes on a World Partition map)", [this]()
		{
			const FString Label = TEXT("RqlSpecIsland_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			// landscape | call Create("@LABEL@", [-800000, 0, 0], 2000)
			const double Start = FPlatformTime::Seconds();
			TestTrue(TEXT("landscape created"), UnrealMcpRqlTools::RunPlan(FString(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Create","args":[{"lit":"@LABEL@"},{"list":[{"lit":-800000},{"lit":0},{"lit":0}]},{"lit":2000}]}]})RQL")).Replace(TEXT("@LABEL@"), *Label), true));
			AddInfo(FString::Printf(TEXT("Create 2000 m: %.1f s"), FPlatformTime::Seconds() - Start));
			ALandscape* Landscape = nullptr;
			for (TActorIterator<ALandscape> It(UnrealMcpRql::EditorWorld()); It; ++It)
				Landscape = It->GetActorLabel() == Label ? *It : Landscape;
			if (!TestNotNull(TEXT("the landscape is in the editor world"), Landscape))
				return;
			ON_SCOPE_EXIT { LandscapeSpecDestroy(Landscape); };
			int32 Components = 0;
			Landscape->GetLandscapeInfo()->ForAllLandscapeComponents([&Components](ULandscapeComponent*) { ++Components; });
			TestEqual(TEXT("8 x 8 components"), Components, 64);
			TestEqual(TEXT("254-quad components (2 x 2 sections of 127)"), Landscape->ComponentSizeQuads, 254);
			FIntRect Whole;
			if (TestTrue(TEXT("landscape extent"), Landscape->GetLandscapeInfo()->GetLandscapeExtent(Whole)))
			{
				const FTransform ToWorld = Landscape->LandscapeActorToWorld();
				const double Width = ToWorld.TransformPosition(FVector(Whole.Max.X, 0, 0)).X - ToWorld.TransformPosition(FVector(Whole.Min.X, 0, 0)).X;
				TestTrue(*FString::Printf(TEXT("2000 m wide (%.0f cm)"), Width), FMath::IsNearlyEqual(Width, 200000.0, 1.0));
			}
		});

		It("returns from Create, Sculpt and Paint with the landscape merged: collision, traces and Surface see the new ground at once", [this]()
		{
			// Away from the other specs' actors, so the trace below meets only this landscape.
			const FString Label = TEXT("RqlSpecGround_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const double OriginX = 400000.0;
			auto Run = [&Label](const FString& Plan, bool bMutate, TSharedPtr<FJsonObject>* Out = nullptr)
			{
				return UnrealMcpRqlTools::RunPlan(Plan.Replace(TEXT("@LABEL@"), *Label), bMutate, Out);
			};
			// landscape | call Surface(unreal.find("@LABEL@"), [X, Y, 0]): the returned struct (an empty object when it failed)
			auto Surface = [this, &Run](double X, double Y)
			{
				TSharedPtr<FJsonObject> Result;
				const FString Plan = FString(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Surface","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]},{"list":[{"lit":@X@},{"lit":@Y@},{"lit":0}]}]}]})RQL"))
					.Replace(TEXT("@X@"), *FString::SanitizeFloat(X)).Replace(TEXT("@Y@"), *FString::SanitizeFloat(Y));
				TestTrue(*FString::Printf(TEXT("Surface(%.0f, %.0f) runs"), X, Y), Run(Plan, true, &Result));
				const TSharedPtr<FJsonObject> Ground = LandscapeSpecCallResult(Result);
				return Ground.IsValid() ? Ground.ToSharedRef() : MakeShared<FJsonObject>();
			};
			auto Number = [](const TSharedRef<FJsonObject>& Ground, const TCHAR* Field)
			{
				double Value = std::numeric_limits<double>::quiet_NaN();
				Ground->TryGetNumberField(Field, Value);
				return Value;
			};
			auto Text = [](const TSharedRef<FJsonObject>& Ground) { return LandscapeSpecJson(Ground); };

			// landscape | call Create("@LABEL@", [400000, 0, 0], 20)
			TestTrue(TEXT("landscape created"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Create","args":[{"lit":"@LABEL@"},{"list":[{"lit":400000},{"lit":0},{"lit":0}]},{"lit":20}]}]})RQL"), true));
			ALandscape* Landscape = nullptr;
			for (TActorIterator<ALandscape> It(UnrealMcpRql::EditorWorld()); It; ++It)
				Landscape = It->GetActorLabel() == Label ? *It : Landscape;
			if (!TestNotNull(TEXT("the landscape is in the editor world"), Landscape))
				return;
			ON_SCOPE_EXIT { LandscapeSpecDestroy(Landscape); };

			// landscapes | where Label == "@LABEL@" | select Bounds
			TSharedPtr<FJsonObject> Bounds;
			Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscapes"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"@LABEL@"}]}},{"op":"select","fields":[{"name":"Bounds","expr":{"path":["Bounds"]}}]}]})RQL"), false, &Bounds);
			const FString BoundsText = LandscapeSpecJson(Bounds);
			TestTrue(*FString::Printf(TEXT("Bounds: 20 x 20 m around X 400000, flat at Z 0 (%s)"), *BoundsText.Left(300)),
				BoundsText.Contains(TEXT("\"SizeMeters\":[20,20]")) && BoundsText.Contains(TEXT("\"Min\":[399000,-1000,0]")) && BoundsText.Contains(TEXT("\"Max\":[401000,1000,0]")));

			const TSharedRef<FJsonObject> Flat = Surface(OriginX, 0.0);
			TestTrue(*FString::Printf(TEXT("flat ground: inside, height 0, slope 0 (%s)"), *Text(Flat)), Flat->HasTypedField<EJson::Boolean>(TEXT("Inside")) && Flat->GetBoolField(TEXT("Inside"))
				&& FMath::Abs(Number(Flat, TEXT("Height"))) < 1.0 && FMath::Abs(Number(Flat, TEXT("Slope"))) < 0.5);
			const TSharedRef<FJsonObject> Outside = Surface(OriginX + 5000.0, 0.0);
			TestTrue(*FString::Printf(TEXT("off the landscape: Inside false (%s)"), *Text(Outside)), Outside->HasTypedField<EJson::Boolean>(TEXT("Inside")) && !Outside->GetBoolField(TEXT("Inside")));
			const TSharedRef<FJsonObject> Corner = Surface(OriginX + 1000.0, 1000.0);
			TestTrue(*FString::Printf(TEXT("the far corner is on the landscape (%s)"), *Text(Corner)), Corner->HasTypedField<EJson::Boolean>(TEXT("Inside")) && Corner->GetBoolField(TEXT("Inside"))
				&& FMath::Abs(Number(Corner, TEXT("Height"))) < 1.0);

			if (!FApp::CanEverRender())
			{
				// -nullrhi: a landscape merges its edit layers on the GPU, so the edit is checked only in a rendering editor.
				AddInfo(TEXT("this editor does not render (-nullrhi): the merge after Sculpt is checked only in a rendering editor"));
				return;
			}

			// landscape | call Sculpt(unreal.find("@LABEL@"), [400000, 0, 0], 500, 300)
			TestTrue(TEXT("hill raised"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Sculpt","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]},{"list":[{"lit":400000},{"lit":0},{"lit":0}]},{"lit":500},{"lit":300}]}]})RQL"), true));
			// No editor frame has passed since the call: everything below must already see the hill.
			const FVector Top(OriginX, 0.0, 0.0);
			const TOptional<float> Collision = Landscape->GetHeightAtLocation(Top, EHeightfieldSource::Complex);
			TestTrue(*FString::Printf(TEXT("the collision heightfield has the 3 m hill (%.1f cm)"), Collision.Get(-1.0f)), Collision.IsSet() && FMath::IsNearlyEqual(Collision.GetValue(), 300.0f, 25.0f));
			FHitResult Hit;
			const bool bHit = Landscape->GetWorld()->LineTraceSingleByChannel(Hit, Top + FVector(0, 0, 100000), Top - FVector(0, 0, 100000), ECC_Visibility);
			TestTrue(*FString::Printf(TEXT("a line trace hits the hill top (%.1f cm)"), bHit ? Hit.ImpactPoint.Z : -1.0), bHit && FMath::IsNearlyEqual(Hit.ImpactPoint.Z, 300.0, 25.0));
			{
				ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
				FIntRect Whole;
				if (TestTrue(TEXT("landscape extent"), Info && Info->GetLandscapeExtent(Whole)))
				{
					FLandscapeEditDataInterface Merged(Info); // no edit layer: the final heightmap the editor renders
					uint16 Value = 0;
					const int32 X = (Whole.Min.X + Whole.Max.X) / 2;
					const int32 Y = (Whole.Min.Y + Whole.Max.Y) / 2;
					Merged.GetHeightDataFast(X, Y, X, Y, &Value, 0);
					const double Cm = (static_cast<double>(Value) - LandscapeDataAccess::MidValue) * Landscape->GetActorScale3D().Z * LANDSCAPE_ZSCALE;
					TestTrue(*FString::Printf(TEXT("the rendered (merged) heightmap has the hill (%.1f cm)"), Cm), FMath::IsNearlyEqual(Cm, 300.0, 25.0));
				}
			}
			const TSharedRef<FJsonObject> Hill = Surface(OriginX, 0.0);
			TestTrue(*FString::Printf(TEXT("Surface reads the hill top, flat on top (%s)"), *Text(Hill)),
				FMath::IsNearlyEqual(Number(Hill, TEXT("Height")), 300.0, 25.0) && Number(Hill, TEXT("Slope")) < 5.0);
			const TSharedRef<FJsonObject> Side = Surface(OriginX + 250.0, 0.0);
			const TOptional<float> SideCollision = Landscape->GetHeightAtLocation(FVector(OriginX + 250.0, 0.0, 0.0), EHeightfieldSource::Complex);
			const double SideSlope = Number(Side, TEXT("Slope"));
			TestTrue(*FString::Printf(TEXT("Surface on the hillside: the collision height, a slope of 20 to 60 degrees (%s)"), *Text(Side)),
				SideCollision.IsSet() && FMath::IsNearlyEqual(Number(Side, TEXT("Height")), static_cast<double>(SideCollision.GetValue()), 0.5)
				&& SideSlope > 20.0 && SideSlope < 60.0);

			// landscape | call Paint(unreal.find("@LABEL@"), "Grass", [400000, 0, 0], 0)
			TestTrue(TEXT("layer painted"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Paint","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]},{"lit":"Grass"},{"list":[{"lit":400000},{"lit":0},{"lit":0}]},{"lit":0}]}]})RQL"), true));

			// landscape | call Rebuild(unreal.find("@LABEL@"))
			TSharedPtr<FJsonObject> Rebuilt;
			TestTrue(TEXT("rebuilt"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Rebuild","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]}]}]})RQL"), true, &Rebuilt));
			const TOptional<float> After = Landscape->GetHeightAtLocation(Top, EHeightfieldSource::Complex);
			TestTrue(*FString::Printf(TEXT("the hill survives a rebuild (%.1f cm; %s)"), After.Get(-1.0f), *LandscapeSpecJson(Rebuilt).Left(200)), After.IsSet() && FMath::IsNearlyEqual(After.GetValue(), 300.0f, 25.0f));
		});
	});

	Describe("water", [this]()
	{
		It("lists the water operations and the Water status members", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("WaterOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Method : { TEXT("EnableWater"), TEXT("SetZone"), TEXT("CreateOcean"), TEXT("CreateLake"), TEXT("CreateRiver"), TEXT("CreateRiverBetween"), TEXT("SetMaterial") })
				TestTrue(*FString::Printf(TEXT("lists %s"), Method), Schema.Contains(FString(Method) + TEXT("(")));
			const FString Body = UnrealMcpRqlTools::Schema(TEXT("WaterBody"), bOk);
			TestTrue(TEXT("lists name:Path"), Body.Contains(TEXT("\"name\":\"Path\"")));
			TestTrue(TEXT("Water describes Ready and Problem"), Body.Contains(TEXT("Ready")) && Body.Contains(TEXT("Problem")));
			TestTrue(TEXT("the guide steers to the water operations"), UnrealMcpRqlTools::SkillMarkdown().Contains(TEXT("with the `water` operations rather than `editor | call SpawnActor(")));
		});

		It("creates an ocean, a lake and a river with the Water defaults, never touching the landscape (no brush, no edit layer, no dialog)", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Land = TEXT("RqlSpecWaterLand_") + Id;
			const FString Label = TEXT("RqlSpecWater_") + Id;
			// landscape | call Create("@LAND@", [1200000, 0, 0], 100)
			TestTrue(TEXT("landscape created"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Create","args":[{"lit":"@LAND@"},{"list":[{"lit":1200000},{"lit":0},{"lit":0}]},{"lit":100}]}]})RQL"), Label, Land, true));
			ALandscape* Landscape = LandscapeSpecFind<ALandscape>(Land);
			if (!TestNotNull(TEXT("the landscape"), Landscape))
				return;
			TArray<AActor*> Created;
			const TSet<AActor*> Snapshot = LandscapeSpecSnapshot();
			ON_SCOPE_EXIT
			{
				LandscapeSpecDestroyNewWater(Snapshot);
				LandscapeSpecDestroy(Landscape);
			};
			const int32 Managers = LandscapeSpecCount(UnrealMcpRql::ResolveClass(TEXT("WaterBrushManager")));

			if (LandscapeSpecCount(AWaterBodyOcean::StaticClass()) == 0)
			{
				// water | call CreateOcean("@LABEL@O", 50)
				TestTrue(TEXT("ocean created"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"CreateOcean","args":[{"lit":"@LABEL@O"},{"lit":50}]}]})RQL"), Label, Land, true));
				AWaterBodyOcean* Ocean = LandscapeSpecFind<AWaterBodyOcean>(Label + TEXT("O"));
				if (TestNotNull(TEXT("the ocean"), Ocean))
				{
					Created.Add(Ocean);
					TestTrue(TEXT("the ocean surface is at Z 50"), FMath::IsNearlyEqual(Ocean->GetActorLocation().Z, 50.0, 1.0));
					TestNotNull(TEXT("a water zone renders it"), Ocean->GetWaterBodyComponent()->GetWaterZone());
				}
				// water | call CreateOcean("@LABEL@O2")
				TestFalse(TEXT("a second ocean is refused"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"CreateOcean","args":[{"lit":"@LABEL@O2"}]}]})RQL"), Label, Land, true));
			}
			// water | call CreateLake("@LABEL@L", [1201000, 1000, 200], 1500)
			TestTrue(TEXT("lake created"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"CreateLake","args":[{"lit":"@LABEL@L"},{"list":[{"lit":1201000},{"lit":1000},{"lit":200}]},{"lit":1500}]}]})RQL"), Label, Land, true));
			AWaterBodyLake* Lake = LandscapeSpecFind<AWaterBodyLake>(Label + TEXT("L"));
			if (TestNotNull(TEXT("the lake"), Lake))
			{
				Created.Add(Lake);
				TestEqual(TEXT("an 8-point shore"), Lake->GetWaterSpline()->GetNumberOfSplinePoints(), 8);
				const FVector Shore = Lake->GetWaterSpline()->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
				TestTrue(*FString::Printf(TEXT("the shore is 15 m out at the water surface Z 200 (%s)"), *Shore.ToString()), FMath::IsNearlyEqual(Shore.X, 1202500.0, 1.0) && FMath::IsNearlyEqual(Shore.Z, 200.0, 1.0));
			}
			// water | call CreateRiver("@LABEL@R", [[1198000, -3000, 300], [1200000, 0, 200], [1202000, 3000, 100]], 600)
			TestTrue(TEXT("river created from points"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"CreateRiver","args":[{"lit":"@LABEL@R"},{"list":[{"list":[{"lit":1198000},{"lit":-3000},{"lit":300}]},{"list":[{"lit":1200000},{"lit":0},{"lit":200}]},{"list":[{"lit":1202000},{"lit":3000},{"lit":100}]}]},{"lit":600}]}]})RQL"), Label, Land, true));
			AWaterBodyRiver* River = LandscapeSpecFind<AWaterBodyRiver>(Label + TEXT("R"));
			if (TestNotNull(TEXT("the river"), River))
			{
				Created.Add(River);
				TestEqual(TEXT("three spline points"), River->GetWaterSpline()->GetNumberOfSplinePoints(), 3);
				const FVector Last = River->GetWaterSpline()->GetLocationAtSplinePoint(2, ESplineCoordinateSpace::World);
				TestTrue(*FString::Printf(TEXT("the last point is where it was asked (%s)"), *Last.ToString()), Last.Equals(FVector(1202000, 3000, 100), 1.0));
				const UWaterSplineMetadata* Metadata = River->GetWaterBodyComponent()->GetWaterSplineMetadata();
				TestTrue(TEXT("6 m wide"), Metadata && Metadata->RiverWidth.Points.Num() == 3 && FMath::IsNearlyEqual(Metadata->RiverWidth.Points[1].OutVal, 600.0f, 0.5f));
			}
			for (AActor* Actor : Created)
			{
				const UWaterBodyComponent* Component = CastChecked<AWaterBody>(Actor)->GetWaterBodyComponent();
				TestFalse(*FString::Printf(TEXT("%s does not affect the landscape"), *Actor->GetActorLabel()), Component->bAffectsLandscape);
				TestNotNull(*FString::Printf(TEXT("%s has the Water default material"), *Actor->GetActorLabel()), Component->GetWaterMaterial());
			}
			TestEqual(TEXT("no water brush manager was spawned"), LandscapeSpecCount(UnrealMcpRql::ResolveClass(TEXT("WaterBrushManager"))), Managers);
			TestEqual(TEXT("no Water edit layer was added"), Landscape->GetLayerIndex(FName(TEXT("Water"))), static_cast<int32>(INDEX_NONE));
			TestFalse(TEXT("the extension keeps the modal edit layer dialog off"), GetDefault<ULandscapeSettings>()->bShowDialogForAutomaticLayerCreation);

			// water | call CreateRiver("@LABEL@X", [[0, 0, 0]])
			TestFalse(TEXT("a one-point river is refused"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"CreateRiver","args":[{"lit":"@LABEL@X"},{"list":[{"list":[{"lit":0},{"lit":0},{"lit":0}]}]}]}]})RQL"), Label, Land, true));
			// water | call SetMaterial(unreal.find("@LABEL@L"), "/Engine/BasicShapes/BasicShapeMaterial")
			UMaterialInterface* Before = Lake ? Lake->GetWaterBodyComponent()->GetWaterMaterial() : nullptr;
			TSharedPtr<FJsonObject> Refused;
			TestFalse(TEXT("a non-water material is refused"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"SetMaterial","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@L"}]},{"lit":"/Engine/BasicShapes/BasicShapeMaterial"}]}]})RQL"), Label, Land, true, &Refused));
			TestTrue(*FString::Printf(TEXT("the refusal names a water material to use (%s)"), *LandscapeSpecJson(Refused).Left(300)), LandscapeSpecJson(Refused).Contains(TEXT("Water_Material")));
			TestTrue(TEXT("the lake keeps its material"), !Lake || Lake->GetWaterBodyComponent()->GetWaterMaterial() == Before);
		});

		It("routes a river downhill between two points and carves its bed into the landscape", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Land = TEXT("RqlSpecRiverLand_") + Id;
			const FString Label = TEXT("RqlSpecRiver_") + Id;
			// landscape | call Create("@LAND@", [-1200000, 0, 0], 300)
			TestTrue(TEXT("landscape created"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Create","args":[{"lit":"@LAND@"},{"list":[{"lit":-1200000},{"lit":0},{"lit":0}]},{"lit":300}]}]})RQL"), Label, Land, true));
			ALandscape* Landscape = LandscapeSpecFind<ALandscape>(Land);
			if (!TestNotNull(TEXT("the landscape"), Landscape))
				return;
			const TSet<AActor*> Snapshot = LandscapeSpecSnapshot();
			ON_SCOPE_EXIT
			{
				LandscapeSpecDestroyNewWater(Snapshot);
				LandscapeSpecDestroy(Landscape);
			};
			// landscape | call Sculpt(unreal.find("@LAND@"), [-1200000, 15000, 0], 12000, 2000): a 20 m hill at the north edge
			TestTrue(TEXT("hill raised"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Sculpt","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LAND@"}]},{"list":[{"lit":-1200000},{"lit":15000},{"lit":0}]},{"lit":12000},{"lit":2000}]}]})RQL"), Label, Land, true));
			// water | call CreateRiverBetween("@LABEL@", unreal.find("@LAND@"), [-1200000, 12000, 0], [-1200000, -13000, 0], 800, 300)
			const double Start = FPlatformTime::Seconds();
			TestTrue(TEXT("river created"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"CreateRiverBetween","args":[{"lit":"@LABEL@"},{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LAND@"}]},{"list":[{"lit":-1200000},{"lit":12000},{"lit":0}]},{"list":[{"lit":-1200000},{"lit":-13000},{"lit":0}]},{"lit":800},{"lit":300}]}]})RQL"), Label, Land, true));
			AddInfo(FString::Printf(TEXT("CreateRiverBetween on a 300 m landscape: %.2f s"), FPlatformTime::Seconds() - Start));
			const AWaterBodyRiver* River = LandscapeSpecFind<AWaterBodyRiver>(Label);
			if (!TestNotNull(TEXT("the river"), River))
				return;
			const UWaterSplineComponent* Spline = River->GetWaterSpline();
			const int32 Count = Spline->GetNumberOfSplinePoints();
			TestTrue(*FString::Printf(TEXT("a routed spline (%d points)"), Count), Count >= 3 && Count <= 150);
			bool bDownhill = true;
			for (int32 Index = 1; Index < Count; ++Index)
				bDownhill &= Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).Z <= Spline->GetLocationAtSplinePoint(Index - 1, ESplineCoordinateSpace::World).Z + 0.5;
			TestTrue(TEXT("the water surface never climbs"), bDownhill);
			const FVector First = Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
			const FVector Last = Spline->GetLocationAtSplinePoint(Count - 1, ESplineCoordinateSpace::World);
			TestTrue(*FString::Printf(TEXT("from the hillside (%s) down to the mouth (%s)"), *First.ToString(), *Last.ToString()),
				FVector2D::Distance(FVector2D(First), FVector2D(-1200000, 12000)) < 300.0 && FVector2D::Distance(FVector2D(Last), FVector2D(-1200000, -13000)) < 300.0
				&& First.Z > Last.Z + 500.0);
			// The bed: the edit layer heights (merged or not, so -nullrhi checks it too) are lower under the river than the flat 0.
			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			FHeightmapAccessor<false> Heights(Info);
			if (const ULandscapeEditLayerBase* EditLayer = Landscape->GetEditLayerConst(0))
				Heights.SetEditLayer(EditLayer->GetGuid());
			const FVector Middle = Landscape->LandscapeActorToWorld().InverseTransformPosition(FVector(-1200000, -10000, 0));
			uint16 Value = 0;
			Heights.GetDataFast(FMath::RoundToInt(Middle.X), FMath::RoundToInt(Middle.Y), FMath::RoundToInt(Middle.X), FMath::RoundToInt(Middle.Y), &Value);
			const double Cm = (static_cast<double>(Value) - LandscapeDataAccess::MidValue) * Landscape->GetActorScale3D().Z * LANDSCAPE_ZSCALE;
			TestTrue(*FString::Printf(TEXT("the bed is carved about 3 m deep (%.1f cm)"), Cm), Cm < -200.0 && Cm > -400.0);
			// water | call CreateRiverBetween("@LABEL@x", unreal.find("@LAND@"), [0, 0, 0], [10, 0, 0])
			TestFalse(TEXT("points off the landscape are refused"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"CreateRiverBetween","args":[{"lit":"@LABEL@x"},{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LAND@"}]},{"list":[{"lit":0},{"lit":0},{"lit":0}]},{"list":[{"lit":10},{"lit":0},{"lit":0}]}]}]})RQL"), Label, Land, true));
		});

		It("keeps a water body placed with SpawnActor from opening the modal edit layer dialog, reports it and repairs it with SetMaterial", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Land = TEXT("RqlSpecGuardLand_") + Id;
			const FString Label = TEXT("RqlSpecGuardLake_") + Id;
			// landscape | call Create("@LAND@", [0, 1200000, 0], 50)
			TestTrue(TEXT("landscape created"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscape"]},"stages":[{"op":"call","method":"Create","args":[{"lit":"@LAND@"},{"list":[{"lit":0},{"lit":1200000},{"lit":0}]},{"lit":50}]}]})RQL"), Label, Land, true));
			ALandscape* Landscape = LandscapeSpecFind<ALandscape>(Land);
			if (!TestNotNull(TEXT("the landscape"), Landscape))
				return;
			const TSet<AActor*> Snapshot = LandscapeSpecSnapshot();
			ON_SCOPE_EXIT
			{
				LandscapeSpecDestroyNewWater(Snapshot); // the lake, and the brush manager / zone the Water editor added for it
				LandscapeSpecDestroy(Landscape);
			};
			// editor | call SpawnActor("WaterBodyLake", "@LABEL@", [0, 1200000, 100]): a landscape-affecting lake without the Water
			// defaults. With the engine's setting on, the Water plugin blocks this call on the modal "Insert New Landscape Edit Layer"
			// dialog (under -unattended too: this test would never return).
			TestTrue(TEXT("spawned and returned (no modal dialog)"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"WaterBodyLake"},{"lit":"@LABEL@"},{"list":[{"lit":0},{"lit":1200000},{"lit":100}]}]}]})RQL"), Label, Land, true));
			// The Water editor gives the brush to the landscape the lake overlaps, else the world's first one (a map's own landscape).
			bool bWaterLayer = false;
			for (TActorIterator<ALandscape> It(UnrealMcpRql::EditorWorld()); It; ++It)
				bWaterLayer |= It->GetLayerIndex(FName(TEXT("Water"))) != INDEX_NONE;
			TestTrue(TEXT("the engine created a Water edit layer for the brush (without its dialog)"), bWaterLayer);
			AWaterBodyLake* Lake = LandscapeSpecFind<AWaterBodyLake>(Label);
			if (!TestNotNull(TEXT("the lake"), Lake))
				return;
			// waterBodies | where Label == "@LABEL@" | select Water
			TSharedPtr<FJsonObject> Status;
			LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["waterBodies"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"@LABEL@"}]}},{"op":"select","fields":[{"name":"Water","expr":{"path":["Water"]}}]}]})RQL"), Label, Land, false, &Status);
			const FString StatusText = LandscapeSpecJson(Status);
			TestTrue(*FString::Printf(TEXT("Water reports it is not ready and names SetMaterial (%s)"), *StatusText.Left(400)),
				StatusText.Contains(TEXT("\"Ready\":false")) && StatusText.Contains(TEXT("water | call SetMaterial")));
			// water | call SetMaterial(unreal.find("@LABEL@"))
			TestTrue(TEXT("repaired"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["water"]},"stages":[{"op":"call","method":"SetMaterial","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]}]}]})RQL"), Label, Land, true));
			TestNotNull(TEXT("the lake has the Water default material"), Lake->GetWaterBodyComponent()->GetWaterMaterial());
		});

		LatentIt("gives a water body placed with SpawnActor (no Water defaults) the Place Actors defaults on the next tick", [this](const FDoneDelegate& Done)
		{
			const FString Label = TEXT("RqlSpecBareLake_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const TSet<AActor*> Snapshot = LandscapeSpecSnapshot();
			// editor | call SpawnActor("WaterBodyLake", "@LABEL@", [0, -1200000, 100])
			TestTrue(TEXT("spawned"), LandscapeSpecRun(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"WaterBodyLake"},{"lit":"@LABEL@"},{"list":[{"lit":0},{"lit":-1200000},{"lit":100}]}]}]})RQL"), Label, FString(), true));
			const TWeakObjectPtr<AWaterBodyLake> Lake = LandscapeSpecFind<AWaterBodyLake>(Label);
			if (!TestTrue(TEXT("the lake"), Lake.IsValid()))
			{
				LandscapeSpecDestroyNewWater(Snapshot);
				Done.Execute();
				return;
			}
			const int32 Points = Lake->GetWaterSpline()->GetNumberOfSplinePoints();
			TestNull(TEXT("no material right after SpawnActor"), Lake->GetWaterBodyComponent()->GetWaterMaterial());
			const double Deadline = FPlatformTime::Seconds() + 10.0;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this, Lake, Snapshot, Points, Deadline, Done](float)
			{
				const UWaterBodyComponent* Component = Lake.IsValid() ? Lake->GetWaterBodyComponent() : nullptr;
				if (Component && !Component->GetWaterMaterial() && FPlatformTime::Seconds() < Deadline)
					return true; // next tick
				TestTrue(TEXT("the lake got the Water default material"), Component && Component->GetWaterMaterial() != nullptr);
				TestTrue(TEXT("it kept its spline"), Lake.IsValid() && Lake->GetWaterSpline()->GetNumberOfSplinePoints() == Points);
				LandscapeSpecDestroyNewWater(Snapshot);
				Done.Execute();
				return false;
			}));
		});
	});

	Describe("members", [this]()
	{
		It("describes LandscapeProxy with its Sections and PaintLayers members", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("LandscapeProxy"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Sections"), Schema.Contains(TEXT("\"name\":\"Sections\"")));
			TestTrue(TEXT("lists name:IsPrimary"), Schema.Contains(TEXT("\"name\":\"IsPrimary\"")));
			TestTrue(TEXT("lists name:ComponentCount"), Schema.Contains(TEXT("\"name\":\"ComponentCount\"")));
			TestTrue(TEXT("lists name:PaintLayers"), Schema.Contains(TEXT("\"name\":\"PaintLayers\"")));
			TestTrue(TEXT("lists name:Bounds"), Schema.Contains(TEXT("\"name\":\"Bounds\"")));
		});

		It("describes WaterBody with its Water member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("WaterBody"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Water"), Schema.Contains(TEXT("\"name\":\"Water\"")));
			TestTrue(TEXT("lists name:WaterType"), Schema.Contains(TEXT("\"name\":\"WaterType\"")));
			TestTrue(TEXT("lists name:WaterZone"), Schema.Contains(TEXT("\"name\":\"WaterZone\"")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
