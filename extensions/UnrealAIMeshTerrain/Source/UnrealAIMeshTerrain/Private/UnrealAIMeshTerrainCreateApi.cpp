// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainCreateApi.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "IUnrealMcpRqlModule.h"

#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"

#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionHeightmapImporter.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionRectangleGenerator.h"

namespace UnrealAIMeshTerrainCreateApiLocal
{
	using namespace UnrealAIMeshTerrain;

	// Hard caps: a terrain an agent creates builds in seconds to about a minute, never freezes the editor.
	constexpr double MinSizeMeters = 10.0;
	constexpr double MaxSizeMeters = 20000.0;
	constexpr int32 MaxQuadsPerSide = 2000;           // Create: 4 x 4 sections of 500 x 500 quads
	constexpr int32 MaxHeightmapQuads = 2048;         // ImportHeightmap: 4 x 4 sections of 512 x 512 quads
	constexpr int32 MaxSections = 16;
	constexpr int32 MaxHeightmapPixels = 8193;        // the file itself (resampled to at most MaxHeightmapQuads)
	constexpr double MaxLocation = 2.0e7;             // 200 km from the origin
	constexpr double LandscapeZScale = 1.0 / 128.0;   // LANDSCAPE_ZSCALE: one 16-bit step is ZScale / 128 cm
	constexpr int32 LandscapeMidValue = 32768;        // the 16-bit value of height 0

	bool CheckLabel(const FString& Label)
	{
		if (Label.TrimStartAndEnd().IsEmpty())
			return UnrealMcpRql::Failed<bool>(TEXT("Label is empty: name the terrain (agents find it with unreal.find(\"<label>\"))"));
		for (const AMeshPartition* Terrain : Terrains())
		{
			if (Terrain->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("a Mesh Terrain labelled '%s' already exists: pick another label or Delete it first"), *Label));
		}
		return true;
	}

	bool CheckCommon(const FString& Label, const FVector& Location, double SizeMeters, double TimeoutSeconds)
	{
		if (!(TimeoutSeconds >= 1.0 && TimeoutSeconds <= 600.0))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("TimeoutSeconds %g is outside 1 .. 600"), TimeoutSeconds));
		if (!(SizeMeters >= MinSizeMeters && SizeMeters <= MaxSizeMeters))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("SizeMeters %g is outside %g .. %g"), SizeMeters, MinSizeMeters, MaxSizeMeters));
		if (Location.ContainsNaN() || Location.GetAbsMax() > MaxLocation)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("Location %s is not within %g cm of the origin"), *Location.ToString(), MaxLocation));
		return CheckAvailable() && CheckLabel(Label);
	}

	/** The section grid the Create / Import tools' automatic mode makes for @p Quads, failed when it is over MaxSections. */
	bool Sections(const FInt32Point& Quads, int32 MaxTrianglesPerSection, FInt32Point& OutSections)
	{
		// ComputeSectionResolution returns the NUMBER of sections on X and Y (not quads per section, as its comment says).
		OutSections = UE::MeshPartition::FRectangleGeneratorUtils::ComputeSectionResolution(Quads, MaxTrianglesPerSection);
		if (OutSections.X <= 0 || OutSections.Y <= 0 || OutSections.X * OutSections.Y > MaxSections)
		{
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%d x %d quads would make %d x %d sections (at most %d): lower Resolution"),
				Quads.X, Quads.Y, OutSections.X, OutSections.Y, MaxSections));
		}
		return true;
	}

	/**
	 * Label the new terrain, give it @p Definition (when set) and record both in the open transaction; then build it. The
	 * generators spawn the terrain and its base-mesh actors inside that transaction, so one undo removes them all.
	 */
	void Finish(AMeshPartition* Terrain, const FString& Label, UMeshPartitionDefinition* Definition)
	{
		Terrain->Modify();
		Terrain->SetActorLabel(Label);
		if (Definition && Terrain->GetMeshPartitionDefinition() != Definition)
			Terrain->SetMeshPartitionDefinition(Definition);
	}

	/** WaitForBuild with the message the create operations need: the terrain exists even when the wait times out. */
	AMeshPartition* Built(AMeshPartition* Terrain, double TimeoutSeconds)
	{
		if (UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
			Component->UpdateModifierList();
		return WaitForBuild(Terrain, TimeoutSeconds) ? Terrain : nullptr;
	}

	/** A heightmap decoded to 16-bit gray. */
	struct FHeightmap
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray64<uint16> Values;
	};

	FString ResolvePath(const FString& File)
	{
		FString Path = File.TrimStartAndEnd().TrimQuotes();
		if (FPaths::IsRelative(Path))
			Path = FPaths::Combine(FPaths::ProjectDir(), Path);
		return FPaths::ConvertRelativePathToFull(Path);
	}

	/** Read @p Path (.png of any bit depth / colour, or .r16 / .raw: little-endian 16-bit, square) as 16-bit gray. */
	bool LoadHeightmap(const FString& Path, FHeightmap& Out)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		const bool bRaw = Extension == TEXT("r16") || Extension == TEXT("raw");
		if (!bRaw && Extension != TEXT("png"))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s: the heightmap must be a .png or a .r16 / .raw file (16-bit)"), *Path));
		const int64 FileSize = IFileManager::Get().FileSize(*Path);
		if (FileSize < 0)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("heightmap file not found: %s"), *Path));
		if (FileSize > 256ll * 1024 * 1024)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is %lld bytes: at most 256 MB"), *Path, FileSize));
		TArray64<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("could not read %s"), *Path));
		if (bRaw)
		{
			const int64 Count = Bytes.Num() / 2;
			const int32 Side = static_cast<int32>(FMath::RoundToInt(FMath::Sqrt(static_cast<double>(Count))));
			if (Bytes.Num() % 2 != 0 || static_cast<int64>(Side) * Side != Count)
			{
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is %lld bytes: a .r16 / .raw heightmap is a square of 16-bit little-endian values (side x side x 2 bytes)"),
					*Path, static_cast<long long>(Bytes.Num())));
			}
			Out.Width = Out.Height = Side;
			Out.Values.SetNumUninitialized(Count);
			for (int64 Index = 0; Index < Count; ++Index)
				Out.Values[Index] = static_cast<uint16>(Bytes[Index * 2] | (Bytes[Index * 2 + 1] << 8));
		}
		else
		{
			IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			FImage Image;
			if (Images.DetectImageFormat(Bytes.GetData(), Bytes.Num()) != EImageFormat::PNG || !Images.DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not a readable PNG image"), *Path));
			// Heights are linear: an 8-bit PNG's sRGB flag must not bend them.
			if (ERawImageFormat::GetFormatNeedsGammaSpace(Image.Format))
				Image.GammaSpace = EGammaSpace::Linear;
			Image.ChangeFormat(ERawImageFormat::G16, EGammaSpace::Linear);
			Out.Width = Image.SizeX;
			Out.Height = Image.SizeY;
			Out.Values = TArray64<uint16>(Image.AsG16().GetData(), Image.AsG16().Num());
		}
		if (Out.Width < 2 || Out.Height < 2 || Out.Width > MaxHeightmapPixels || Out.Height > MaxHeightmapPixels)
		{
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is %d x %d pixels: a heightmap is 2 .. %d pixels per side"),
				*Path, Out.Width, Out.Height, MaxHeightmapPixels));
		}
		return true;
	}

	/** Write @p Heightmap as the 16-bit gray PNG Mesh Partition's importer reads (it reads only .png). */
	bool WriteImporterPng(const FHeightmap& Heightmap, const FString& Path)
	{
		IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const FImageView View(const_cast<uint16*>(Heightmap.Values.GetData()), Heightmap.Width, Heightmap.Height, ERawImageFormat::G16);
		TArray64<uint8> Png;
		if (!Images.CompressImage(Png, EImageFormat::PNG, View) || !FFileHelper::SaveArrayToFile(Png, *Path))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("could not write the heightmap for the importer to %s"), *Path));
		return true;
	}

	FString Count(int32 Number, const TCHAR* Singular, const TCHAR* Plural)
	{
		return FString::Printf(TEXT("%d %s"), Number, Number == 1 ? Singular : Plural);
	}
}

UE::MeshPartition::AMeshPartition* UUnrealAIMeshTerrainCreateApi::Create(const FString& Label, FVector Location, double SizeMeters, double SizeYMeters,
	int32 Resolution, UE::MeshPartition::UMeshPartitionDefinition* Definition, double TimeoutSeconds)
{
	using namespace UnrealAIMeshTerrainCreateApiLocal;
	if (!CheckCommon(Label, Location, SizeMeters, TimeoutSeconds))
		return nullptr;
	if (SizeYMeters == 0.0)
		SizeYMeters = SizeMeters;
	if (!(SizeYMeters >= MinSizeMeters && SizeYMeters <= MaxSizeMeters))
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("SizeYMeters %g is outside %g .. %g (0 = square)"), SizeYMeters, MinSizeMeters, MaxSizeMeters));
	if (Resolution < 0 || Resolution > MaxQuadsPerSide)
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("Resolution %d is outside 1 .. %d quads (0 = about one per metre)"), Resolution, MaxQuadsPerSide));

	// Square quads: Resolution along X, Y in proportion (0: about one quad per metre, at most 1000 per side).
	const double LongerMeters = FMath::Max(SizeMeters, SizeYMeters);
	const int32 QuadsX = Resolution > 0 ? Resolution
		: FMath::Max(1, FMath::RoundToInt(FMath::Clamp(FMath::RoundToDouble(LongerMeters), 10.0, 1000.0) * SizeMeters / LongerMeters));
	const int32 QuadsY = FMath::Max(1, FMath::RoundToInt(QuadsX * SizeYMeters / SizeMeters));
	if (QuadsX < 1 || QuadsY > MaxQuadsPerSide)
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("%d x %d quads: at most %d per side, lower Resolution"), QuadsX, QuadsY, MaxQuadsPerSide));
	FInt32Point SectionCount;
	if (!Sections(FInt32Point(QuadsX, QuadsY), 500 * 500 * 2, SectionCount))
		return nullptr;

	UWorld* World = UnrealMcpRql::EditorWorld();
	UE::MeshPartition::FMegaMeshRectangleGeneratorParams Params;
	Params.MeshSize = FVector2D(SizeMeters * 100.0, SizeYMeters * 100.0);
	Params.MeshResolution = FInt32Point(QuadsX, QuadsY);
	Params.SectionsResolution = SectionCount;
	// Saving and unloading as it goes (World Partition) would save packages and may prompt source control: never.
	Params.bSaveAndUnload = false;
	Params.LocationVolumesResolution = FInt32Point(0, 0);
	AMeshPartition* Terrain = nullptr;
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UnrealAIMeshTerrain", "CreateTerrain", "Create Mesh Terrain"));
		UE::MeshPartition::FMegaMeshRectangleGenerator Generator(Params, *World, FTransform(Location));
		if (!Generator.Generate(nullptr) || !Generator.GetMegaMesh())
			return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("Mesh Terrain creation failed: %s"), *Generator.GetErrorText().ToString()));
		Terrain = Generator.GetMegaMesh();
		Finish(Terrain, Label, Definition);
	}
	return Built(Terrain, TimeoutSeconds);
}

UE::MeshPartition::AMeshPartition* UUnrealAIMeshTerrainCreateApi::ImportHeightmap(const FString& Label, const FString& File, FVector Location, double SizeMeters,
	double HeightScale, int32 Resolution, UE::MeshPartition::UMeshPartitionDefinition* Definition, double TimeoutSeconds)
{
	using namespace UnrealAIMeshTerrainCreateApiLocal;
	if (!(HeightScale >= 0.01 && HeightScale <= 10000.0))
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("HeightScale %g is outside 0.01 .. 10000 (a Landscape Z scale: 100 = -256 .. +256 m)"), HeightScale));
	if (Resolution < 0 || Resolution > MaxHeightmapQuads)
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("Resolution %d is outside 1 .. %d quads (0 = one per heightmap pixel)"), Resolution, MaxHeightmapQuads));
	if (!CheckCommon(Label, Location, SizeMeters, TimeoutSeconds))
		return nullptr;
	const FString Path = ResolvePath(File);
	FHeightmap Heightmap;
	if (!LoadHeightmap(Path, Heightmap))
		return nullptr;

	// Quads along X (one per pixel step by default, at most MaxHeightmapQuads); Y keeps the image's aspect (square quads).
	const double Aspect = double(Heightmap.Height - 1) / double(Heightmap.Width - 1);
	const int32 QuadsX = Resolution > 0 ? Resolution : FMath::Min(Heightmap.Width - 1, MaxHeightmapQuads);
	const int32 QuadsY = FMath::Max(1, FMath::RoundToInt(QuadsX * Aspect));
	if (QuadsY > MaxHeightmapQuads)
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("%d x %d quads: at most %d per side, lower Resolution"), QuadsX, QuadsY, MaxHeightmapQuads));
	const double SizeYMeters = SizeMeters * Aspect;
	if (SizeYMeters < 1.0 || SizeYMeters > MaxSizeMeters)
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("the heightmap's aspect makes the terrain %g m on Y: keep it within 1 .. %g"), SizeYMeters, MaxSizeMeters));

	UE::MeshPartition::FHeightmapImportParams Params;
	Params.World = UnrealMcpRql::EditorWorld();
	// QUADS on X and Y, although the importer's comment says vertices (verified: 4 quads over a 5-pixel image hit every pixel).
	Params.MeshResolution = FInt32Point(QuadsX, QuadsY);
	// The importer maps value v to Z = v / 65535 * MeshSize.Z: with Z = 65535 / 128 * HeightScale one step is HeightScale / 128 cm,
	// exactly a Landscape's (LANDSCAPE_ZSCALE); the terrain is then lowered so value 32768 lands on Location Z (see Import).
	Params.MeshSize = FVector(SizeMeters * 100.0, SizeYMeters * 100.0, 65535.0 * LandscapeZScale * HeightScale);
	Params.bSaveAndUnload = false;
	Params.LocationVolumesResolution = FInt32Point(0, 0);
	// The import tool's automatic sections: at most 512 x 512 quads each.
	if (!Sections(FInt32Point(QuadsX, QuadsY), 512 * 512 * 2, Params.SectionsResolution))
		return nullptr;

	// Mesh Partition's importer reads only 16-bit gray .png files: hand it the decoded heights as one.
	const FString ImporterFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealAIMeshTerrain"),
		FString::Printf(TEXT("Heightmap_%s.png"), *FGuid::NewGuid().ToString(EGuidFormats::Short))));
	if (!WriteImporterPng(Heightmap, ImporterFile))
		return nullptr;
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*ImporterFile, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true); };
	Params.HeightmapFilename = ImporterFile;

	AMeshPartition* Terrain = nullptr;
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UnrealAIMeshTerrain", "ImportHeightmap", "Import Mesh Terrain Heightmap"));
		UE::MeshPartition::FHeightmapImporter Importer(Params);
		if (!Importer.Import(nullptr) || !Importer.GetMegaMesh())
			return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("heightmap import failed: %s"), *Importer.GetErrorText().ToString()));
		Terrain = Importer.GetMegaMesh();
		Finish(Terrain, Label, Definition);
		// The importer centres the terrain on the origin with value 0 at Z 0: move it onto Location, value 32768 at its Z.
		Terrain->SetActorLocation(Location - FVector(0.0, 0.0, LandscapeMidValue * LandscapeZScale * HeightScale));
	}
	return Built(Terrain, TimeoutSeconds);
}

FString UUnrealAIMeshTerrainCreateApi::Delete(UE::MeshPartition::AMeshPartition* Terrain)
{
	using namespace UnrealAIMeshTerrainCreateApiLocal;
	if (!CheckAvailable() || !CheckTerrain(Terrain))
		return FString();
	if (UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
		Component->UpdateModifierList();
	int32 Bases = 0;
	int32 Others = 0;
	for (const UModifierComponent* Modifier : Modifiers(Terrain, /*bBases*/ true))
		++(Modifier->IsBase() ? Bases : Others);
	const int32 SectionCount = PreviewSections(Terrain).Num();
	const FString Name = Terrain->GetActorNameOrLabel();
	DestroyTerrain(Terrain);
	return FString::Printf(TEXT("deleted Mesh Terrain '%s' with %s, %s and %s (one undo step)"), *Name,
		*Count(Bases, TEXT("base mesh"), TEXT("base meshes")), *Count(Others, TEXT("modifier"), TEXT("modifiers")),
		*Count(SectionCount, TEXT("built section"), TEXT("built sections")));
}
