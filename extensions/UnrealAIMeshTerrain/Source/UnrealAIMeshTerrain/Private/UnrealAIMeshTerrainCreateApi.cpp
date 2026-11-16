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
		if (!CheckTimeout(TimeoutSeconds))
			return false;
		if (!(SizeMeters >= MinSizeMeters && SizeMeters <= MaxSizeMeters))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("SizeMeters %.10g is outside %g .. %g"), SizeMeters, MinSizeMeters, MaxSizeMeters));
		if (Location.ContainsNaN() || Location.GetAbsMax() > MaxLocation)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("Location %s is not within 200 km (%g cm) of the origin"), *Location.ToString(), MaxLocation));
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
	 * Record the new terrain in @p Transaction (opened before the generator spawned it and its base-mesh actors, so one undo
	 * removes them all), label it and give it @p Definition when set.
	 */
	void Finish(FTerrainTransaction& Transaction, AMeshPartition* Terrain, const FString& Label, UMeshPartitionDefinition* Definition)
	{
		Transaction.Modify(Terrain);
		Terrain->SetActorLabel(Label);
		if (Definition && Terrain->GetMeshPartitionDefinition() != Definition)
			Terrain->SetMeshPartitionDefinition(Definition);
	}

	FString ResolvePath(const FString& File)
	{
		FString Path = File.TrimStartAndEnd().TrimQuotes();
		if (FPaths::IsRelative(Path))
			Path = FPaths::Combine(FPaths::ProjectDir(), Path);
		return FPaths::ConvertRelativePathToFull(Path);
	}

	bool CheckPixels(const FString& Path, int64 Width, int64 Height)
	{
		if (Width < 2 || Height < 2 || Width > MaxHeightmapPixels || Height > MaxHeightmapPixels)
		{
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is %lld x %lld pixels: a heightmap is 2 .. %d pixels per side"),
				*Path, static_cast<long long>(Width), static_cast<long long>(Height), MaxHeightmapPixels));
		}
		return true;
	}

	/** Read @p Path (.png of any bit depth / colour, or .r16 / .raw: little-endian 16-bit, square) as a 16-bit gray image. */
	bool LoadHeightmap(const FString& Path, FImage& Out)
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
			if (!CheckPixels(Path, Side, Side))
				return false;
			static_assert(PLATFORM_LITTLE_ENDIAN, "a .r16 / .raw file is little-endian");
			Out.Init(Side, Side, ERawImageFormat::G16, EGammaSpace::Linear);
			FMemory::Memcpy(Out.RawData.GetData(), Bytes.GetData(), Bytes.Num());
		}
		else
		{
			IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			const TSharedPtr<IImageWrapper> Png = Images.DetectImageFormat(Bytes.GetData(), Bytes.Num()) == EImageFormat::PNG
				? Images.CreateImageWrapper(EImageFormat::PNG) : nullptr;
			if (!Png.IsValid() || !Png->SetCompressed(Bytes.GetData(), Bytes.Num()))
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not a readable PNG image"), *Path));
			// The header first: a small, well-compressed PNG can claim dimensions that would not fit in memory decoded.
			if (!CheckPixels(Path, Png->GetWidth(), Png->GetHeight()))
				return false;
			if (!Images.DecompressImage(Bytes.GetData(), Bytes.Num(), Out))
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not a readable PNG image"), *Path));
			// Heights are linear: an 8-bit PNG's sRGB flag must not bend them.
			if (ERawImageFormat::GetFormatNeedsGammaSpace(Out.Format))
				Out.GammaSpace = EGammaSpace::Linear;
			Out.ChangeFormat(ERawImageFormat::G16, EGammaSpace::Linear);
		}
		return CheckPixels(Path, Out.SizeX, Out.SizeY);
	}

	/**
	 * The image to hand Mesh Partition's importer so that vertex (i, j) of a @p Quads mesh gets exactly @p Heightmap's value at
	 * pixel position (i * (Width - 1) / Quads.X, j * (Height - 1) / Quads.Y) (bilinear between pixels; pixel i itself when
	 * Quads = Width - 1). The importer samples its image bilinearly at texel-centre coordinates, p = i / Quads * ImageWidth - 0.5,
	 * clamped at the borders (verified: a 5-pixel image on 4 quads gives vertex 1 a 0.75 / 0.25 blend of pixels 1 and 2). An
	 * image of 2 * Quads pixels per side whose pixels 2k - 1 and 2k both hold vertex k's value (pixel 0 vertex 0, the last
	 * pixel vertex Quads) puts every sample exactly between two equal pixels, so the mesh gets the values unblended.
	 */
	FImage ImporterImage(const FImage& Heightmap, const FInt32Point& Quads)
	{
		const TArrayView64<const uint16> Source = Heightmap.AsG16();
		const auto Sample = [&Heightmap, &Source](double X, double Y)
		{
			const int32 X0 = FMath::Min(FMath::FloorToInt(X), Heightmap.SizeX - 2);
			const int32 Y0 = FMath::Min(FMath::FloorToInt(Y), Heightmap.SizeY - 2);
			const double FX = X - X0;
			const double FY = Y - Y0;
			const auto At = [&Heightmap, &Source](int32 PX, int32 PY) { return double(Source[int64(PY) * Heightmap.SizeX + PX]); };
			const double Top = FMath::Lerp(At(X0, Y0), At(X0 + 1, Y0), FX);
			const double Bottom = FMath::Lerp(At(X0, Y0 + 1), At(X0 + 1, Y0 + 1), FX);
			return static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(FMath::Lerp(Top, Bottom, FY)), 0, 65535));
		};
		TArray<uint16> Vertices;
		Vertices.SetNumUninitialized((Quads.X + 1) * (Quads.Y + 1));
		for (int32 J = 0; J <= Quads.Y; ++J)
		{
			for (int32 I = 0; I <= Quads.X; ++I)
				Vertices[J * (Quads.X + 1) + I] = Sample(double(I) * (Heightmap.SizeX - 1) / Quads.X, double(J) * (Heightmap.SizeY - 1) / Quads.Y);
		}
		FImage Out(2 * Quads.X, 2 * Quads.Y, ERawImageFormat::G16, EGammaSpace::Linear);
		const TArrayView64<uint16> Pixels = Out.AsG16();
		for (int32 Y = 0; Y < Out.SizeY; ++Y)
		{
			for (int32 X = 0; X < Out.SizeX; ++X)
				Pixels[int64(Y) * Out.SizeX + X] = Vertices[((Y + 1) / 2) * (Quads.X + 1) + (X + 1) / 2];
		}
		return Out;
	}

	/** Write @p Heightmap as the 16-bit gray PNG Mesh Partition's importer reads (only .png; written once, read once: uncompressed). */
	bool WriteImporterPng(const FImage& Heightmap, const FString& Path)
	{
		IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TArray64<uint8> Png;
		if (!Images.CompressImage(Png, EImageFormat::PNG, Heightmap, static_cast<int32>(EImageCompressionQuality::Uncompressed))
			|| !FFileHelper::SaveArrayToFile(Png, *Path))
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
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("SizeYMeters %.10g is outside %g .. %g (0 = square)"), SizeYMeters, MinSizeMeters, MaxSizeMeters));
	if (Resolution < 0 || Resolution > MaxQuadsPerSide)
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("Resolution %d is outside 1 .. %d quads (0 = about one per metre, at most 1000)"), Resolution, MaxQuadsPerSide));

	// Square quads: Resolution along X, Y in proportion (0: about one quad per metre, at most 1000 per side).
	const double LongerMeters = FMath::Max(SizeMeters, SizeYMeters);
	const int32 QuadsX = Resolution > 0 ? Resolution
		: FMath::Max(1, FMath::RoundToInt(FMath::Clamp(FMath::RoundToDouble(LongerMeters), 10.0, 1000.0) * SizeMeters / LongerMeters));
	const int32 QuadsY = FMath::Max(1, FMath::RoundToInt(QuadsX * SizeYMeters / SizeMeters));
	if (QuadsY > MaxQuadsPerSide)
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("%d x %d quads: at most %d per side, lower Resolution"), QuadsX, QuadsY, MaxQuadsPerSide));
	// The automatic sections SpawnFlatTerrain makes (at most 500 x 500 quads each), checked against the cap first.
	FInt32Point SectionCount;
	if (!Sections(FInt32Point(QuadsX, QuadsY), 500 * 500 * 2, SectionCount))
		return nullptr;

	AMeshPartition* Terrain = nullptr;
	{
		FTerrainTransaction Transaction(NSLOCTEXT("UnrealAIMeshTerrain", "CreateTerrain", "Create Mesh Terrain"));
		Terrain = SpawnFlatTerrain(Label, Location, SizeMeters * 100.0, SizeYMeters * 100.0, QuadsX, QuadsY);
		if (!Terrain)
			return nullptr; // SpawnFlatTerrain failed with the reason
		Finish(Transaction, Terrain, Label, Definition);
	}
	// The terrain stays when the wait times out: WaitForBuild's error names it.
	return WaitForBuild(Terrain, TimeoutSeconds) ? Terrain : nullptr;
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
	FImage Heightmap;
	if (!LoadHeightmap(Path, Heightmap))
		return nullptr;

	// Quads along X (one per pixel step by default, at most MaxHeightmapQuads); Y keeps the image's aspect (square quads).
	const double Aspect = double(Heightmap.SizeY - 1) / double(Heightmap.SizeX - 1);
	const int32 QuadsX = Resolution > 0 ? Resolution
		: FMath::Max(1, FMath::Min3<int32>(Heightmap.SizeX - 1, MaxHeightmapQuads, FMath::FloorToInt(MaxHeightmapQuads / Aspect)));
	const int32 QuadsY = FMath::Max(1, FMath::RoundToInt(QuadsX * Aspect));
	if (QuadsY > MaxHeightmapQuads)
	{
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("%d x %d quads (the image is %d x %d pixels): at most %d per side, lower Resolution"),
			QuadsX, QuadsY, Heightmap.SizeX, Heightmap.SizeY, MaxHeightmapQuads));
	}
	const double SizeYMeters = SizeMeters * Aspect;
	if (SizeYMeters < 1.0 || SizeYMeters > MaxSizeMeters)
		return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("the heightmap's aspect makes the terrain %.10g m on Y: keep it within 1 .. %g"), SizeYMeters, MaxSizeMeters));

	UE::MeshPartition::FHeightmapImportParams Params;
	Params.World = UnrealMcpRql::EditorWorld();
	// QUADS on X and Y, although the importer's comment says vertices (verified on a 5-pixel image).
	Params.MeshResolution = FInt32Point(QuadsX, QuadsY);
	// The importer maps value v to Z = v / 65535 * MeshSize.Z: with Z = 65535 / 128 * HeightScale one step is HeightScale / 128 cm,
	// exactly a Landscape's (LANDSCAPE_ZSCALE); the terrain is then lowered so value 32768 lands on Location Z (see Import).
	Params.MeshSize = FVector(SizeMeters * 100.0, SizeYMeters * 100.0, 65535.0 * LandscapeZScale * HeightScale);
	Params.bSaveAndUnload = false;
	Params.LocationVolumesResolution = FInt32Point(0, 0);
	// The import tool's automatic sections: at most 512 x 512 quads each.
	if (!Sections(FInt32Point(QuadsX, QuadsY), 512 * 512 * 2, Params.SectionsResolution))
		return nullptr;

	// Mesh Partition's importer reads only 16-bit gray .png files: hand it the heights as one, laid out so its sampling
	// lands exactly on them (ImporterImage).
	const FString ImporterFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealAIMeshTerrain"),
		FString::Printf(TEXT("Heightmap_%s.png"), *FGuid::NewGuid().ToString(EGuidFormats::Short))));
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*ImporterFile, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true); };
	if (!WriteImporterPng(ImporterImage(Heightmap, Params.MeshResolution), ImporterFile))
		return nullptr;
	Params.HeightmapFilename = ImporterFile;

	AMeshPartition* Terrain = nullptr;
	{
		FTerrainTransaction Transaction(NSLOCTEXT("UnrealAIMeshTerrain", "ImportHeightmap", "Import Mesh Terrain Heightmap"));
		UE::MeshPartition::FHeightmapImporter Importer(Params);
		if (!Importer.Import(nullptr) || !Importer.GetMegaMesh())
		{
			DestroyTerrain(Importer.GetMegaMesh()); // whatever a failed import spawned (null-safe)
			return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("heightmap import failed: %s"), *Importer.GetErrorText().ToString()));
		}
		Terrain = Importer.GetMegaMesh();
		Finish(Transaction, Terrain, Label, Definition);
		// The importer centres the terrain on the origin with value 0 at Z 0: move it onto Location, value 32768 at its Z.
		Terrain->SetActorLocation(Location - FVector(0.0, 0.0, LandscapeMidValue * LandscapeZScale * HeightScale));
	}
	return WaitForBuild(Terrain, TimeoutSeconds) ? Terrain : nullptr;
}

FString UUnrealAIMeshTerrainCreateApi::Delete(UE::MeshPartition::AMeshPartition* Terrain)
{
	using namespace UnrealAIMeshTerrainCreateApiLocal;
	if (!CheckTerrain(Terrain))
		return FString();
	// Right after an undo / redo the editor component may not list the restored modifiers yet: refresh before counting and
	// before DestroyTerrain collects the actors from that list.
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
