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
#include "UnrealAIMeshTerrainModifiersApi.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "MeshPartition.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionModifierActor.h"
#include "Components/SplineComponent.h"
#include "EngineUtils.h"
#include "Modifiers/MeshPartitionPatchModifier.h"

// ============================================================================================
//  UE Automation spec for the `meshTerrainModifier` operations (UnrealAIMeshTerrainModifiersApi): add every modifier
//  kind, edit, move, reorder, switch, bind and remove them, undo / redo, bad input. Every check reads the built terrain
//  back (UnrealAIMeshTerrain::SampleSurface, what Surface returns) after the operation returned.
//
//  Plans are the QueryPlans Rql.Parser produces for the query in the comment above them. Terrains are flat ones made
//  with UnrealAIMeshTerrain::SpawnFlatTerrain far from the foundation spec's.
//
//  Run via:  Automation RunTests UnrealAIMeshTerrain.Modifiers
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIMeshTerrainModifiersSpec, "UnrealAIMeshTerrain.Modifiers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	using AMeshPartition = UE::MeshPartition::AMeshPartition;
	using UModifierComponent = UE::MeshPartition::UModifierComponent;

	/** The compact JSON text of an RQL result. */
	static FString ModSpecJson(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** Plan argument: unreal.find("<Label>") */
	static FString ModSpecFind(const FString& Label)
	{
		return FString::Printf(TEXT(R"({"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]})"), *Label);
	}

	/** Plan argument: unreal.find("<Label>").Modifier (the modifier component of a modifier actor) */
	static FString ModSpecModifier(const FString& Label)
	{
		return FString::Printf(TEXT(R"({"path":["Modifier"],"on":%s})"), *ModSpecFind(Label));
	}

	/** Plan argument: [X, Y, Z] */
	static FString ModSpecVec(double X, double Y, double Z)
	{
		return FString::Printf(TEXT(R"({"list":[{"lit":%.17g},{"lit":%.17g},{"lit":%.17g}]})"), X, Y, Z);
	}

	/** Plan argument: [X, Y] */
	static FString ModSpecVec2(double X, double Y)
	{
		return FString::Printf(TEXT(R"({"list":[{"lit":%.17g},{"lit":%.17g}]})"), X, Y);
	}

	static FString ModSpecNum(double Value)
	{
		return FString::Printf(TEXT(R"({"lit":%.17g})"), Value);
	}

	static FString ModSpecStr(const FString& Value)
	{
		return FString::Printf(TEXT(R"({"lit":"%s"})"), *Value);
	}

	/** `meshTerrainModifier | call <Method>(<Args>)` as a plan. */
	static FString ModSpecCall(const FString& Method, const TArray<FString>& Args)
	{
		return FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifier"]},"stages":[{"op":"call","method":"%s","args":[%s]}]})RQL"),
			*Method, *FString::Join(Args, TEXT(",")));
	}

	/** The object in the "result" column of the first row of an RQL call result (null when there is none). */
	static TSharedPtr<FJsonObject> ModSpecResult(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("columns"), Columns) || !Result->TryGetArrayField(TEXT("rows"), Rows) || Rows->IsEmpty())
			return nullptr;
		const int32 Column = Columns->IndexOfByPredicate([](const TSharedPtr<FJsonValue>& Name) { return Name->AsString() == TEXT("result"); });
		const TArray<TSharedPtr<FJsonValue>>& Row = (*Rows)[0]->AsArray();
		return Row.IsValidIndex(Column) && Row[Column]->Type == EJson::Object ? Row[Column]->AsObject() : nullptr;
	}

	static FString ModSpecLabel(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Short));
	}

	/** The modifier of @p Terrain whose actor is labelled @p Label (null when there is none). */
	static UModifierComponent* ModSpecFindModifier(const AMeshPartition* Terrain, const FString& Label)
	{
		for (UModifierComponent* Modifier : UnrealAIMeshTerrain::Modifiers(Terrain))
		{
			if (Modifier->GetOwner() && Modifier->GetOwner()->GetActorLabel() == Label)
				return Modifier;
		}
		return nullptr;
	}

	/** How many modifier actors the editor world has (a refused Add must not leave one behind). */
	static int32 ModSpecModifierActors()
	{
		int32 Count = 0;
		for (TActorIterator<UE::MeshPartition::AModifierActor> It(UnrealMcpRql::EditorWorld()); It; ++It)
			Count += IsValid(*It) ? 1 : 0;
		return Count;
	}

	/** The built height (what traces hit) of @p Terrain at X, Y; -1e9 off the terrain. */
	static double ModSpecHeight(const AMeshPartition* Terrain, double X, double Y)
	{
		const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, X, Y, /*bChannels*/ false);
		return Sample.bInside ? Sample.Height : -1.0e9;
	}

	/** Run an operation: true with its result object when it succeeded and returned with the terrain built. */
	bool ModSpecRun(const FString& What, const FString& Method, const TArray<FString>& Args, TSharedPtr<FJsonObject>* OutResult = nullptr)
	{
		TSharedPtr<FJsonObject> Result;
		const bool bOk = UnrealMcpRqlTools::RunPlan(ModSpecCall(Method, Args), true, &Result);
		const TSharedPtr<FJsonObject> Op = ModSpecResult(Result);
		const bool bReady = bOk && Op.IsValid() && Op->GetBoolField(TEXT("Ready"));
		TestTrue(*FString::Printf(TEXT("%s: %s returns with the terrain built (%s)"), *What, *Method, *ModSpecJson(Result).Left(600)), bReady);
		if (OutResult)
			*OutResult = Op;
		return bReady;
	}

	/** Run an operation that must fail with a message containing @p Expected. */
	void ModSpecRefuses(const FString& What, const FString& Method, const TArray<FString>& Args, const FString& Expected)
	{
		TSharedPtr<FJsonObject> Result;
		TestFalse(*FString::Printf(TEXT("%s is refused"), *What), UnrealMcpRqlTools::RunPlan(ModSpecCall(Method, Args), true, &Result));
		const FString Json = ModSpecJson(Result);
		TestTrue(*FString::Printf(TEXT("%s: the error says '%s' (%s)"), *What, *Expected, *Json.Left(600)), Json.Contains(Expected));
	}

	/** Expect @p Terrain's height at X, Y to be @p Expected (within @p Tolerance cm). */
	void ModSpecExpectHeight(const FString& What, const AMeshPartition* Terrain, double X, double Y, double Expected, double Tolerance = 1.0)
	{
		const double Height = ModSpecHeight(Terrain, X, Y);
		TestTrue(*FString::Printf(TEXT("%s: height at (%.0f, %.0f) is %.1f (got %.2f)"), *What, X, Y, Expected, Height), FMath::IsNearlyEqual(Height, Expected, Tolerance));
	}
END_DEFINE_SPEC(FUnrealAIMeshTerrainModifiersSpec)

void FUnrealAIMeshTerrainModifiersSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("lists the modifier operations with their parameters and the LatticePoints member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("MeshTerrainModifierOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Method : {
				TEXT("AddNoise(MeshPartition Terrain, Vector Location, Vector2D Size, double Strength, double Frequency, double Falloff, string Label)"),
				TEXT("AddPatch(MeshPartition Terrain, Vector Location, double Radius, double Falloff, string Label)"),
				TEXT("AddWeight(MeshPartition Terrain, string Channel, Vector Location, double Radius, double Falloff, double Value, string Label)"),
				TEXT("AddSpline(MeshPartition Terrain, List<Vector> Points, double Width, double Falloff, bool Closed, string Channel, string Label)"),
				TEXT("AddShape(MeshPartition Terrain, string Shape, Vector Location, Vector Size, string Operation, Rotator Rotation, string Label)"),
				TEXT("AddLattice(MeshPartition Terrain, Vector Location, Vector Size, int Resolution, string Label)"),
				TEXT("AddTexturePatch(MeshPartition Terrain, Texture2D Texture, Vector Location, Vector2D Size, double Height, string Label)"),
				TEXT("AddRemesh(MeshPartition Terrain, Vector Location, Vector Size, double EdgeLength, string Label)"),
				TEXT("Add(MeshPartition Terrain, string Kind, Vector Location, string Label)"),
				TEXT("Edit(ModifierComponent Modifier, Vector Size, double Radius, double Width, double Strength, double Falloff)"),
				TEXT("Move(ModifierComponent Modifier, Vector Location)"), TEXT("SetSplinePoints(ModifierComponent Modifier, List<Vector> Points, int Closed)"),
				TEXT("SetLatticePoints(ModifierComponent Modifier, List<Vector> Points)"), TEXT("SetOrder(ModifierComponent Modifier, double Priority, string PriorityLayer)"),
				TEXT("Enable(ModifierComponent Modifier)"), TEXT("Disable(ModifierComponent Modifier)"), TEXT("Bind(ModifierComponent Modifier, MeshPartition Terrain)"),
				TEXT("Remove(ModifierComponent Modifier)") })
			{
				TestTrue(*FString::Printf(TEXT("schema lists %s"), Method), Schema.Contains(Method));
			}
			const FString Lattice = UnrealMcpRqlTools::Schema(TEXT("LatticeModifier"), bOk);
			TestTrue(*FString::Printf(TEXT("LatticeModifier has the LatticePoints member (%s)"), *Lattice.Left(300)), bOk && (Lattice.Contains(TEXT("\"LatticePoints\"")) || Lattice.Contains(TEXT(" LatticePoints ")) || Lattice.Contains(TEXT("{LatticePoints "))));
			TestTrue(TEXT("the skill names the source"), UnrealMcpRqlTools::SkillMarkdown().Contains(TEXT("meshTerrainModifier | call AddNoise(")));
		});
	});

	Describe("operations", [this]()
	{
		It("refuse bad input with a clear error and change nothing", [this]()
		{
			const FString Label = ModSpecLabel(TEXT("ModBad"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-1300000.0, 0.0, 0.0), 4000.0, 4000.0, 40, 40);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestTrue(TEXT("built"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0)))
				return;
			const FString T = ModSpecFind(Label);
			const int32 ActorsBefore = ModSpecModifierActors();
			// meshTerrainModifier | call AddNoise(unreal.find("<label>"), [-1200000, 0, 0])
			ModSpecRefuses(TEXT("a noise off the terrain"), TEXT("AddNoise"), { T, ModSpecVec(-1200000.0, 0.0, 0.0) }, TEXT("would not change the terrain"));
			ModSpecRefuses(TEXT("a negative size"), TEXT("AddNoise"), { T, ModSpecVec(-1300000.0, 0.0, 0.0), ModSpecVec2(-5.0, 100.0) }, TEXT("must be above 0"));
			ModSpecRefuses(TEXT("a zero frequency"), TEXT("AddNoise"), { T, ModSpecVec(-1300000.0, 0.0, 0.0), ModSpecVec2(1000.0, 1000.0), ModSpecNum(100.0), ModSpecNum(0.0) }, TEXT("Frequency 0"));
			ModSpecRefuses(TEXT("a zero radius"), TEXT("AddPatch"), { T, ModSpecVec(-1300000.0, 0.0, 0.0), ModSpecNum(0.0) }, TEXT("Radius 0"));
			ModSpecRefuses(TEXT("an unknown channel"), TEXT("AddWeight"), { T, ModSpecStr(TEXT("Purple")), ModSpecVec(-1300000.0, 0.0, 0.0) }, TEXT("its channels: Red"));
			ModSpecRefuses(TEXT("a one-point spline"), TEXT("AddSpline"), { T, TEXT(R"({"list":[{"list":[{"lit":-1300000},{"lit":0},{"lit":0}]}]})") }, TEXT("2 .. 1000"));
			ModSpecRefuses(TEXT("an unknown shape"), TEXT("AddShape"), { T, ModSpecStr(TEXT("Pyramid")), ModSpecVec(-1300000.0, 0.0, 0.0) }, TEXT("use Box, Sphere"));
			ModSpecRefuses(TEXT("a flat shape union"), TEXT("AddShape"), { T, ModSpecStr(TEXT("Disc")), ModSpecVec(-1300000.0, 0.0, 0.0), ModSpecVec(500.0, 500.0, 0.0), ModSpecStr(TEXT("Union")) }, TEXT("is flat"));
			ModSpecRefuses(TEXT("an unknown operation"), TEXT("AddShape"), { T, ModSpecStr(TEXT("Box")), ModSpecVec(-1300000.0, 0.0, 0.0), ModSpecVec(500.0, 500.0, 500.0), ModSpecStr(TEXT("Explode")) }, TEXT("Union, Subtract, Trim or Project"));
			ModSpecRefuses(TEXT("a lattice resolution of 1"), TEXT("AddLattice"), { T, ModSpecVec(-1300000.0, 0.0, 0.0), ModSpecVec(500.0, 500.0, 500.0), ModSpecNum(1.0) }, TEXT("2 .. 9"));
			ModSpecRefuses(TEXT("a remesh too fine"), TEXT("AddRemesh"), { T, ModSpecVec(-1300000.0, 0.0, 0.0), ModSpecVec(4000.0, 4000.0, 1000.0), ModSpecNum(1.5) }, TEXT("EdgeLength"));
			ModSpecRefuses(TEXT("a base mesh kind"), TEXT("Add"), { T, ModSpecStr(TEXT("MeshProvider")), ModSpecVec(-1300000.0, 0.0, 0.0) }, TEXT("not a modifier kind"));
			ModSpecRefuses(TEXT("a two-point closed spline"), TEXT("AddSpline"), { T, FString::Printf(TEXT(R"({"list":[%s,%s]})"), *ModSpecVec(-1300000.0, 0.0, 0.0), *ModSpecVec(-1299000.0, 0.0, 0.0)),
				ModSpecNum(100.0), ModSpecNum(100.0), TEXT(R"({"lit":true})") }, TEXT("at least 3 points"));
			TestEqual(TEXT("nothing was added to the terrain"), UnrealAIMeshTerrain::Modifiers(Terrain).Num(), 0);
			TestEqual(TEXT("no modifier actor was left in the level"), ModSpecModifierActors(), ActorsBefore);

			// A base mesh is not a modifier: meshTerrainModifier | call Remove(<base mesh>)
			// meshTerrains | where Label == "<label>" | get BaseMeshes | call meshTerrainModifier.Remove($)
			TSharedPtr<FJsonObject> Result;
			TestFalse(TEXT("removing a base mesh is refused"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrains"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"%s"}]}},{"op":"get","expr":{"path":["BaseMeshes"]}},{"op":"call","method":"Remove","on":{"path":["meshTerrainModifier"]},"args":[{"path":[]}]}]})RQL"), *Label), true, &Result));
			TestTrue(*FString::Printf(TEXT("the error says it is a base mesh (%s)"), *ModSpecJson(Result).Left(600)), ModSpecJson(Result).Contains(TEXT("is a base mesh")));
			TestEqual(TEXT("the base mesh stays"), UnrealAIMeshTerrain::Modifiers(Terrain, /*bBases*/ true).Num(), 1);
		});

		It("add a patch, reorder, disable, enable, remove, undo and redo: the ground follows every step", [this]()
		{
			const FString Label = ModSpecLabel(TEXT("ModPatch"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-1400000.0, 0.0, 0.0), 4000.0, 4000.0, 40, 40);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			const FString T = ModSpecFind(Label);
			const FString Low = Label + TEXT("_Low");
			const FString High = Label + TEXT("_High");
			// meshTerrainModifier | call AddPatch(unreal.find("<label>"), [-1400000, 0, 300], 500, 300, "<label>_Low")
			TSharedPtr<FJsonObject> Result;
			if (!ModSpecRun(TEXT("low pad"), TEXT("AddPatch"), { T, ModSpecVec(-1400000.0, 0.0, 300.0), ModSpecNum(500.0), ModSpecNum(300.0), ModSpecStr(Low) }, &Result))
				return;
			TestEqual(TEXT("the result names the patch"), Result->GetStringField(TEXT("Kind")), FString(TEXT("Patch")));
			TestEqual(TEXT("bound to the named terrain"), Result->GetStringField(TEXT("Terrain")), Label);
			ModSpecExpectHeight(TEXT("low pad"), Terrain, -1400000.0, 0.0, 300.0);
			ModSpecExpectHeight(TEXT("outside the pad"), Terrain, -1401500.0, 1500.0, 0.0);
			UModifierComponent* LowPad = ModSpecFindModifier(Terrain, Low);
			TestNotNull(TEXT("the terrain lists the low pad"), LowPad);

			// A second, higher pad over the same place, applied later (higher sub-priority) than the first.
			if (!ModSpecRun(TEXT("high pad"), TEXT("AddPatch"), { T, ModSpecVec(-1400000.0, 0.0, 600.0), ModSpecNum(500.0), ModSpecNum(300.0), ModSpecStr(High) }))
				return;
			// meshTerrainModifier | call SetOrder(unreal.find("<label>_High").Modifier, 5)
			ModSpecRun(TEXT("high pad on top"), TEXT("SetOrder"), { ModSpecModifier(High), ModSpecNum(5.0) });
			ModSpecExpectHeight(TEXT("the later (high) pad wins"), Terrain, -1400000.0, 0.0, 600.0);
			ModSpecRun(TEXT("low pad on top"), TEXT("SetOrder"), { ModSpecModifier(Low), ModSpecNum(10.0) });
			ModSpecExpectHeight(TEXT("reordered: the low pad wins"), Terrain, -1400000.0, 0.0, 300.0);
			TestTrue(TEXT("Modifiers lists them in build order"), UnrealAIMeshTerrain::Modifiers(Terrain).Num() == 2 && UnrealAIMeshTerrain::Modifiers(Terrain).Last() == LowPad);
			ModSpecRefuses(TEXT("an unknown priority layer"), TEXT("SetOrder"), { ModSpecModifier(Low), ModSpecNum(1.0), ModSpecStr(TEXT("NoSuchLayer")) }, TEXT("is not a priority layer"));

			// meshTerrainModifier | call Disable(unreal.find("<label>_Low").Modifier)
			ModSpecRun(TEXT("disable the low pad"), TEXT("Disable"), { ModSpecModifier(Low) }, &Result);
			TestTrue(TEXT("Disabled in the result"), Result.IsValid() && Result->GetBoolField(TEXT("Disabled")));
			ModSpecExpectHeight(TEXT("without the low pad"), Terrain, -1400000.0, 0.0, 600.0);
			ModSpecRun(TEXT("enable it"), TEXT("Enable"), { ModSpecModifier(Low) });
			ModSpecExpectHeight(TEXT("with it again"), Terrain, -1400000.0, 0.0, 300.0);

			// meshTerrainModifier | call Remove(unreal.find("<label>_Low").Modifier)
			ModSpecRun(TEXT("remove the low pad"), TEXT("Remove"), { ModSpecModifier(Low) }, &Result);
			TestTrue(TEXT("Remove names what it removed"), Result.IsValid() && Result->GetStringField(TEXT("Actor")) == Low);
			TestNull(TEXT("the terrain no longer lists it"), ModSpecFindModifier(Terrain, Low));
			ModSpecExpectHeight(TEXT("after removing the low pad"), Terrain, -1400000.0, 0.0, 600.0);

			// Each operation is one undo step: Ctrl+Z brings the low pad back, Ctrl+Y removes it again.
			TestTrue(TEXT("undo"), GEditor->UndoTransaction());
			TestTrue(TEXT("rebuilt after undo"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
			TestNotNull(TEXT("undo restored the low pad"), ModSpecFindModifier(Terrain, Low));
			ModSpecExpectHeight(TEXT("after undo"), Terrain, -1400000.0, 0.0, 300.0);
			TestTrue(TEXT("redo"), GEditor->RedoTransaction());
			TestTrue(TEXT("rebuilt after redo"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
			ModSpecExpectHeight(TEXT("after redo"), Terrain, -1400000.0, 0.0, 600.0);
		});

		It("add noise, every shape, a spline, a weight, a texture patch, a remesh and the other kinds; edit and move them", [this]()
		{
			const FString Label = ModSpecLabel(TEXT("ModKinds"));
			const double X0 = -1500000.0;
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(X0, 0.0, 0.0), 12000.0, 12000.0, 120, 120);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			const FString T = ModSpecFind(Label);

			// meshTerrainModifier | call AddNoise(unreal.find("<label>"), [X0 - 4000, -4000, 0], [3000, 3000], 400, 0.002, 0.3, "<label>_Noise")
			const FString Noise = Label + TEXT("_Noise");
			if (ModSpecRun(TEXT("noise"), TEXT("AddNoise"), { T, ModSpecVec(X0 - 4000.0, -4000.0, 0.0), ModSpecVec2(3000.0, 3000.0), ModSpecNum(400.0), ModSpecNum(0.002), ModSpecNum(0.3), ModSpecStr(Noise) }))
			{
				double Relief = 0.0;
				for (const FVector2D Point : { FVector2D(-300.0, 200.0), FVector2D(500.0, -400.0), FVector2D(-600.0, -500.0), FVector2D(250.0, 650.0) })
					Relief = FMath::Max(Relief, FMath::Abs(ModSpecHeight(Terrain, X0 - 4000.0 + Point.X, -4000.0 + Point.Y)));
				TestTrue(*FString::Printf(TEXT("the noise shaped the ground (%.1f cm)"), Relief), Relief > 20.0);
				ModSpecExpectHeight(TEXT("outside the noise"), Terrain, X0 + 4000.0, 4000.0, 0.0);
				// meshTerrainModifier | call Edit(unreal.find("<label>_Noise").Modifier, [-1, -1, -1], -1, -1, 0)
				ModSpecRun(TEXT("noise strength 0"), TEXT("Edit"), { ModSpecModifier(Noise), ModSpecVec(-1.0, -1.0, -1.0), ModSpecNum(-1.0), ModSpecNum(-1.0), ModSpecNum(0.0) });
				ModSpecExpectHeight(TEXT("a noise of strength 0 is flat"), Terrain, X0 - 4300.0, -3800.0, 0.0);
			}

			// The solid shapes, added (Union) or cut out (Subtract), 1000 cm wide, 600 cm high, centred on the ground.
			struct FShapeCase { const TCHAR* Shape; const TCHAR* Operation; double X; double Y; double Expected; };
			for (const FShapeCase& Case : { FShapeCase{ TEXT("Box"), TEXT("Union"), -1500.0, -4000.0, 300.0 }, FShapeCase{ TEXT("Cylinder"), TEXT("Union"), 1500.0, -4000.0, 300.0 },
				FShapeCase{ TEXT("Sphere"), TEXT("Subtract"), 4000.0, -4000.0, -300.0 }, FShapeCase{ TEXT("Capsule"), TEXT("Union"), -4000.0, -1500.0, 300.0 },
				FShapeCase{ TEXT("Cone"), TEXT("Union"), -1500.0, -1500.0, 300.0 } })
			{
				const FString Name = Label + TEXT("_") + Case.Shape;
				// meshTerrainModifier | call AddShape(unreal.find("<label>"), "<Shape>", [x, y, 0], [1000, 1000, 600], "<Operation>", [0, 0, 0], "<name>")
				if (ModSpecRun(Case.Shape, TEXT("AddShape"), { T, ModSpecStr(Case.Shape), ModSpecVec(X0 + Case.X, Case.Y, 0.0), ModSpecVec(1000.0, 1000.0, 600.0), ModSpecStr(Case.Operation),
					ModSpecVec(0.0, 0.0, 0.0), ModSpecStr(Name) }))
				{
					// The built (collision) mesh is simplified: a cone's tip comes out lower than its model.
					ModSpecExpectHeight(Case.Shape, Terrain, X0 + Case.X, Case.Y, Case.Expected, FCString::Strcmp(Case.Shape, TEXT("Cone")) == 0 ? 120.0 : 15.0);
					ModSpecExpectHeight(*FString::Printf(TEXT("beside the %s"), Case.Shape), Terrain, X0 + Case.X + 900.0, Case.Y, 0.0);
				}
			}
			// A torus lying flat: its ring rises, its hole does not.
			const FString Torus = Label + TEXT("_Torus");
			if (ModSpecRun(TEXT("torus"), TEXT("AddShape"), { T, ModSpecStr(TEXT("Torus")), ModSpecVec(X0 + 1500.0, -1500.0, 0.0), ModSpecVec(2000.0, 2000.0, 400.0), ModSpecStr(TEXT("Union")), ModSpecVec(0.0, 0.0, 0.0), ModSpecStr(Torus) }))
			{
				ModSpecExpectHeight(TEXT("on the torus ring"), Terrain, X0 + 1500.0 + 800.0, -1500.0, 200.0, 15.0);
				ModSpecExpectHeight(TEXT("in the torus hole"), Terrain, X0 + 1500.0, -1500.0, 0.0);
			}
			// The flat shapes press the ground onto their face (Project, their default).
			for (const TCHAR* Shape : { TEXT("Rectangle"), TEXT("Disc") })
			{
				const double X = FCString::Strcmp(Shape, TEXT("Disc")) == 0 ? 4000.0 : -4000.0;
				const FString Name = Label + TEXT("_") + Shape;
				TSharedPtr<FJsonObject> Result;
				if (ModSpecRun(Shape, TEXT("AddShape"), { T, ModSpecStr(Shape), ModSpecVec(X0 + X, 1500.0, 250.0), ModSpecVec(1200.0, 1200.0, 0.0), ModSpecStr(TEXT("")), ModSpecVec(0.0, 0.0, 0.0), ModSpecStr(Name) }, &Result))
				{
					TestEqual(*FString::Printf(TEXT("a %s is a projection"), Shape), Result->GetStringField(TEXT("Kind")), FString(TEXT("MeshProject")));
					ModSpecExpectHeight(Shape, Terrain, X0 + X, 1500.0, 250.0);
					ModSpecExpectHeight(*FString::Printf(TEXT("off the %s"), Shape), Terrain, X0 + X + 1200.0, 1500.0, 0.0);
				}
			}
			// meshTerrainModifier | call Edit(unreal.find("<label>_Box").Modifier, [1000, 1000, 1000]): the box grows 200 cm taller.
			const FString Box = Label + TEXT("_Box");
			ModSpecRun(TEXT("taller box"), TEXT("Edit"), { ModSpecModifier(Box), ModSpecVec(-1.0, -1.0, 1000.0) });
			ModSpecExpectHeight(TEXT("the taller box"), Terrain, X0 - 1500.0, -4000.0, 500.0, 15.0);
			TestTrue(TEXT("undo the edit"), GEditor->UndoTransaction());
			TestTrue(TEXT("rebuilt after undo"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
			ModSpecExpectHeight(TEXT("undo restores the box's size"), Terrain, X0 - 1500.0, -4000.0, 300.0, 15.0);
			TestTrue(TEXT("redo the edit"), GEditor->RedoTransaction());
			TestTrue(TEXT("rebuilt after redo"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
			ModSpecExpectHeight(TEXT("redo makes it taller again"), Terrain, X0 - 1500.0, -4000.0, 500.0, 15.0);

			// meshTerrainModifier | call AddSpline(unreal.find("<label>"), [[X0 - 5000, 4000, 150], [X0 + 5000, 4000, 150]], 400, 300, false, "Red", "<label>_Road")
			const FString Road = Label + TEXT("_Road");
			if (ModSpecRun(TEXT("road"), TEXT("AddSpline"), { T, FString::Printf(TEXT(R"({"list":[%s,%s]})"), *ModSpecVec(X0 - 5000.0, 4000.0, 150.0), *ModSpecVec(X0 + 5000.0, 4000.0, 150.0)),
				ModSpecNum(400.0), ModSpecNum(300.0), TEXT(R"({"lit":false})"), ModSpecStr(TEXT("Red")), ModSpecStr(Road) }))
			{
				ModSpecExpectHeight(TEXT("on the road"), Terrain, X0, 4000.0, 150.0, 2.0);
				ModSpecExpectHeight(TEXT("beyond the road's falloff"), Terrain, X0, 4800.0, 0.0, 2.0);
				const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, X0, 4000.0);
				TestTrue(TEXT("the road paints Red"), Sample.Channels.Contains(TEXT("Red")) && Sample.Channels[TEXT("Red")] > 0.9f);
				// meshTerrainModifier | call SetSplinePoints(unreal.find("<label>_Road").Modifier, [[X0 - 5000, 5000, 100], [X0 + 5000, 5000, 100]])
				ModSpecRun(TEXT("moved road"), TEXT("SetSplinePoints"), { ModSpecModifier(Road), FString::Printf(TEXT(R"({"list":[%s,%s]})"), *ModSpecVec(X0 - 5000.0, 5000.0, 100.0), *ModSpecVec(X0 + 5000.0, 5000.0, 100.0)) });
				ModSpecExpectHeight(TEXT("on the moved road"), Terrain, X0, 5000.0, 100.0, 2.0);
				ModSpecExpectHeight(TEXT("where the road was"), Terrain, X0, 4000.0, 0.0, 2.0);
				ModSpecRefuses(TEXT("spline points on a box"), TEXT("SetSplinePoints"), { ModSpecModifier(Box), FString::Printf(TEXT(R"({"list":[%s,%s]})"), *ModSpecVec(X0, 0.0, 0.0), *ModSpecVec(X0 + 100.0, 0.0, 0.0)) }, TEXT("has no spline"));
			}

			// meshTerrainModifier | call AddWeight(unreal.find("<label>"), "Green", [X0 + 2000, 2500, 0], 400, 200, 0.8, "<label>_Green")
			const FString Green = Label + TEXT("_Green");
			if (ModSpecRun(TEXT("weight"), TEXT("AddWeight"), { T, ModSpecStr(TEXT("Green")), ModSpecVec(X0 + 2000.0, 2500.0, 0.0), ModSpecNum(400.0), ModSpecNum(200.0), ModSpecNum(0.8), ModSpecStr(Green) }))
			{
				UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, X0 + 2000.0, 2500.0);
				TestTrue(TEXT("Green 0.8 under the weight"), Sample.Channels.Contains(TEXT("Green")) && FMath::IsNearlyEqual(Sample.Channels[TEXT("Green")], 0.8f, 0.02f));
				TestTrue(TEXT("the ground keeps its shape"), Sample.bInside && FMath::IsNearlyZero(Sample.Height, 1.0));
				// meshTerrainModifier | call Edit(unreal.find("<label>_Green").Modifier, [-1, -1, -1], -1, -1, 0.3)
				ModSpecRun(TEXT("weaker weight"), TEXT("Edit"), { ModSpecModifier(Green), ModSpecVec(-1.0, -1.0, -1.0), ModSpecNum(-1.0), ModSpecNum(-1.0), ModSpecNum(0.3) });
				Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, X0 + 2000.0, 2500.0);
				TestTrue(TEXT("Green 0.3 after Edit"), Sample.Channels.Contains(TEXT("Green")) && FMath::IsNearlyEqual(Sample.Channels[TEXT("Green")], 0.3f, 0.02f));
			}

			// A 300 m stamp: white raises half the Height. The engine's default vertical extents (100 m up and down) would clamp it.
			// meshTerrainModifier | call AddTexturePatch(unreal.find("<label>"), "/Engine/EngineResources/WhiteSquareTexture", [X0, 0, 0], [1000, 1000], 60000)
			if (ModSpecRun(TEXT("texture patch"), TEXT("AddTexturePatch"), { T, ModSpecStr(TEXT("/Engine/EngineResources/WhiteSquareTexture")), ModSpecVec(X0, 0.0, 0.0), ModSpecVec2(1000.0, 1000.0), ModSpecNum(60000.0),
				ModSpecStr(Label + TEXT("_Tex")) }))
			{
				const auto Extent = [this, Terrain, &Label](const TCHAR* Name)
				{
					UModifierComponent* Patch = ModSpecFindModifier(Terrain, Label + TEXT("_Tex"));
					const FNumericProperty* Property = Patch ? CastField<FNumericProperty>(Patch->GetClass()->FindPropertyByName(Name)) : nullptr;
					return Property ? Property->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Patch)) : -1.0;
				};
				TestTrue(*FString::Printf(TEXT("the patch reaches its Height up and down (%.0f / %.0f)"), Extent(TEXT("VerticalPatchExtentUp")), Extent(TEXT("VerticalPatchExtentDown"))),
					Extent(TEXT("VerticalPatchExtentUp")) >= 60000.0 && Extent(TEXT("VerticalPatchExtentDown")) >= 60000.0);
				// Mesh Partition reads a texture patch's texture through the renderer: under -nullrhi (the suite) it applies nothing.
				if (FApp::CanEverRender())
					ModSpecExpectHeight(TEXT("white raises half the Height, 300 m"), Terrain, X0, 0.0, 30000.0, 20.0);
				else
					AddInfo(TEXT("-nullrhi: a texture patch changes no height without a renderer (engine); its height is checked in a rendering editor"));
				// meshTerrainModifier | call Edit(unreal.find("<label>_Tex").Modifier, [-1, -1, -1], -1, -1, 80000): a higher stamp widens them again.
				ModSpecRun(TEXT("higher stamp"), TEXT("Edit"), { ModSpecModifier(Label + TEXT("_Tex")), ModSpecVec(-1.0, -1.0, -1.0), ModSpecNum(-1.0), ModSpecNum(-1.0), ModSpecNum(80000.0) });
				TestTrue(*FString::Printf(TEXT("Edit widens the extents (%.0f)"), Extent(TEXT("VerticalPatchExtentUp"))), Extent(TEXT("VerticalPatchExtentUp")) >= 80000.0);
				// Back to a low stamp: the shapes placed later around it are checked at ground level.
				ModSpecRun(TEXT("low stamp"), TEXT("Edit"), { ModSpecModifier(Label + TEXT("_Tex")), ModSpecVec(-1.0, -1.0, -1.0), ModSpecNum(-1.0), ModSpecNum(-1.0), ModSpecNum(1000.0) });
			}

			// meshTerrainModifier | call AddRemesh(unreal.find("<label>"), [X0 + 4000, -1500, 0], [2000, 2000, 50000], 100)
			ModSpecRun(TEXT("remesh"), TEXT("AddRemesh"), { T, ModSpecVec(X0 + 4000.0, -1500.0, 0.0), ModSpecVec(2000.0, 2000.0, 50000.0), ModSpecNum(100.0), ModSpecStr(Label + TEXT("_Remesh")) });
			ModSpecExpectHeight(TEXT("a remesh keeps flat ground flat"), Terrain, X0 + 4000.0, -1500.0, 0.0);

			// The other kinds with their defaults: meshTerrainModifier | call Add(unreal.find("<label>"), "<Kind>", [x, y, 0])
			for (const TCHAR* Kind : { TEXT("InstancedPatch"), TEXT("SplineRemesh"), TEXT("MeshProject"), TEXT("Boolean"), TEXT("ProjectMeshLayers"), TEXT("Lattice") })
			{
				TSharedPtr<FJsonObject> Result;
				if (ModSpecRun(Kind, TEXT("Add"), { T, ModSpecStr(Kind), ModSpecVec(X0, -5500.0, 0.0) }, &Result))
					TestEqual(*FString::Printf(TEXT("a %s"), Kind), Result->GetStringField(TEXT("Kind")), FString(Kind));
			}
			ModSpecExpectHeight(TEXT("empty defaults leave the ground"), Terrain, X0 + 500.0, -5500.0, 0.0, 2.0);

			// meshTerrainModifier | call Move(unreal.find("<label>_Rectangle").Modifier, [X0 - 4000, 3000, 100])
			const FString Rectangle = Label + TEXT("_Rectangle");
			ModSpecRun(TEXT("move the rectangle"), TEXT("Move"), { ModSpecModifier(Rectangle), ModSpecVec(X0 - 4000.0, 3000.0, 100.0) });
			ModSpecExpectHeight(TEXT("the pad moved"), Terrain, X0 - 4000.0, 3000.0, 100.0);
			ModSpecExpectHeight(TEXT("the old pad is gone"), Terrain, X0 - 4000.0, 1500.0, 0.0, 2.0);
		});

		It("deform with a lattice: its points move the ground, Edit scales them, Move carries them", [this]()
		{
			const FString Label = ModSpecLabel(TEXT("ModLattice"));
			const double X0 = -1600000.0;
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(X0, 0.0, 0.0), 8000.0, 8000.0, 80, 80);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			const FString Lattice = Label + TEXT("_Lattice");
			// meshTerrainModifier | call AddLattice(unreal.find("<label>"), [X0, 0, 0], [2000, 2000, 1000], 3, "<label>_Lattice")
			if (!ModSpecRun(TEXT("lattice"), TEXT("AddLattice"), { ModSpecFind(Label), ModSpecVec(X0, 0.0, 0.0), ModSpecVec(2000.0, 2000.0, 1000.0), ModSpecNum(3.0), ModSpecStr(Lattice) }))
				return;
			ModSpecExpectHeight(TEXT("an undeformed lattice keeps the ground"), Terrain, X0, 0.0, 0.0);
			UModifierComponent* Modifier = ModSpecFindModifier(Terrain, Lattice);
			TArray<FVector> Points = UnrealAIMeshTerrain::LatticeWorldPoints(Modifier);
			if (!TestEqual(TEXT("3 x 3 x 3 points"), Points.Num(), 27))
				return;
			TestTrue(*FString::Printf(TEXT("the centre point is at the lattice (%s)"), *Points[13].ToString()), Points[13].Equals(FVector(X0, 0.0, 0.0), 0.5));
			// meshTerrainModifier | call SetLatticePoints(unreal.find("<label>_Lattice").Modifier, <points, the centre one 600 cm up>)
			Points[13].Z += 600.0;
			TArray<FString> List;
			for (const FVector& Point : Points)
				List.Add(ModSpecVec(Point.X, Point.Y, Point.Z));
			ModSpecRun(TEXT("lift the centre"), TEXT("SetLatticePoints"), { ModSpecModifier(Lattice), FString::Printf(TEXT(R"({"list":[%s]})"), *FString::Join(List, TEXT(","))) });
			ModSpecExpectHeight(TEXT("the ground follows the lifted point"), Terrain, X0, 0.0, 600.0, 5.0);
			ModSpecExpectHeight(TEXT("the lattice's edge stays"), Terrain, X0 + 1100.0, 0.0, 0.0);
			ModSpecRefuses(TEXT("a wrong point count"), TEXT("SetLatticePoints"), { ModSpecModifier(Lattice), FString::Printf(TEXT(R"({"list":[%s]})"), *ModSpecVec(X0, 0.0, 0.0)) }, TEXT("the lattice has 27"));
			// meshTerrainModifier | call Edit(<lattice>, [4000, 4000, 1000]): twice as wide, the bump spreads.
			ModSpecRun(TEXT("wider lattice"), TEXT("Edit"), { ModSpecModifier(Lattice), ModSpecVec(4000.0, 4000.0, -1.0) });
			ModSpecExpectHeight(TEXT("the centre stays lifted"), Terrain, X0, 0.0, 600.0, 5.0);
			ModSpecExpectHeight(TEXT("the bump reaches further"), Terrain, X0 + 1000.0, 0.0, 300.0, 15.0);
			// meshTerrainModifier | call Move(<lattice>, [X0 - 1000, 0, 0])
			ModSpecRun(TEXT("moved lattice"), TEXT("Move"), { ModSpecModifier(Lattice), ModSpecVec(X0 - 1000.0, 0.0, 0.0) });
			ModSpecExpectHeight(TEXT("the bump moved with it"), Terrain, X0 - 1000.0, 0.0, 600.0, 5.0);
		});

		It("list the base meshes and modifiers a deleted terrain left behind as bound to no terrain, and Remove them", [this]()
		{
			const FString Label = ModSpecLabel(TEXT("ModOrphan"));
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(-1400000.0, 0.0, 0.0), 4000.0, 4000.0, 40, 40);
			if (!TestNotNull(TEXT("terrain spawned"), Terrain) || !TestTrue(TEXT("built"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0)))
			{
				UnrealAIMeshTerrain::DestroyTerrain(Terrain);
				return;
			}
			const TArray<UModifierComponent*> Bases = UnrealAIMeshTerrain::Modifiers(Terrain, /*bBases*/ true);
			if (!TestEqual(TEXT("one base mesh"), Bases.Num(), 1))
			{
				UnrealAIMeshTerrain::DestroyTerrain(Terrain);
				return;
			}
			TWeakObjectPtr<AActor> BaseActor = Bases[0]->GetOwner();
			const FString BaseLabel = BaseActor->GetActorLabel();
			ON_SCOPE_EXIT { if (BaseActor.IsValid()) BaseActor->GetWorld()->EditorDestroyActor(BaseActor.Get(), true); };
			// The terrain actor deleted alone (the Outliner's Delete): its base mesh stays, bound to nothing.
			Terrain->GetWorld()->EditorDestroyActor(Terrain, /*bShouldModifyLevel*/ true);

			// meshTerrainModifiers | where Terrain == null | select Modifier
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("orphans query"), UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifiers"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Terrain"]},{"lit":null}]}},{"op":"select","fields":[{"name":"Modifier","expr":{"path":["Modifier"]}}]}]})RQL"), false, &Result));
			const FString Json = ModSpecJson(Result);
			TestTrue(*FString::Printf(TEXT("the left-behind base mesh is listed (%s)"), *Json.Left(800)), Json.Contains(FString::Printf(TEXT("\"Actor\":\"%s\""), *BaseLabel)) && Json.Contains(TEXT("\"IsBase\":true")));

			// meshTerrainModifiers | where Terrain == null and Modifier.Actor == "<base label>" | call meshTerrainModifier.Remove($)
			TestTrue(*FString::Printf(TEXT("Remove takes the left-behind base mesh (%s)"), *ModSpecJson(Result).Left(600)), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifiers"]},"stages":[{"op":"where","pred":{"op":"and","args":[{"op":"==","args":[{"path":["Terrain"]},{"lit":null}]},{"op":"==","args":[{"path":["Modifier","Actor"]},{"lit":"%s"}]}]}},{"op":"call","method":"Remove","on":{"path":["meshTerrainModifier"]},"args":[{"path":[]}]}]})RQL"), *BaseLabel), true, &Result));
			TestTrue(TEXT("its actor is gone, not left empty"), !BaseActor.IsValid() || BaseActor->IsActorBeingDestroyed() || !IsValid(BaseActor.Get()));
		});

		It("bind to the terrain they name, not the nearest one, and move between two terrains with Bind", [this]()
		{
			const FString First = ModSpecLabel(TEXT("ModFirst"));
			const FString Second = ModSpecLabel(TEXT("ModSecond"));
			// Two terrains side by side; the modifier factory binds the first spawned / nearest by its own rule.
			AMeshPartition* A = UnrealAIMeshTerrain::SpawnFlatTerrain(First, FVector(-1700000.0, 0.0, 0.0), 4000.0, 4000.0, 40, 40);
			AMeshPartition* B = UnrealAIMeshTerrain::SpawnFlatTerrain(Second, FVector(-1700000.0, 4000.0, 100.0), 4000.0, 4000.0, 40, 40);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(A); UnrealAIMeshTerrain::DestroyTerrain(B); };
			if (!TestNotNull(TEXT("first terrain"), A) || !TestNotNull(TEXT("second terrain"), B) || !TestTrue(TEXT("first built"), UnrealAIMeshTerrain::WaitForBuild(A, 60.0)))
				return;
			const FString Pad = Second + TEXT("_Pad");
			// meshTerrainModifier | call AddPatch(unreal.find("<second>"), [-1700000, 2500, 400], 800, 400, "<second>_Pad")
			TSharedPtr<FJsonObject> Result;
			if (!ModSpecRun(TEXT("pad on the second terrain"), TEXT("AddPatch"), { ModSpecFind(Second), ModSpecVec(-1700000.0, 2500.0, 400.0), ModSpecNum(800.0), ModSpecNum(400.0), ModSpecStr(Pad) }, &Result))
				return;
			TestEqual(TEXT("bound to the second terrain"), Result->GetStringField(TEXT("Terrain")), Second);
			TestNotNull(TEXT("the second terrain lists it"), ModSpecFindModifier(B, Pad));
			TestNull(TEXT("the first does not"), ModSpecFindModifier(A, Pad));
			ModSpecExpectHeight(TEXT("the second terrain's pad"), B, -1700000.0, 2500.0, 400.0);
			ModSpecExpectHeight(TEXT("the first terrain is untouched"), A, -1700000.0, 1500.0, 0.0);

			// meshTerrainModifier | call Bind(unreal.find("<second>_Pad").Modifier, unreal.find("<first>")): it still covers the first terrain's edge.
			ModSpecRun(TEXT("bind to the first"), TEXT("Bind"), { ModSpecModifier(Pad), ModSpecFind(First) });
			TestNotNull(TEXT("the first terrain lists it now"), ModSpecFindModifier(A, Pad));
			ModSpecExpectHeight(TEXT("the second terrain lost the pad"), B, -1700000.0, 2500.0, 100.0);
			ModSpecExpectHeight(TEXT("the first terrain's edge rose"), A, -1700000.0, 1900.0, 400.0);
		});

		It("refuse a partial Edit whole, keep a closed spline closed, remove only the modifier of a shared actor, place a default Torus", [this]()
		{
			const FString Label = ModSpecLabel(TEXT("ModEdge"));
			const double X0 = -1900000.0;
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(X0, 0.0, 0.0), 8000.0, 8000.0, 80, 80);
			AActor* Shared = nullptr;
			ON_SCOPE_EXIT
			{
				if (IsValid(Shared))
					Shared->GetWorld()->EditorDestroyActor(Shared, true);
				UnrealAIMeshTerrain::DestroyTerrain(Terrain);
			};
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			const FString T = ModSpecFind(Label);

			// An Edit with one parameter the kind lacks changes nothing: meshTerrainModifier | call Edit(<noise>, [3000, 3000, -1], 500)
			const FString Noise = Label + TEXT("_Noise");
			if (ModSpecRun(TEXT("noise"), TEXT("AddNoise"), { T, ModSpecVec(X0 - 2000.0, -2000.0, 0.0), ModSpecVec2(2000.0, 2000.0), ModSpecNum(300.0), ModSpecNum(0.002), ModSpecNum(0.3), ModSpecStr(Noise) }))
			{
				const UModifierComponent* NoiseModifier = ModSpecFindModifier(Terrain, Noise);
				ModSpecRefuses(TEXT("a noise Radius"), TEXT("Edit"), { ModSpecModifier(Noise), ModSpecVec(3000.0, 3000.0, -1.0), ModSpecNum(500.0) }, TEXT("has no Radius"));
				const FStructProperty* Coverage = NoiseModifier ? CastField<FStructProperty>(NoiseModifier->GetClass()->FindPropertyByName(TEXT("UnscaledCoverage"))) : nullptr;
				TestTrue(TEXT("the refused Edit kept the size"), Coverage && FMath::IsNearlyEqual(Coverage->ContainerPtrToValuePtr<FVector>(const_cast<UModifierComponent*>(NoiseModifier))->X, 2000.0));
			}

			// A closed spline stays closed when only its points change (Closed -1).
			const FString Plateau = Label + TEXT("_Plateau");
			const auto Square = [X0](double Half, double Z)
			{
				TArray<FString> Points;
				for (const FVector2D Corner : { FVector2D(-1.0, -1.0), FVector2D(1.0, -1.0), FVector2D(1.0, 1.0), FVector2D(-1.0, 1.0) })
					Points.Add(ModSpecVec(X0 + 2000.0 + Corner.X * Half, 2000.0 + Corner.Y * Half, Z));
				return FString::Printf(TEXT(R"({"list":[%s]})"), *FString::Join(Points, TEXT(",")));
			};
			if (ModSpecRun(TEXT("closed spline"), TEXT("AddSpline"), { T, Square(800.0, 200.0), ModSpecNum(0.0), ModSpecNum(300.0), TEXT(R"({"lit":true})"), ModSpecStr(TEXT("")), ModSpecStr(Plateau) }))
			{
				ModSpecExpectHeight(TEXT("inside the closed spline"), Terrain, X0 + 2000.0, 2000.0, 200.0, 2.0);
				ModSpecRun(TEXT("new points"), TEXT("SetSplinePoints"), { ModSpecModifier(Plateau), Square(1000.0, 250.0) });
				const UModifierComponent* Spline = ModSpecFindModifier(Terrain, Plateau);
				const USplineComponent* Path = Spline ? Spline->GetOwner()->FindComponentByClass<USplineComponent>() : nullptr;
				TestTrue(TEXT("still a closed loop"), Path && Path->IsClosedLoop());
				ModSpecExpectHeight(TEXT("still filled"), Terrain, X0 + 2000.0, 2000.0, 250.0, 2.0);
			}

			// A modifier component on an actor that is not a modifier actor: Remove takes the component, not the actor.
			Shared = Terrain->GetWorld()->SpawnActor<AActor>(FVector(X0 - 2000.0, 2000.0, 300.0), FRotator::ZeroRotator);
			if (TestNotNull(TEXT("a plain actor"), Shared))
			{
				Shared->SetActorLabel(Label + TEXT("_Shared"));
				USceneComponent* Root = NewObject<USceneComponent>(Shared, TEXT("Root"));
				Shared->SetRootComponent(Root);
				Root->RegisterComponent();
				Root->SetWorldLocation(FVector(X0 - 2000.0, 2000.0, 300.0));
				UE::MeshPartition::UPatchModifier* Patch = NewObject<UE::MeshPartition::UPatchModifier>(Shared, TEXT("Patch"));
				Patch->SetupAttachment(Root);
				Shared->AddInstanceComponent(Patch);
				Patch->RegisterComponent();
				Patch->BP_SetAffectedMegaMesh(Terrain);
				TestTrue(TEXT("built with the shared patch"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
				const FUnrealAIMeshTerrainModifierResult Removed = GetMutableDefault<UUnrealAIMeshTerrainModifiersApi>()->Remove(Patch);
				TestTrue(TEXT("Remove returned built"), Removed.Ready);
				TestTrue(TEXT("the plain actor stays"), IsValid(Shared));
				TestFalse(TEXT("its patch is gone"), IsValid(Patch) && Patch->IsRegistered());
				TestTrue(TEXT("the terrain stays"), IsValid(Terrain));
			}

			// A Torus at the default Size (a 1000 cm cube) is a ring 1000 cm high.
			TSharedPtr<FJsonObject> Result;
			if (ModSpecRun(TEXT("default torus"), TEXT("AddShape"), { T, ModSpecStr(TEXT("Torus")), ModSpecVec(X0 + 2000.0, -2000.0, 0.0) }, &Result))
			{
				TestTrue(TEXT("a unique default label"), Result->GetStringField(TEXT("Actor")).StartsWith(TEXT("Torus")));
				ModSpecExpectHeight(TEXT("on the default torus ring"), Terrain, X0 + 2000.0 + 333.0, -2000.0, 500.0, 60.0);
				ModSpecExpectHeight(TEXT("in its hole"), Terrain, X0 + 2000.0, -2000.0, 0.0);
			}
		});

		It("paint a 300 m dome and raise a plateau as high: Weight and Patch reach 300 m, Surface reads the channel on the dome's large triangles", [this]()
		{
			const FString Label = ModSpecLabel(TEXT("ModDome"));
			const double X0 = -2600000.0;
			const double Y0 = -3000000.0;
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(X0, Y0, 0.0), 160000.0, 160000.0, 80, 80);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			const FString T = ModSpecFind(Label);
			// A 1200 m wide, 300 m high dome: a lat-long sphere's triangles there are 100+ m across.
			// meshTerrainModifier | call AddShape(unreal.find("<label>"), "Sphere", [X0, Y0, 0], [120000, 120000, 60000], "Union", [0, 0, 0], "<label>_Dome")
			if (!ModSpecRun(TEXT("dome"), TEXT("AddShape"), { T, ModSpecStr(TEXT("Sphere")), ModSpecVec(X0, Y0, 0.0), ModSpecVec(120000.0, 120000.0, 60000.0), ModSpecStr(TEXT("Union")),
				ModSpecVec(0.0, 0.0, 0.0), ModSpecStr(Label + TEXT("_Dome")) }))
				return;
			ModSpecExpectHeight(TEXT("the dome's top"), Terrain, X0, Y0, 30000.0, 300.0);
			// A 30 m stroke on the dome's flank, between its lat-long vertices (rings 230 and 333 m out, every 15 degrees; the base mesh
			// has one every 20 m), finds none of them: the error says why, not only "a vertex every 20 m".
			// meshTerrainBrush | call Sculpt(unreal.find("<label>"), "Raise", [X0 + 27760, Y0 + 3655, 0], 3000, 100)
			{
				TSharedPtr<FJsonObject> Refused;
				TestFalse(TEXT("a 30 m stroke on the dome's flank is refused"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainBrush"]},"stages":[{"op":"call","method":"Sculpt","args":[%s,{"lit":"Raise"},%s,{"lit":3000},{"lit":100}]}]})RQL"),
					*T, *ModSpecVec(X0 + 27760.0, Y0 + 3655.0, 0.0)), true, &Refused));
				TestTrue(*FString::Printf(TEXT("the error names the large triangles (%s)"), *ModSpecJson(Refused).Left(600)), ModSpecJson(Refused).Contains(TEXT("made of large triangles")));
			}
			// meshTerrains | where Label == "<label>" | get Modifiers | select Modifier: a Boolean is in none of the default definition's layers.
			TSharedPtr<FJsonObject> Listed;
			TestTrue(TEXT("list the modifiers"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrains"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"%s"}]}},{"op":"get","expr":{"path":["Modifiers"]}},{"op":"select","fields":[{"name":"Modifier","expr":{"path":["Modifier"]}}]}]})RQL"), *Label), false, &Listed));
			const FString ListedJson = ModSpecJson(Listed);
			TestTrue(*FString::Printf(TEXT("LayerIndex -1 outside the definition's layers (%s)"), *ListedJson.Left(600)), ListedJson.Contains(TEXT("\"LayerIndex\":-1")) && !ListedJson.Contains(TEXT("4294967295")));
			// meshTerrainModifier | call AddWeight(unreal.find("<label>"), "Red", [X0, Y0, 0], 70000, 1000, 1, "<label>_Rock"): placed on the ground, 300 m below the top.
			if (!ModSpecRun(TEXT("paint the dome"), TEXT("AddWeight"), { T, ModSpecStr(TEXT("Red")), ModSpecVec(X0, Y0, 0.0), ModSpecNum(70000.0), ModSpecNum(1000.0), ModSpecNum(1.0),
				ModSpecStr(Label + TEXT("_Rock")) }))
				return;
			for (const FVector2D Point : { FVector2D(X0, Y0), FVector2D(X0 + 30000.0, Y0 - 20000.0), FVector2D(X0 - 50000.0, Y0) })
			{
				const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, Point.X, Point.Y);
				const float* Red = Sample.Channels.Find(TEXT("Red"));
				TestTrue(*FString::Printf(TEXT("Red painted at %s, %.0f cm up (%s)"), *Point.ToString(), Sample.Height, Red ? *FString::SanitizeFloat(*Red) : TEXT("no channels")),
					Sample.bInside && Red && *Red >= 0.9f);
			}
			const UnrealAIMeshTerrain::FSurfaceSample Outside = UnrealAIMeshTerrain::SampleSurface(Terrain, X0 + 75000.0, Y0 + 75000.0);
			TestTrue(TEXT("not painted outside"), Outside.bInside && Outside.Channels.Contains(TEXT("Red")) && Outside.Channels.FindRef(TEXT("Red")) <= 0.1f);
			// A plateau as high as the dome on the flat ground beside it: 300 m above the ground there. (A Patch moves the
			// vertices within its radius: on the dome's 100+ m triangles a small pad has none to move.)
			// meshTerrainModifier | call AddPatch(unreal.find("<label>"), [X0 + 70000, Y0 + 70000, 30000], 3000, 1000, "<label>_Plateau")
			if (ModSpecRun(TEXT("plateau"), TEXT("AddPatch"), { T, ModSpecVec(X0 + 70000.0, Y0 + 70000.0, 30000.0), ModSpecNum(3000.0), ModSpecNum(1000.0), ModSpecStr(Label + TEXT("_Plateau")) }))
				ModSpecExpectHeight(TEXT("the plateau is level at its Z, 300 m above the ground"), Terrain, X0 + 70000.0, Y0 + 70000.0, 30000.0, 5.0);
		});

		It("rebuild after RQL set on a modifier and WaitForBuild", [this]()
		{
			const FString Label = ModSpecLabel(TEXT("ModSet"));
			const double X0 = -1800000.0;
			AMeshPartition* Terrain = UnrealAIMeshTerrain::SpawnFlatTerrain(Label, FVector(X0, 0.0, 0.0), 4000.0, 4000.0, 40, 40);
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(Terrain); };
			if (!TestNotNull(TEXT("terrain spawned"), Terrain))
				return;
			const FString Pad = Label + TEXT("_Pad");
			if (!ModSpecRun(TEXT("pad"), TEXT("AddPatch"), { ModSpecFind(Label), ModSpecVec(X0, 0.0, 300.0), ModSpecNum(500.0), ModSpecNum(300.0), ModSpecStr(Pad) }))
				return;
			// meshTerrainModifiers | where Owner.Label == "<label>_Pad" | set Radius = 1200
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("set Radius"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifiers"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Owner","Label"]},{"lit":"%s"}]}},{"op":"set","assign":[{"target":{"path":["Radius"]},"value":{"lit":1200}}]}]})RQL"), *Pad), true, &Result));
			TestTrue(TEXT("WaitForBuild"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
			ModSpecExpectHeight(TEXT("the wider pad"), Terrain, X0 + 1000.0, 0.0, 300.0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
