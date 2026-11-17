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
#include "HAL/FileManager.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionModifierActor.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionPreviewSection.h"
#include "Modifiers/MeshPartitionNoiseModifier.h"

// ============================================================================================
//  UE Automation spec for the Mesh Terrain create operations (UnrealAIMeshTerrainCreateApi): `meshTerrainCreate | call
//  Create / ImportHeightmap / Delete`, their limits, units, undo / redo and refusals.
//
//  Plans are the QueryPlans Rql.Parser produces for the query in the comment above them (the C++ runtime has no text
//  parser). Heightmaps are written by the spec (a 16-bit PNG and a .r16) into Saved/UnrealAIMeshTerrainSpec.
//
//  Run via:  Automation RunTests UnrealAIMeshTerrain.Create
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIMeshTerrainCreateSpec, "UnrealAIMeshTerrain.Create",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	using AMeshPartition = UE::MeshPartition::AMeshPartition;

	static FString CreateSpecJson(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** `meshTerrainCreate | call <Method>(<Args>)` as a plan; @p Args are plan argument JSON values. */
	static FString CreateSpecCall(const FString& Method, const FString& Args)
	{
		return FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainCreate"]},"stages":[{"op":"call","method":"%s","args":[%s]}]})RQL"), *Method, *Args);
	}

	/** Plan arguments: a label literal, then a [X, Y, Z] location list, then @p Rest (already JSON). */
	static FString CreateSpecArgs(const FString& Label, const FVector& Location, const FString& Rest)
	{
		return FString::Printf(TEXT(R"({"lit":"%s"},{"list":[{"lit":%.17g},{"lit":%.17g},{"lit":%.17g}]}%s%s)"),
			*Label, Location.X, Location.Y, Location.Z, Rest.IsEmpty() ? TEXT("") : TEXT(","), *Rest);
	}

	/** `meshTerrainCreate | call Delete(unreal.find("<Label>"))` */
	static FString CreateSpecDelete(const FString& Label)
	{
		return CreateSpecCall(TEXT("Delete"), FString::Printf(TEXT(R"({"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]})"), *Label));
	}

	static FString CreateSpecLabel(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Short));
	}

	/** The terrain of the editor world labelled @p Label (null when none). */
	static AMeshPartition* CreateSpecTerrain(const FString& Label)
	{
		for (AMeshPartition* Terrain : UnrealAIMeshTerrain::Terrains())
		{
			if (Terrain->GetActorLabel() == Label)
				return Terrain;
		}
		return nullptr;
	}

	/** The full-resolution height of @p Terrain's modifier stack at X, Y (the rendered mesh is simplified). */
	static double CreateSpecModelHeight(const AMeshPartition* Terrain, double X, double Y)
	{
		return UnrealAIMeshTerrain::SampleSurface(Terrain, X, Y, /*bChannels*/ true).ModelHeight;
	}

	/** The spec's scratch folder for heightmaps (Saved/UnrealAIMeshTerrainSpec). */
	static FString CreateSpecFile(const FString& Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealAIMeshTerrainSpec"), Name));
	}

	/** Remove the spec's scratch folder (each heightmap test calls it on exit). */
	static void CreateSpecCleanFiles()
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(CreateSpecFile(TEXT("x"))), /*RequireExists*/ false, /*Tree*/ true);
	}

	/** A @p Width x @p Height heightmap: value 32768 everywhere, @p Peak at pixel (PeakX, PeakY). */
	static TArray<uint16> CreateSpecTent(int32 Width, int32 Height, int32 PeakX, int32 PeakY, uint16 Peak)
	{
		TArray<uint16> Values;
		Values.Init(32768, Width * Height);
		Values[PeakY * Width + PeakX] = Peak;
		return Values;
	}

	static bool CreateSpecWritePng(const FString& Path, const TArray<uint16>& Values, int32 Width, int32 Height)
	{
		IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TArray64<uint8> Png;
		return Images.CompressImage(Png, EImageFormat::PNG, FImageView(const_cast<uint16*>(Values.GetData()), Width, Height, ERawImageFormat::G16))
			&& FFileHelper::SaveArrayToFile(Png, *Path);
	}

	static bool CreateSpecWriteR16(const FString& Path, const TArray<uint16>& Values)
	{
		TArray<uint8> Bytes;
		for (const uint16 Value : Values)
		{
			Bytes.Add(static_cast<uint8>(Value & 0xff));
			Bytes.Add(static_cast<uint8>(Value >> 8));
		}
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	/** Every actor of the editor world that carries a modifier (base mesh included) of @p Terrain. */
	static int32 CreateSpecTerrainActors(const AMeshPartition* Terrain)
	{
		TSet<AActor*> Actors;
		for (UnrealAIMeshTerrain::UModifierComponent* Modifier : UnrealAIMeshTerrain::Modifiers(Terrain, /*bBases*/ true))
			Actors.Add(Modifier->GetOwner());
		return Actors.Num();
	}
END_DEFINE_SPEC(FUnrealAIMeshTerrainCreateSpec)

void FUnrealAIMeshTerrainCreateSpec::Define()
{
	Describe("RQL", [this]()
	{
		It("adds meshTerrainCreate with Create, ImportHeightmap and Delete to the schema and the skill", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("MeshTerrainCreateOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Method : {
				TEXT("Create(string Label, Vector Location, double SizeMeters, double SizeYMeters, int Resolution, MeshPartitionDefinition Definition, double TimeoutSeconds)"),
				TEXT("ImportHeightmap(string Label, string File, Vector Location, double SizeMeters, double HeightScale, int Resolution, MeshPartitionDefinition Definition, double TimeoutSeconds)"),
				TEXT("Delete(MeshPartition Terrain)") })
			{
				TestTrue(*FString::Printf(TEXT("schema lists %s (%s)"), Method, *Schema.Left(400)), Schema.Contains(Method));
			}
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			for (const TCHAR* Text : { TEXT("meshTerrainCreate | call Create("), TEXT("call ImportHeightmap("), TEXT("call Delete(") })
				TestTrue(*FString::Printf(TEXT("skill mentions %s"), Text), Skill.Contains(Text));
		});
	});

	Describe("Create", [this]()
	{
		It("makes a flat terrain of the asked size and resolution, centred on Location and built when it returns", [this]()
		{
			const FString Label = CreateSpecLabel(TEXT("SpecCreate"));
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Label)); };
			// meshTerrainCreate | call Create("<label>", [600000, 20000, 350], 100, 60, 50)
			TSharedPtr<FJsonObject> Result;
			TestTrue(*FString::Printf(TEXT("Create (%s)"), *CreateSpecJson(Result)), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("Create"),
				CreateSpecArgs(Label, FVector(600000.0, 20000.0, 350.0), TEXT(R"({"lit":100},{"lit":60},{"lit":50})"))), true, &Result));
			AMeshPartition* Terrain = CreateSpecTerrain(Label);
			if (!TestNotNull(TEXT("terrain labelled"), Terrain))
				return;
			TestTrue(TEXT("returns the terrain"), CreateSpecJson(Result).Contains(Label));
			TestTrue(*FString::Printf(TEXT("built when Create returns (%s)"), *UnrealAIMeshTerrain::BuildState(Terrain).Describe()), UnrealAIMeshTerrain::BuildState(Terrain).IsReady());
			TestEqual(TEXT("one base mesh (one section)"), UnrealAIMeshTerrain::Modifiers(Terrain, /*bBases*/ true).Num(), 1);
			TestNotNull(TEXT("the default definition"), Terrain->GetMeshPartitionDefinition());
			const FBox Bounds = UnrealAIMeshTerrain::BuiltBounds(Terrain);
			TestTrue(*FString::Printf(TEXT("100 x 60 m centred on Location (%s)"), *Bounds.ToString()), Bounds.IsValid
				&& FMath::IsNearlyEqual(Bounds.Min.X, 595000.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.X, 605000.0, 1.0)
				&& FMath::IsNearlyEqual(Bounds.Min.Y, 17000.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.Y, 23000.0, 1.0)
				&& FMath::IsNearlyEqual(Bounds.Min.Z, 350.0, 0.5) && FMath::IsNearlyEqual(Bounds.Max.Z, 350.0, 0.5));
			const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, 601234.0, 21000.0);
			TestTrue(TEXT("the ground is at Location Z"), Sample.bInside && FMath::IsNearlyEqual(Sample.Height, 350.0, 0.5));
		});

		It("splits a larger terrain into sections of at most 500 x 500 quads", [this]()
		{
			const FString Label = CreateSpecLabel(TEXT("SpecSections"));
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Label)); };
			// meshTerrainCreate | call Create("<label>", [700000, 0, 0], 720, 360, 720): 720 x 360 quads
			TSharedPtr<FJsonObject> Result;
			TestTrue(*FString::Printf(TEXT("Create (%s)"), *CreateSpecJson(Result)), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("Create"),
				CreateSpecArgs(Label, FVector(700000.0, 0.0, 0.0), TEXT(R"({"lit":720},{"lit":360},{"lit":720})"))), true, &Result));
			AMeshPartition* Terrain = CreateSpecTerrain(Label);
			if (!TestNotNull(TEXT("terrain"), Terrain))
				return;
			const int32 Bases = UnrealAIMeshTerrain::Modifiers(Terrain, /*bBases*/ true).Num();
			TestTrue(*FString::Printf(TEXT("more than one section (%d)"), Bases), Bases > 1 && Bases <= 4);
			TestTrue(TEXT("built"), UnrealAIMeshTerrain::BuildState(Terrain).IsReady());
			// The editor may build neighbouring base meshes as one preview section: the built ground covers the whole terrain.
			const FBox Bounds = UnrealAIMeshTerrain::BuiltBounds(Terrain);
			TestTrue(*FString::Printf(TEXT("the built ground spans 720 x 360 m (%s)"), *Bounds.ToString()), Bounds.IsValid
				&& FMath::IsNearlyEqual(Bounds.Min.X, 664000.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.X, 736000.0, 1.0)
				&& FMath::IsNearlyEqual(Bounds.Min.Y, -18000.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.Y, 18000.0, 1.0));
		});

		It("uses the given definition", [this]()
		{
			const FString Label = CreateSpecLabel(TEXT("SpecDefinition"));
			const UE::MeshPartition::UMeshPartitionDefinition* Default = UE::MeshPartition::UMeshPartitionDefinition::GetDefaultMegaMeshDefinition();
			if (!TestNotNull(TEXT("default definition"), Default))
				return;
			// A copy (transient) stands for a project's own definition asset; the operation takes it by reference.
			UE::MeshPartition::UMeshPartitionDefinition* Definition = DuplicateObject(Default, GetTransientPackage());
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Label)); };
			// meshTerrainCreate | call Create("<label>", [800000, 0, 0], 50, 0, 20, <definition>)
			TSharedPtr<FJsonObject> Result;
			TestTrue(*FString::Printf(TEXT("Create (%s)"), *CreateSpecJson(Result)), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("Create"),
				CreateSpecArgs(Label, FVector(800000.0, 0.0, 0.0), FString::Printf(TEXT(R"({"lit":50},{"lit":0},{"lit":20},{"lit":"%s"})"), *Definition->GetPathName()))), true, &Result));
			const AMeshPartition* Terrain = CreateSpecTerrain(Label);
			TestTrue(TEXT("the terrain has the definition"), Terrain && Terrain->GetMeshPartitionDefinition() == Definition);
			TestTrue(TEXT("built"), Terrain && UnrealAIMeshTerrain::BuildState(Terrain).IsReady());
		});

		It("is one undo step: Undo removes the terrain and its base meshes, Redo brings them back and they build", [this]()
		{
			const FString Label = CreateSpecLabel(TEXT("SpecCreateUndo"));
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Label)); };
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("Create"), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("Create"),
				CreateSpecArgs(Label, FVector(900000.0, 0.0, 100.0), TEXT(R"({"lit":40},{"lit":0},{"lit":20})"))), true, &Result));
			AMeshPartition* Created = CreateSpecTerrain(Label);
			if (!TestNotNull(TEXT("created"), Created) || !TestNotNull(TEXT("editor"), GEditor))
				return;
			const TArray<UnrealAIMeshTerrain::UModifierComponent*> Bases = UnrealAIMeshTerrain::Modifiers(Created, /*bBases*/ true);
			TestEqual(TEXT("one base mesh"), Bases.Num(), 1);
			AActor* BaseActor = Bases.Num() == 1 ? Bases[0]->GetOwner() : nullptr;

			TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
			TestNull(TEXT("no terrain after Undo"), CreateSpecTerrain(Label));
			TestTrue(TEXT("its base mesh actor is gone"), !IsValid(BaseActor));

			TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
			AMeshPartition* Redone = CreateSpecTerrain(Label);
			if (!TestNotNull(TEXT("the terrain is back after Redo"), Redone))
				return;
			// The redone components join the terrain's modifier list when it next updates (WaitForBuild drives that).
			TestTrue(TEXT("it builds again"), UnrealAIMeshTerrain::WaitForBuild(Redone, 60.0));
			TestEqual(TEXT("with its base mesh"), UnrealAIMeshTerrain::Modifiers(Redone, /*bBases*/ true).Num(), 1);
			const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Redone, 900500.0, 300.0);
			TestTrue(TEXT("same ground"), Sample.bInside && FMath::IsNearlyEqual(Sample.Height, 100.0, 0.5));
		});

		It("refuses bad inputs with a clear error and creates nothing", [this]()
		{
			const FString Existing = CreateSpecLabel(TEXT("SpecCreateGuard"));
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Existing)); };
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("first terrain"), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("Create"),
				CreateSpecArgs(Existing, FVector(1000000.0, 0.0, 0.0), TEXT(R"({"lit":20},{"lit":0},{"lit":10})"))), true, &Result));
			const int32 Before = UnrealAIMeshTerrain::Terrains().Num();
			const FVector At(1000000.0, 50000.0, 0.0);
			const TPair<FString, FString> Cases[] = {
				{ CreateSpecArgs(TEXT(""), At, TEXT("")), TEXT("Label is empty") },
				{ CreateSpecArgs(Existing, At, TEXT("")), TEXT("already exists") },
				{ CreateSpecArgs(TEXT("SpecBad"), At, TEXT(R"({"lit":5})")), TEXT("SizeMeters 5 is outside 10 .. 20000") },
				{ CreateSpecArgs(TEXT("SpecBad"), At, TEXT(R"({"lit":30000})")), TEXT("SizeMeters 30000 is outside") },
				{ CreateSpecArgs(TEXT("SpecBad"), At, TEXT(R"({"lit":20000.01})")), TEXT("SizeMeters 20000.01 is outside") },
				{ CreateSpecArgs(TEXT("SpecBad"), At, TEXT(R"({"lit":100},{"lit":9.999})")), TEXT("SizeYMeters 9.999 is outside") },
				{ CreateSpecArgs(TEXT("SpecBad"), At, TEXT(R"({"lit":100},{"lit":50000})")), TEXT("SizeYMeters 50000 is outside") },
				{ CreateSpecArgs(TEXT("SpecBad"), At, TEXT(R"({"lit":100},{"lit":0},{"lit":5000})")), TEXT("Resolution 5000 is outside 1 .. 2000") },
				{ CreateSpecArgs(TEXT("SpecBad"), At, TEXT(R"({"lit":100},{"lit":1000},{"lit":300})")), TEXT("at most 2000 per side") },
				{ CreateSpecArgs(TEXT("SpecBad"), FVector(1.0e9, 0.0, 0.0), TEXT("")), TEXT("is not within") },
				{ CreateSpecArgs(TEXT("SpecBad"), At, TEXT(R"({"lit":100},{"lit":0},{"lit":0},{"lit":null},{"lit":0.5})")), TEXT("TimeoutSeconds 0.5 is outside 1 .. 600") },
			};
			for (const TPair<FString, FString>& Case : Cases)
			{
				TestFalse(*FString::Printf(TEXT("refused: %s"), *Case.Value), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("Create"), Case.Key), true, &Result));
				TestTrue(*FString::Printf(TEXT("says '%s' (%s)"), *Case.Value, *CreateSpecJson(Result)), CreateSpecJson(Result).Contains(Case.Value));
			}
			TestEqual(TEXT("no terrain was created"), UnrealAIMeshTerrain::Terrains().Num(), Before);
		});
	});

	Describe("ImportHeightmap", [this]()
	{
		It("maps 16-bit values in Landscape units (32768 = Location Z, HeightScale / 128 cm per step), one quad per pixel, centred on Location; PNG and R16 alike", [this]()
		{
			ON_SCOPE_EXIT { CreateSpecCleanFiles(); };
			// A 5 x 5 heightmap, 32768 everywhere but 32768 + 12800 at the centre: 12800 / 128 * 50 = 5000 cm with HeightScale 50.
			const TArray<uint16> Tent = CreateSpecTent(5, 5, 2, 2, 32768 + 12800);
			const FString Png = CreateSpecFile(TEXT("Tent5.png"));
			const FString R16 = CreateSpecFile(TEXT("Tent5.r16"));
			if (!TestTrue(TEXT("write the heightmaps"), CreateSpecWritePng(Png, Tent, 5, 5) && CreateSpecWriteR16(R16, Tent)))
				return;
			int32 Index = 0;
			for (const FString& File : { Png, R16 })
			{
				const FString Label = CreateSpecLabel(TEXT("SpecImport"));
				ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Label)); };
				const FVector Location(1100000.0, 10000.0 * Index++, 700.0);
				// meshTerrainCreate | call ImportHeightmap("<label>", "<file>", [X, Y, 700], 40, 50)
				TSharedPtr<FJsonObject> Result;
				const FString Args = FString::Printf(TEXT(R"({"lit":"%s"},{"lit":"%s"},{"list":[{"lit":%.17g},{"lit":%.17g},{"lit":%.17g}]},{"lit":40},{"lit":50})"),
					*Label, *File.Replace(TEXT("\\"), TEXT("/")), Location.X, Location.Y, Location.Z);
				TestTrue(*FString::Printf(TEXT("%s imported (%s)"), *File, *CreateSpecJson(Result)), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("ImportHeightmap"), Args), true, &Result));
				const AMeshPartition* Terrain = CreateSpecTerrain(Label);
				if (!TestNotNull(TEXT("terrain"), Terrain))
					continue;
				TestTrue(TEXT("built when it returns"), UnrealAIMeshTerrain::BuildState(Terrain).IsReady());
				TArray<FString> Left;
				IFileManager::Get().FindFiles(Left, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealAIMeshTerrain"), TEXT("*.png")), /*Files*/ true, /*Directories*/ false);
				TestEqual(TEXT("no importer file is left behind"), Left.Num(), 0);
				const FBox Bounds = UnrealAIMeshTerrain::BuiltBounds(Terrain);
				TestTrue(*FString::Printf(TEXT("40 x 40 m centred on Location (%s)"), *Bounds.ToString()), Bounds.IsValid
					&& FMath::IsNearlyEqual(Bounds.Min.X, Location.X - 2000.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.X, Location.X + 2000.0, 1.0)
					&& FMath::IsNearlyEqual(Bounds.Min.Y, Location.Y - 2000.0, 1.0) && FMath::IsNearlyEqual(Bounds.Max.Y, Location.Y + 2000.0, 1.0));
				const double Peak = CreateSpecModelHeight(Terrain, Location.X, Location.Y);
				const double Flat = CreateSpecModelHeight(Terrain, Location.X - 1000.0, Location.Y + 1000.0);
				TestTrue(*FString::Printf(TEXT("the peak pixel is 5000 cm above Location Z (%.2f)"), Peak), FMath::IsNearlyEqual(Peak, 5700.0, 1.0));
				TestTrue(*FString::Printf(TEXT("value 32768 is Location Z (%.2f)"), Flat), FMath::IsNearlyEqual(Flat, 700.0, 1.0));
			}
		});

		It("puts image column 0 on the -X edge and row 0 on the -Y edge (as Landscape)", [this]()
		{
			ON_SCOPE_EXIT { CreateSpecCleanFiles(); };
			// A 5 x 5 heightmap with one raised pixel at column 1, row 1: 10 m quads on a 40 m terrain (edges are avoided: the sample there is not exact).
			const FString Png = CreateSpecFile(TEXT("Corner5.png"));
			const FString Label = CreateSpecLabel(TEXT("SpecImportAxes"));
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Label)); };
			if (!TestTrue(TEXT("write"), CreateSpecWritePng(Png, CreateSpecTent(5, 5, 1, 1, 32768 + 1280), 5, 5)))
				return;
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("import"), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("ImportHeightmap"), FString::Printf(
				TEXT(R"({"lit":"%s"},{"lit":"%s"},{"list":[{"lit":1250000},{"lit":0},{"lit":0}]},{"lit":40})"), *Label, *Png.Replace(TEXT("\\"), TEXT("/")))), true, &Result));
			const AMeshPartition* Terrain = CreateSpecTerrain(Label);
			if (!TestNotNull(TEXT("imported"), Terrain))
				return;
			// 1280 / 128 * 100 = 1000 cm at (centre X - 1000, centre Y - 1000); the mirrored spots stay flat.
			const double Raised = CreateSpecModelHeight(Terrain, 1250000.0 - 1000.0, -1000.0);
			const double MirrorY = CreateSpecModelHeight(Terrain, 1250000.0 - 1000.0, 1000.0);
			const double MirrorX = CreateSpecModelHeight(Terrain, 1250000.0 + 1000.0, -1000.0);
			TestTrue(*FString::Printf(TEXT("raised at -X / -Y (%.2f), flat at +Y (%.2f) and +X (%.2f)"), Raised, MirrorY, MirrorX),
				FMath::IsNearlyEqual(Raised, 1000.0, 1.0) && FMath::IsNearlyEqual(MirrorY, 0.0, 1.0) && FMath::IsNearlyEqual(MirrorX, 0.0, 1.0));
		});

		It("is one undo step", [this]()
		{
			ON_SCOPE_EXIT { CreateSpecCleanFiles(); };
			const TArray<uint16> Tent = CreateSpecTent(9, 9, 4, 4, 40000);
			const FString Png = CreateSpecFile(TEXT("Tent9.png"));
			const FString Label = CreateSpecLabel(TEXT("SpecImportUndo"));
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Label)); };
			if (!TestTrue(TEXT("write"), CreateSpecWritePng(Png, Tent, 9, 9)) || !TestNotNull(TEXT("editor"), GEditor))
				return;
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("import"), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("ImportHeightmap"), FString::Printf(
				TEXT(R"({"lit":"%s"},{"lit":"%s"},{"list":[{"lit":1200000},{"lit":0},{"lit":0}]},{"lit":80})"), *Label, *Png.Replace(TEXT("\\"), TEXT("/")))), true, &Result));
			TestNotNull(TEXT("imported"), CreateSpecTerrain(Label));
			TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
			TestNull(TEXT("gone after Undo"), CreateSpecTerrain(Label));
			TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
			AMeshPartition* Redone = CreateSpecTerrain(Label);
			TestTrue(TEXT("back after Redo and it builds"), Redone && UnrealAIMeshTerrain::WaitForBuild(Redone, 60.0));
		});

		It("refuses missing, unsupported and malformed files and bad parameters", [this]()
		{
			ON_SCOPE_EXIT { CreateSpecCleanFiles(); };
			const FString Bad = CreateSpecFile(TEXT("Bad.r16"));
			const FString Text = CreateSpecFile(TEXT("NotAPng.png"));
			const FString Good = CreateSpecFile(TEXT("Good.png"));
			if (!TestTrue(TEXT("write"), FFileHelper::SaveStringToFile(TEXT("odd"), *Bad) && FFileHelper::SaveStringToFile(TEXT("not a png"), *Text)
				&& CreateSpecWritePng(Good, CreateSpecTent(3, 3, 1, 1, 33000), 3, 3)))
				return;
			const int32 Before = UnrealAIMeshTerrain::Terrains().Num();
			const auto Args = [](const FString& File, const FString& Rest)
			{
				return FString::Printf(TEXT(R"({"lit":"SpecBadImport"},{"lit":"%s"},{"list":[{"lit":1300000},{"lit":0},{"lit":0}]}%s)"), *File.Replace(TEXT("\\"), TEXT("/")), *Rest);
			};
			const TPair<FString, FString> Cases[] = {
				{ Args(CreateSpecFile(TEXT("Missing.png")), TEXT("")), TEXT("heightmap file not found") },
				{ Args(CreateSpecFile(TEXT("Heightmap.jpg")), TEXT("")), TEXT("must be a .png or a .r16 / .raw file") },
				{ Args(Bad, TEXT("")), TEXT("a .r16 / .raw heightmap is a square") },
				{ Args(Text, TEXT("")), TEXT("is not a readable PNG image") },
				{ Args(Good, TEXT(R"(,{"lit":100},{"lit":0})")), TEXT("HeightScale 0 is outside") },
				{ Args(Good, TEXT(R"(,{"lit":100},{"lit":100},{"lit":4000})")), TEXT("Resolution 4000 is outside 1 .. 2048") },
				{ Args(Good, TEXT(R"(,{"lit":5})")), TEXT("SizeMeters 5 is outside") },
			};
			TSharedPtr<FJsonObject> Result;
			for (const TPair<FString, FString>& Case : Cases)
			{
				TestFalse(*FString::Printf(TEXT("refused: %s"), *Case.Value), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("ImportHeightmap"), Case.Key), true, &Result));
				TestTrue(*FString::Printf(TEXT("says '%s' (%s)"), *Case.Value, *CreateSpecJson(Result)), CreateSpecJson(Result).Contains(Case.Value));
			}
			TestEqual(TEXT("no terrain was created"), UnrealAIMeshTerrain::Terrains().Num(), Before);
		});
	});

	Describe("Delete", [this]()
	{
		It("deletes the terrain with its base meshes, its modifiers and its sections as one undo step", [this]()
		{
			const FString Label = CreateSpecLabel(TEXT("SpecDelete"));
			ON_SCOPE_EXIT { UnrealAIMeshTerrain::DestroyTerrain(CreateSpecTerrain(Label)); };
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("Create"), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("Create"),
				CreateSpecArgs(Label, FVector(1400000.0, 0.0, 0.0), TEXT(R"({"lit":60},{"lit":0},{"lit":30})"))), true, &Result));
			AMeshPartition* Terrain = CreateSpecTerrain(Label);
			if (!TestNotNull(TEXT("terrain"), Terrain) || !TestNotNull(TEXT("editor"), GEditor))
				return;
			// A noise modifier placed like the Modifiers palette does, bound to the terrain.
			UActorFactory* Factory = GEditor->FindActorFactoryByClass(UE::MeshPartition::UModifierActorFactory::StaticClass());
			AActor* NoiseActor = Factory ? Factory->CreateActor(UE::MeshPartition::UNoiseModifier::StaticClass(), Terrain->GetWorld()->GetCurrentLevel(), FTransform(Terrain->GetActorLocation())) : nullptr;
			UE::MeshPartition::UNoiseModifier* Noise = NoiseActor ? NoiseActor->FindComponentByClass<UE::MeshPartition::UNoiseModifier>() : nullptr;
			if (!TestNotNull(TEXT("noise placed"), Noise))
				return;
			Noise->SetAffectedMeshPartition(Terrain);
			TestTrue(TEXT("built with the noise"), UnrealAIMeshTerrain::WaitForBuild(Terrain, 60.0));
			TestEqual(TEXT("two actors carry its base mesh and modifier"), CreateSpecTerrainActors(Terrain), 2);
			const TArray<UnrealAIMeshTerrain::APreviewSection*> Sections = UnrealAIMeshTerrain::PreviewSections(Terrain);

			// meshTerrainCreate | call Delete(unreal.find("<label>"))
			TestTrue(*FString::Printf(TEXT("Delete (%s)"), *CreateSpecJson(Result)), UnrealMcpRqlTools::RunPlan(CreateSpecDelete(Label), true, &Result));
			const FString Json = CreateSpecJson(Result);
			TestTrue(*FString::Printf(TEXT("says what it deleted (%s)"), *Json), Json.Contains(TEXT("1 base mesh, 1 modifier and 1 built section")));
			TestNull(TEXT("no terrain"), CreateSpecTerrain(Label));
			TestTrue(TEXT("its modifier actor is gone"), !IsValid(NoiseActor));
			TestTrue(TEXT("its sections are gone"), !Sections.ContainsByPredicate([](const UnrealAIMeshTerrain::APreviewSection* Section) { return IsValid(Section); }));

			TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
			AMeshPartition* Restored = CreateSpecTerrain(Label);
			if (!TestNotNull(TEXT("Undo restores the terrain"), Restored))
				return;
			// The restored components join the terrain's modifier list when it next updates (WaitForBuild drives that).
			TestTrue(TEXT("it builds"), UnrealAIMeshTerrain::WaitForBuild(Restored, 60.0));
			TestEqual(TEXT("with its base mesh and modifier"), CreateSpecTerrainActors(Restored), 2);
			TestTrue(TEXT("Redo deletes it again"), GEditor->RedoTransaction() && !CreateSpecTerrain(Label));
		});

		It("refuses a missing or null terrain before it runs", [this]()
		{
			TSharedPtr<FJsonObject> Result;
			TestFalse(TEXT("missing terrain"), UnrealMcpRqlTools::RunPlan(CreateSpecDelete(CreateSpecLabel(TEXT("NoSuchTerrain"))), true, &Result));
			TestTrue(*FString::Printf(TEXT("names the argument (%s)"), *CreateSpecJson(Result)), CreateSpecJson(Result).Contains(TEXT("argument 'Terrain'")));
			TestFalse(TEXT("null terrain"), UnrealMcpRqlTools::RunPlan(CreateSpecCall(TEXT("Delete"), TEXT(R"({"lit":null})")), true, &Result));
			TestTrue(TEXT("null refused"), CreateSpecJson(Result).Contains(TEXT("null is not allowed for MeshPartition")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
