// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "MeshPartition.h"
#include "MeshPartitionModifierActor.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionPreviewSection.h"
#include "Modifiers/MeshPartitionNoiseModifier.h"

// ============================================================================================
//  UE Automation spec for the Mesh Terrain extension's foundation: the RQL module, the shared helpers
//  (UnrealAIMeshTerrainCommon) and the `meshTerrain` operations Rebuild / WaitForBuild / Surface / EnableMeshTerrain.
//
//  Plans are the QueryPlans Rql.Parser produces for the query in the comment above them (the C++ runtime has no text
//  parser). Terrains are made with UnrealAIMeshTerrain::SpawnFlatTerrain (what Mesh Terrain Mode's Create tool runs),
//  modifiers with the modifier actor factory (what its Modifiers palette runs).
//
//  Run via:  Automation RunTests UnrealAIMeshTerrain
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIMeshTerrainSpec, "UnrealAIMeshTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	using AMeshPartition = UE::MeshPartition::AMeshPartition;

	/** The compact JSON text of an RQL result (empty when there is none). */
	static FString MeshTerrainSpecJson(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** The object in the "result" column of the first row of an RQL call result (null when there is none). */
	static TSharedPtr<FJsonObject> MeshTerrainSpecCallResult(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("columns"), Columns) || !Result->TryGetArrayField(TEXT("rows"), Rows) || Rows->IsEmpty())
			return nullptr;
		const int32 Column = Columns->IndexOfByPredicate([](const TSharedPtr<FJsonValue>& Name) { return Name->AsString() == TEXT("result"); });
		const TArray<TSharedPtr<FJsonValue>>& Row = (*Rows)[0]->AsArray();
		return Row.IsValidIndex(Column) && Row[Column]->Type == EJson::Object ? Row[Column]->AsObject() : nullptr;
	}

	/** `meshTerrain | call <Method>(unreal.find("<Label>"), <Args>)` as a plan; @p Args are plan argument JSON values. */
	static FString MeshTerrainSpecCall(const FString& Method, const FString& Label, const FString& Args = FString())
	{
		return FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrain"]},"stages":[{"op":"call","method":"%s","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]}%s%s]}]})RQL"),
			*Method, *Label, Args.IsEmpty() ? TEXT("") : TEXT(","), *Args);
	}

	/** A unique label for a spec terrain. */
	static FString MeshTerrainSpecLabel(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Short));
	}

	/** A noise modifier actor placed like the Modifiers palette does (UModifierActorFactory), bound to @p Terrain. */
	static UE::MeshPartition::UNoiseModifier* MeshTerrainSpecNoise(AMeshPartition* Terrain, const FString& Label)
	{
		UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryByClass(UE::MeshPartition::UModifierActorFactory::StaticClass()) : nullptr;
		AActor* Actor = Factory ? Factory->CreateActor(UE::MeshPartition::UNoiseModifier::StaticClass(), Terrain->GetWorld()->GetCurrentLevel(), FTransform(Terrain->GetActorLocation())) : nullptr;
		UE::MeshPartition::UNoiseModifier* Noise = Actor ? Actor->FindComponentByClass<UE::MeshPartition::UNoiseModifier>() : nullptr;
		if (!Noise)
			return nullptr;
		Actor->SetActorLabel(Label);
		// The factory binds the nearest terrain by its own rule (not always the one under it): bind the wanted one.
		Noise->SetAffectedMeshPartition(Terrain);
		return Noise;
	}

	/** The world trace (Visibility, complex) straight down at X, Y: what an agent's line trace would hit. */
	static bool MeshTerrainSpecTrace(UWorld* World, double X, double Y, FHitResult& Hit)
	{
		return World->LineTraceSingleByChannel(Hit, FVector(X, Y, 100000.0), FVector(X, Y, -100000.0), ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(MeshTerrainSpec), true));
	}
END_DEFINE_SPEC(FUnrealAIMeshTerrainSpec)

void FUnrealAIMeshTerrainSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-mesh-terrain"));
			if (TestNotNull(TEXT("module registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("MeshTerrain")));
				TestFalse(TEXT("reports its .uplugin version"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources, operations and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			for (const TCHAR* Text : { TEXT("meshTerrains"), TEXT("meshTerrainModifiers"), TEXT("meshTerrain | call Surface("), TEXT("call WaitForBuild("),
				TEXT("Mesh Terrain (Experimental, UE 5.8+)"), TEXT("generic paint slots"), TEXT("A stroke is at most 250 m in radius") })
			{
				TestTrue(*FString::Printf(TEXT("skill mentions %s"), Text), Skill.Contains(Text));
			}
		});

		It("lists the operations with their parameters", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("MeshTerrainOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Method : { TEXT("Rebuild(MeshPartition Terrain, double TimeoutSeconds)"), TEXT("WaitForBuild(MeshPartition Terrain, double TimeoutSeconds)"),
				TEXT("Surface(MeshPartition Terrain, Vector Point)"), TEXT("SurfacePoints(MeshPartition Terrain, List<Vector> Points)"), TEXT("EnableMeshTerrain()") })
			{
				TestTrue(*FString::Printf(TEXT("schema lists %s (%s)"), Method, *Schema.Left(400)), Schema.Contains(Method));
			}
			// The reads are read-only (rql-query can call them); Rebuild invalidates the sections, so it is not.
			for (const TCHAR* Read : { TEXT("[read-only] the built surface"), TEXT("[read-only] Surface at every point"), TEXT("[read-only] wait (at most") })
				TestTrue(*FString::Printf(TEXT("schema marks %s"), Read), Schema.Contains(Read));
			TestFalse(TEXT("Rebuild is not read-only"), Schema.Contains(TEXT("[read-only] rebuild")));
		});

		It("returns its guide for any spelling of its name or extension id", [this]()
		{
			for (const TCHAR* Key : { TEXT("MeshTerrain"), TEXT("mesh-terrain"), TEXT("Mesh Terrain"), TEXT("mesh_terrain"),
				TEXT("com.ivanmurzak.unreal-ai-mesh-terrain"), TEXT("unreal-ai-mesh-terrain"), TEXT("meshTerrains") })
			{
				bool bOk = false;
				const FString Guide = UnrealMcpRqlTools::Schema(FString(), Key, bOk);
				TestTrue(*FString::Printf(TEXT("guide '%s' (%s)"), Key, *Guide.Left(200)), bOk && Guide.Contains(TEXT("\"name\":\"MeshTerrain\"")));
			}
			bool bOk = true;
			UnrealMcpRqlTools::Schema(FString(), TEXT("mesh-terrainx"), bOk);
			TestFalse(TEXT("another name is still unknown"), bOk);
		});

		It("adds the terrain and modifier members", [this]()
		{
			bool bOk = false;
			const FString Terrain = UnrealMcpRqlTools::Schema(TEXT("MeshPartition"), bOk);
			TestTrue(TEXT("terrain schema ok"), bOk);
			for (const TCHAR* Member : { TEXT("Definition"), TEXT("PriorityLayers"), TEXT("Channels"), TEXT("Bounds"), TEXT("Build"), TEXT("Sections"), TEXT("Modifiers"), TEXT("BaseMeshes") })
				TestTrue(*FString::Printf(TEXT("MeshPartition has %s"), Member), Terrain.Contains(FString(TEXT("\"")) + Member + TEXT("\"")) || Terrain.Contains(FString(Member) + TEXT(" ")));
			const FString Modifier = UnrealMcpRqlTools::Schema(TEXT("NoiseModifier"), bOk);
			TestTrue(TEXT("modifier schema ok"), bOk);
			for (const TCHAR* Member : { TEXT("Terrain"), TEXT("Kind"), TEXT("Label"), TEXT("Bounds"), TEXT("Disabled"), TEXT("Modifier") })
				TestTrue(*FString::Printf(TEXT("modifiers have %s"), Member), Modifier.Contains(FString(TEXT("\"")) + Member + TEXT("\"")) || Modifier.Contains(FString(Member) + TEXT(" ")));
		});
	});

	Describe("sources", [this]()
	{
		It("meshTerrains and meshTerrainModifiers answer a query", [this]()
		{
			for (const TCHAR* Source : { TEXT("meshTerrains"), TEXT("meshTerrainModifiers") })
			{
				TSharedPtr<FJsonObject> Result;
				const bool bOk = UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["%s"]},"stages":[{"op":"count"}]})RQL"), Source), false, &Result);
				TestTrue(*FString::Printf(TEXT("%s | count (%s)"), Source, *MeshTerrainSpecJson(Result)), bOk);
			}
		});
	});

	Describe("guard", [this]()
	{
		It("reports Mesh Terrain available in this editor (the plugins are enabled with the extension)", [this]()
		{
			TestEqual(TEXT("no reason it is unavailable"), UnrealAIMeshTerrain::UnavailableReason(), FString());
		});

		It("refuses a missing terrain and out-of-range timeouts with a clear error", [this]()
		{
			const FString Missing = MeshTerrainSpecLabel(TEXT("NoSuchTerrain"));
			for (const TCHAR* Method : { TEXT("Rebuild"), TEXT("WaitForBuild") })
			{
				TSharedPtr<FJsonObject> Result;
				TestFalse(*FString::Printf(TEXT("%s on a missing terrain fails"), Method), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(Method, Missing), true, &Result));
				const FString Json = MeshTerrainSpecJson(Result);
				TestTrue(*FString::Printf(TEXT("%s names the missing terrain (%s)"), Method, *Json), Json.Contains(TEXT("argument 'Terrain'")) && Json.Contains(TEXT("not-found")));
			}
			TSharedPtr<FJsonObject> Result;
			TestFalse(TEXT("Surface on a missing terrain fails"), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("Surface"), Missing, TEXT(R"({"list":[{"lit":0},{"lit":0},{"lit":0}]})")), true, &Result));
			TestTrue(TEXT("names Terrain"), MeshTerrainSpecJson(Result).Contains(TEXT("argument 'Terrain'")));
			// meshTerrain | call Surface(null, [0, 0, 0]): RQL refuses a null terrain before the operation runs.
			TestFalse(TEXT("Surface(null) fails"), UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrain"]},"stages":[{"op":"call","method":"Surface","args":[{"lit":null},{"list":[{"lit":0},{"lit":0},{"lit":0}]}]}]})RQL"), true, &Result));
			TestTrue(*FString::Printf(TEXT("names the parameter (%s)"), *MeshTerrainSpecJson(Result)), MeshTerrainSpecJson(Result).Contains(TEXT("null is not allowed for MeshPartition")));

			const FString Label = MeshTerrainSpecLabel(TEXT("SpecGuard"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-600000.0, 0.0, 0.0), 2000.0, 2000.0, 10, 10);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			for (const TCHAR* Timeout : { TEXT("0.5"), TEXT("601") })
			{
				TestFalse(*FString::Printf(TEXT("timeout %s refused"), Timeout), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("WaitForBuild"), Label, FString::Printf(TEXT(R"({"lit":%s})"), Timeout)), true, &Result));
				TestTrue(*FString::Printf(TEXT("names the range (%s)"), *MeshTerrainSpecJson(Result)), MeshTerrainSpecJson(Result).Contains(TEXT("1 .. 600")));
			}
			TestNull(TEXT("FindTerrain of a missing label is null"), UnrealAIMeshTerrain::FindTerrain(Missing));
			TestEqual(TEXT("FindTerrain finds it by label"), UnrealAIMeshTerrain::FindTerrain(Label), Terrain);
		});
	});

	Describe("terrain", [this]()
	{
		It("builds a created terrain and returns from WaitForBuild with its sections, collision and Bounds ready", [this]()
		{
			const FString Label = MeshTerrainSpecLabel(TEXT("SpecFlat"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-700000.0, 0.0, 500.0), 4000.0, 3000.0, 40, 30);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			TestEqual(TEXT("one base mesh"), UnrealAIMeshTerrain::Modifiers(Terrain, /*bBases*/ true).Num(), 1);
			TestEqual(TEXT("no shaping modifier"), UnrealAIMeshTerrain::Modifiers(Terrain).Num(), 0);
			TSharedPtr<FJsonObject> Result;
			TestTrue(*FString::Printf(TEXT("WaitForBuild (%s)"), *MeshTerrainSpecJson(Result)), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("WaitForBuild"), Label), true, &Result));
			const TSharedPtr<FJsonObject> Build = MeshTerrainSpecCallResult(Result);
			if (!TestTrue(TEXT("build result"), Build.IsValid()))
				return;
			TestTrue(*FString::Printf(TEXT("Ready (%s)"), *MeshTerrainSpecJson(Build)), Build->GetBoolField(TEXT("Ready")));
			TestTrue(TEXT("sections built"), Build->GetIntegerField(TEXT("Sections")) > 0);
			TestTrue(TEXT("triangles rendered"), Build->GetIntegerField(TEXT("Triangles")) > 0);
			TestEqual(TEXT("State"), Build->GetStringField(TEXT("State")), FString(TEXT("built")));
			TestTrue(TEXT("the build state agrees"), UnrealAIMeshTerrain::BuildState(Terrain).IsReady());

			const FBox Bounds = UnrealAIMeshTerrain::BuiltBounds(Terrain);
			TestTrue(*FString::Printf(TEXT("Bounds span the terrain (%s)"), *Bounds.ToString()), Bounds.IsValid
				&& FMath::IsNearlyEqual(Bounds.Min.X, -702000.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.X, -698000.0, 1.0)
				&& FMath::IsNearlyEqual(Bounds.Min.Y, -1500.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.Y, 1500.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.Z, 500.0, 1.0));

			// meshTerrains | where Label == "<label>" | select Build Bounds Sections
			TestTrue(TEXT("members query"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrains"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"%s"}]}},{"op":"select","fields":[{"name":"Build","expr":{"path":["Build"]}},{"name":"Bounds","expr":{"path":["Bounds"]}},{"name":"Sections","expr":{"path":["Sections"]}}]}]})RQL"), *Label), false, &Result));
			const FString Json = MeshTerrainSpecJson(Result);
			TestTrue(*FString::Printf(TEXT("Build member Ready (%s)"), *Json), Json.Contains(TEXT("\"Ready\":true")));
			TestTrue(TEXT("Bounds member in metres"), Json.Contains(TEXT("\"SizeMeters\":[40,30]")));
			TestTrue(TEXT("Sections member"), Json.Contains(TEXT("PreviewSection")));
		});

		It("reports in Bounds the built box past the edge, the terrain's own Base box and its highest and lowest ground", [this]()
		{
			const FString Label = MeshTerrainSpecLabel(TEXT("SpecBounds"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-900000.0, 0.0, 0.0), 4000.0, 4000.0, 40, 40);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestTrue(TEXT("built"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0)))
				return;
			// meshTerrainModifier | call AddShape(unreal.find("<label>"), "Sphere", [-898500, 0, 0], [2000, 2000, 1000], "Union"):
			// a dome whose half reaches 5 m past the terrain's +X edge (-898000).
			TSharedPtr<FJsonObject> Result;
			TestTrue(*FString::Printf(TEXT("AddShape (%s)"), *MeshTerrainSpecJson(Result)), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifier"]},"stages":[{"op":"call","method":"AddShape","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},{"lit":"Sphere"},{"list":[{"lit":-898500},{"lit":0},{"lit":0}]},{"list":[{"lit":2000},{"lit":2000},{"lit":1000}]},{"lit":"Union"}]}]})RQL"), *Label), true, &Result));

			TOptional<FVector> High, Low;
			UnrealAIMeshTerrain::GroundExtremes(Terrain, UnrealAIMeshTerrain::BaseBounds(Terrain), High, Low);
			if (!TestTrue(TEXT("the built ground has extremes"), High.IsSet() && Low.IsSet()))
				return;
			const FVector Highest = *High, Lowest = *Low;
			TestTrue(*FString::Printf(TEXT("highest ground on the dome's top (%s)"), *Highest.ToString()), FMath::IsNearlyEqual(Highest.Z, 500.0, 5.0)
				&& FVector2D::Distance(FVector2D(Highest), FVector2D(-898500.0, 0.0)) <= 300.0);
			const FBox Base = UnrealAIMeshTerrain::BaseBounds(Terrain);
			const FBox Built = UnrealAIMeshTerrain::BuiltBounds(Terrain);
			// Past the edge the dome's lower half is built too, under it: the lowest GROUND (a trace from above) is the flat terrain.
			TestTrue(*FString::Printf(TEXT("lowest ground on the flat terrain, not under the overhang (%s, %s)"), *Lowest.ToString(), *Built.ToString()), FMath::IsNearlyEqual(Lowest.Z, 0.0, 1.0));
			TestTrue(*FString::Printf(TEXT("Base is the terrain as created (%s)"), *Base.ToString()), Base.IsValid
				&& FMath::IsNearlyEqual(Base.Min.X, -902000.0, 1.0) && FMath::IsNearlyEqual(Base.Max.X, -898000.0, 1.0)
				&& FMath::IsNearlyEqual(Base.Min.Y, -2000.0, 1.0) && FMath::IsNearlyEqual(Base.Max.Y, 2000.0, 1.0));
			TestTrue(*FString::Printf(TEXT("the built box reaches past the edge (%s)"), *Built.ToString()), Built.IsValid && Built.Max.X > -898000.0 + 100.0);

			// meshTerrains | where Label == "<label>" | select Bounds
			TestTrue(TEXT("Bounds query"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrains"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"%s"}]}},{"op":"select","fields":[{"name":"Bounds","expr":{"path":["Bounds"]}}]}]})RQL"), *Label), false, &Result));
			const FString Json = MeshTerrainSpecJson(Result);
			TestTrue(*FString::Printf(TEXT("Bounds.Base in metres (%s)"), *Json), Json.Contains(TEXT("\"Base\":{\"Min\":[-902000,-2000,")) && Json.Contains(TEXT("\"SizeMeters\":[40,40]}")));
			TestTrue(TEXT("Bounds.Highest and Lowest"), Json.Contains(TEXT("\"Highest\":[")) && Json.Contains(TEXT("\"Lowest\":[")));

			// A box pit with vertical walls: its floor corners sit right under the rim, still the lowest ground.
			// meshTerrainModifier | call AddShape(unreal.find("<label>"), "Box", [-901000, 0, 0], [1000, 1000, 1000], "Subtract")
			TestTrue(TEXT("box pit"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifier"]},"stages":[{"op":"call","method":"AddShape","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},{"lit":"Box"},{"list":[{"lit":-901000},{"lit":0},{"lit":0}]},{"list":[{"lit":1000},{"lit":1000},{"lit":1000}]},{"lit":"Subtract"}]}]})RQL"), *Label), true, &Result));
			UnrealAIMeshTerrain::GroundExtremes(Terrain, UnrealAIMeshTerrain::BaseBounds(Terrain), High, Low);
			TestTrue(*FString::Printf(TEXT("lowest ground on the pit's floor (%s)"), Low ? *Low->ToString() : TEXT("unset")), Low.IsSet() && FMath::IsNearlyEqual(Low->Z, -500.0, 2.0)
				&& FVector2D::Distance(FVector2D(*Low), FVector2D(-901000.0, 0.0)) <= 710.0);
		});

		It("reads the flat ground with Surface: height, slope, location; off the terrain Inside is false", [this]()
		{
			const FString Label = MeshTerrainSpecLabel(TEXT("SpecSurface"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-800000.0, 0.0, 250.0), 4000.0, 4000.0, 40, 40);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("Surface on the terrain"), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("Surface"), Label, TEXT(R"({"list":[{"lit":-800500},{"lit":700},{"lit":99999}]})")), true, &Result));
			const TSharedPtr<FJsonObject> Surface = MeshTerrainSpecCallResult(Result);
			if (!TestTrue(*FString::Printf(TEXT("surface result (%s)"), *MeshTerrainSpecJson(Result)), Surface.IsValid()))
				return;
			TestTrue(TEXT("Inside"), Surface->GetBoolField(TEXT("Inside")));
			TestTrue(*FString::Printf(TEXT("Height is the terrain Z (%s)"), *MeshTerrainSpecJson(Surface)), FMath::IsNearlyEqual(Surface->GetNumberField(TEXT("Height")), 250.0, 0.5));
			TestTrue(TEXT("flat"), Surface->GetNumberField(TEXT("Slope")) < 0.5);
			TestTrue(TEXT("Location on the ground"), MeshTerrainSpecJson(Surface).Contains(TEXT("\"Location\":")));

			TestTrue(TEXT("Surface off the terrain"), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("Surface"), Label, TEXT(R"({"list":[{"lit":-900000},{"lit":0},{"lit":0}]})")), true, &Result));
			const TSharedPtr<FJsonObject> Off = MeshTerrainSpecCallResult(Result);
			TestTrue(TEXT("off the terrain Inside is false"), Off.IsValid() && !Off->GetBoolField(TEXT("Inside")));

			// Read-only operations: rql-query runs them (no rql-mutate needed to read the ground).
			TestTrue(*FString::Printf(TEXT("Surface in rql-query (%s)"), *MeshTerrainSpecJson(Result)),
				UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("Surface"), Label, TEXT(R"({"list":[{"lit":-800500},{"lit":700},{"lit":0}]})")), false, &Result));
			TestTrue(*FString::Printf(TEXT("SurfacePoints in rql-query (%s)"), *MeshTerrainSpecJson(Result)),
				UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("SurfacePoints"), Label, TEXT(R"({"list":[{"list":[{"lit":-800500},{"lit":700},{"lit":0}]},{"list":[{"lit":-800500},{"lit":700},{"lit":0}]}]})")), false, &Result));
			TestTrue(*FString::Printf(TEXT("WaitForBuild in rql-query (%s)"), *MeshTerrainSpecJson(Result)),
				UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("WaitForBuild"), Label), false, &Result));
			TestFalse(TEXT("Rebuild is refused in rql-query"), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("Rebuild"), Label), false, &Result));
			TestTrue(*FString::Printf(TEXT("as permission-denied (%s)"), *MeshTerrainSpecJson(Result)), MeshTerrainSpecJson(Result).Contains(TEXT("permission-denied")));
		});

		It("returns from WaitForBuild with a modifier applied: Surface, a world trace and the evaluated stack agree; disabling it with set flattens it again", [this]()
		{
			const FString Label = MeshTerrainSpecLabel(TEXT("SpecNoise"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-900000.0, 0.0, 0.0), 6000.0, 6000.0, 60, 60);
			UE::MeshPartition::UNoiseModifier* Noise = Terrain ? MeshTerrainSpecNoise(Terrain, Label + TEXT("_Noise")) : nullptr;
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestNotNull(TEXT("noise modifier placed"), Noise))
				return;
			// meshTerrainModifiers | where Owner.Label == "<label>_Noise" | set Intensity = 800, bWriteToWeightChannel = true, WeightChannelName = {Name: "Red"}
			const FString Where = FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifiers"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Owner","Label"]},{"lit":"%s_Noise"}]}},)RQL"), *Label);
			TSharedPtr<FJsonObject> Result;
			TestTrue(*FString::Printf(TEXT("set the noise (%s)"), *MeshTerrainSpecJson(Result)), UnrealMcpRqlTools::RunPlan(Where
				+ TEXT(R"RQL({"op":"set","assign":[{"target":{"path":["Intensity"]},"value":{"lit":800}},{"target":{"path":["UnscaledCoverage"]},"value":{"object":[{"key":"X","value":{"lit":6000}},{"key":"Y","value":{"lit":6000}},{"key":"Z","value":{"lit":50000}}]}},{"target":{"path":["bWriteToWeightChannel"]},"value":{"lit":true}},{"target":{"path":["WeightChannelName"]},"value":{"object":[{"key":"Name","value":{"lit":"Red"}}]}}]}]})RQL"), true, &Result));
			TestTrue(*FString::Printf(TEXT("WaitForBuild (%s)"), *MeshTerrainSpecJson(Result)), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("WaitForBuild"), Label), true, &Result));
			const TSharedPtr<FJsonObject> Build = MeshTerrainSpecCallResult(Result);
			TestTrue(TEXT("Ready with the modifier"), Build.IsValid() && Build->GetBoolField(TEXT("Ready")) && Build->GetIntegerField(TEXT("Modifiers")) == 1);
			const TArray<UnrealAIMeshTerrain::UModifierComponent*> Listed = UnrealAIMeshTerrain::Modifiers(Terrain);
			TestTrue(TEXT("Modifiers lists the noise"), Listed.Num() == 1 && Listed[0] == Noise && UnrealAIMeshTerrain::ModifierKind(Noise) == TEXT("Noise"));
			// meshTerrainModifiers | where Owner.Label == "<label>_Noise" | select Label Bounds
			TestTrue(TEXT("Label / Bounds query"), UnrealMcpRqlTools::RunPlan(Where + TEXT(R"RQL({"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}},{"name":"Bounds","expr":{"path":["Bounds"]}}]}]})RQL"), false, &Result));
			const FString Members = MeshTerrainSpecJson(Result);
			TestTrue(*FString::Printf(TEXT("a modifier's Label and Bounds (%s)"), *Members.Left(400)), Members.Contains(FString::Printf(TEXT("\"%s_Noise\""), *Label)) && Members.Contains(TEXT("\"SizeMeters\":[60,60]")));

			UWorld* World = Terrain->GetWorld();
			double Relief = 0.0;
			double ModelGap = 0.0;
			bool bChannel = false;
			for (const FVector2D Point : { FVector2D(-901300.0, 900.0), FVector2D(-899200.0, -1100.0), FVector2D(-900400.0, 1700.0), FVector2D(-898700.0, 2100.0) })
			{
				const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, Point.X, Point.Y);
				FHitResult Hit;
				const bool bHit = MeshTerrainSpecTrace(World, Point.X, Point.Y, Hit);
				TestTrue(*FString::Printf(TEXT("Surface inside at %s"), *Point.ToString()), Sample.bInside);
				TestTrue(*FString::Printf(TEXT("a world trace hits the same ground at %s (%.2f vs %.2f)"), *Point.ToString(), Sample.Height, bHit ? Hit.ImpactPoint.Z : -1.0),
					bHit && FMath::IsNearlyEqual(Sample.Height, Hit.ImpactPoint.Z, 1.0));
				ModelGap = FMath::Max(ModelGap, FMath::Abs(Sample.ModelHeight - Sample.Height));
				Relief = FMath::Max(Relief, FMath::Abs(Sample.Height));
				bChannel |= Sample.Channels.Contains(TEXT("Red")) && !FMath::IsNearlyZero(Sample.Channels[TEXT("Red")]);
			}
			TestTrue(*FString::Printf(TEXT("the noise raised or lowered the ground (%.1f cm)"), Relief), Relief > 10.0);
			// The rendered (and collision) mesh is the simplified one the preview pipeline makes: close to, not equal to, the stack.
			TestTrue(*FString::Printf(TEXT("the evaluated stack is near the built ground (%.1f cm off, relief %.1f cm)"), ModelGap, Relief), ModelGap < 0.25 * Relief + 5.0);
			TestTrue(TEXT("Channels reads the noise's Red weights"), bChannel);

			// ... | set bIsDisabled = true
			TestTrue(TEXT("disable the noise"), UnrealMcpRqlTools::RunPlan(Where + TEXT(R"RQL({"op":"set","assign":[{"target":{"path":["bIsDisabled"]},"value":{"lit":true}}]}]})RQL"), true, &Result));
			TestTrue(TEXT("Disabled member"), Noise->GetIsDisabledFlag());
			TestTrue(TEXT("WaitForBuild after disabling"), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("WaitForBuild"), Label), true, &Result));
			for (const FVector2D Point : { FVector2D(-901300.0, 900.0), FVector2D(-899200.0, -1100.0) })
			{
				const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, Point.X, Point.Y);
				TestTrue(*FString::Printf(TEXT("flat again at %s (%.2f)"), *Point.ToString(), Sample.Height), Sample.bInside && FMath::IsNearlyZero(Sample.Height, 0.5));
			}
		});

		It("reads a flat ground's Slope as 0 on the seams between sections and at its border", [this]()
		{
			// 1000 x 300 quads: more than one section (at most 500 x 500 quads each), so there are seams to sample.
			const FString Label = MeshTerrainSpecLabel(TEXT("SpecSeams"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-2300000.0, -3000000.0, 40.0), 12000.0, 3600.0, 1000, 300);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestTrue(TEXT("built"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 120.0)))
				return;
			const TArray<UnrealAIMeshTerrain::APreviewSection*> Sections = UnrealAIMeshTerrain::PreviewSections(Terrain);
			if (!TestTrue(*FString::Printf(TEXT("several sections (%d)"), Sections.Num()), Sections.Num() > 1))
				return;
			TArray<FVector2D> Points;
			for (const UnrealAIMeshTerrain::APreviewSection* Section : Sections)
			{
				// Every section corner and edge midpoint: on a seam or on the terrain's border.
				const FBox Box = Section->GetPreviewMeshBounds();
				for (const double X : { Box.Min.X, Box.GetCenter().X, Box.Max.X })
				{
					for (const double Y : { Box.Min.Y, Box.GetCenter().Y, Box.Max.Y })
						Points.Add(FVector2D(X, Y));
				}
			}
			for (const FVector2D& Point : Points)
			{
				const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, Point.X, Point.Y);
				const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FMath::Abs(Sample.Normal.Z), 0.0, 1.0)));
				TestTrue(*FString::Printf(TEXT("flat at %s: height %.2f, slope %.2f (normal %s)"), *Point.ToString(), Sample.Height, Slope, *Sample.Normal.ToString()),
					Sample.bInside && FMath::IsNearlyEqual(Sample.Height, 40.0, 0.5) && Slope < 0.5);
			}
		});

		It("rebuilds every section with Rebuild and keeps the ground", [this]()
		{
			const FString Label = MeshTerrainSpecLabel(TEXT("SpecRebuild"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-1000000.0, 0.0, 120.0), 3000.0, 3000.0, 30, 30);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("first build"), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("WaitForBuild"), Label), true, &Result));
			const TArray<UnrealAIMeshTerrain::APreviewSection*> Before = UnrealAIMeshTerrain::PreviewSections(Terrain);
			TestTrue(*FString::Printf(TEXT("Rebuild (%s)"), *MeshTerrainSpecJson(Result)), UnrealMcpRqlTools::RunPlan(MeshTerrainSpecCall(TEXT("Rebuild"), Label, TEXT(R"({"lit":60})")), true, &Result));
			const TSharedPtr<FJsonObject> Build = MeshTerrainSpecCallResult(Result);
			TestTrue(TEXT("Ready after Rebuild"), Build.IsValid() && Build->GetBoolField(TEXT("Ready")));
			const TArray<UnrealAIMeshTerrain::APreviewSection*> After = UnrealAIMeshTerrain::PreviewSections(Terrain);
			TestTrue(TEXT("new sections were built"), After.Num() > 0 && !After.ContainsByPredicate([&Before](UnrealAIMeshTerrain::APreviewSection* Section) { return Before.Contains(Section); }));
			const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, -1000000.0, 0.0);
			TestTrue(TEXT("same ground"), Sample.bInside && FMath::IsNearlyEqual(Sample.Height, 120.0, 0.5));
		});

		It("evaluates the modifier stack of a region (BuildRegion) in the terrain's space", [this]()
		{
			const FString Label = MeshTerrainSpecLabel(TEXT("SpecRegion"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-1100000.0, 30000.0, 75.0), 2000.0, 2000.0, 20, 20);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestTrue(TEXT("built"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0)))
				return;
			const TArray<UnrealAIMeshTerrain::FRegionMesh> Parts = UnrealAIMeshTerrain::BuildRegion(Terrain, FBox(FVector(-1100300.0, 29700.0, -1000.0), FVector(-1099700.0, 30300.0, 1000.0)));
			int32 Triangles = 0;
			FBox World(ForceInit);
			for (const UnrealAIMeshTerrain::FRegionMesh& Part : Parts)
			{
				Triangles += Part.Mesh->TriangleCount();
				for (const int32 Vertex : Part.Mesh->VertexIndicesItr())
					World += Part.Transform.TransformPosition(Part.Mesh->GetVertex(Vertex));
			}
			TestTrue(*FString::Printf(TEXT("the region's triangles (%d)"), Triangles), Triangles > 0 && Triangles < 800);
			TestTrue(*FString::Printf(TEXT("in world space they lie in the region on the terrain (%s)"), *World.ToString()), World.IsValid
				&& World.Min.X >= -1100301.0 && World.Max.X <= -1099699.0 && FMath::IsNearlyEqual(World.Min.Z, 75.0, 0.5) && FMath::IsNearlyEqual(World.Max.Z, 75.0, 0.5));
		});
	});

	Describe("project", [this]()
	{
		It("EnableMeshTerrain keeps both plugins in the project file without a restart", [this]()
		{
			// meshTerrain | call EnableMeshTerrain()
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("EnableMeshTerrain"), UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrain"]},"stages":[{"op":"call","method":"EnableMeshTerrain","args":[]}]})RQL"), true, &Result));
			const FString Json = MeshTerrainSpecJson(Result);
			TestTrue(*FString::Printf(TEXT("both plugins in the project file (%s)"), *Json), Json.Contains(TEXT("MeshPartition is")) && Json.Contains(TEXT("MeshTerrainMode is")));
			TestTrue(TEXT("no restart needed"), Json.Contains(TEXT("no restart needed")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
