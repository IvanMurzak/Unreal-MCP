// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
			for (const TCHAR* Method : { TEXT("Create"), TEXT("Sculpt"), TEXT("Paint"), TEXT("CreateLayerMaterial") })
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
			FString LayersText;
			if (Layers.IsValid())
				FJsonSerializer::Serialize(Layers.ToSharedRef(), TJsonWriterFactory<>::Create(&LayersText));
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

			TArray<ALandscapeProxy*> Proxies;
			Info->ForEachLandscapeProxy([&Proxies](ALandscapeProxy* Proxy) { Proxies.Add(Proxy); return true; });
			for (ALandscapeProxy* Proxy : Proxies)
				Proxy->Destroy();
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
