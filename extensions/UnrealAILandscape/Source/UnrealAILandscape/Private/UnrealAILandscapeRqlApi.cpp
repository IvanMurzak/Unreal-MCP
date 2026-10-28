// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAILandscapeRqlApi.h"

#include "IUnrealMcpRqlModule.h"

#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"

namespace UnrealAILandscapeRqlApiLocal
{
	/** Quads per section and sections per component of a created landscape: one 63-quad section per component. */
	constexpr int32 QuadsPerSection = 63;
	constexpr int32 MaxComponentsPerSide = 32;

	/** The landscape's vertex extent (inclusive) and the edit layer writes go to; false after Fail. */
	bool Extent(ALandscapeProxy* Landscape, ULandscapeInfo*& OutInfo, FIntRect& OutRect)
	{
		OutInfo = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
		if (!OutInfo || !OutInfo->GetLandscapeExtent(OutRect))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no landscape components to edit"), Landscape ? *Landscape->GetActorNameOrLabel() : TEXT("null")));
			return false;
		}
		return true;
	}

	/**
	 * The vertex rectangle a circle around @p Center (world) with @p Radius (cm) covers, clamped to the landscape, plus the
	 * centre and radius in vertex units; a Radius <= 0 covers everything. False (no Fail) when the circle misses it.
	 */
	bool Region(ALandscapeProxy* Landscape, const FIntRect& Whole, const FVector& Center, double Radius, FIntRect& OutRect,
		FVector2D& OutCenter, double& OutRadius)
	{
		const FTransform ToWorld = Landscape->LandscapeActorToWorld();
		const FVector Local = ToWorld.InverseTransformPosition(FVector(Center.X, Center.Y, ToWorld.GetLocation().Z));
		OutCenter = FVector2D(Local.X, Local.Y);
		if (Radius <= 0.0)
		{
			OutRadius = 0.0;
			OutRect = Whole;
			return true;
		}
		const double Scale = FMath::Max(FMath::Abs(ToWorld.GetScale3D().X), UE_KINDA_SMALL_NUMBER);
		OutRadius = Radius / Scale;
		OutRect.Min.X = FMath::Max(Whole.Min.X, FMath::FloorToInt(OutCenter.X - OutRadius));
		OutRect.Min.Y = FMath::Max(Whole.Min.Y, FMath::FloorToInt(OutCenter.Y - OutRadius));
		OutRect.Max.X = FMath::Min(Whole.Max.X, FMath::CeilToInt(OutCenter.X + OutRadius));
		OutRect.Max.Y = FMath::Min(Whole.Max.Y, FMath::CeilToInt(OutCenter.Y + OutRadius));
		return OutRect.Min.X <= OutRect.Max.X && OutRect.Min.Y <= OutRect.Max.Y;
	}

	/** 1 at the centre, a smooth (cosine) fall to 0 at the radius; 1 everywhere for a whole-landscape edit. */
	double Falloff(int32 X, int32 Y, const FVector2D& Center, double Radius)
	{
		if (Radius <= 0.0)
			return 1.0;
		const double Distance = FVector2D::Distance(FVector2D(X, Y), Center) / Radius;
		return Distance >= 1.0 ? 0.0 : 0.5 * (1.0 + FMath::Cos(PI * Distance));
	}

	/** The edit layer heights and weights are written to (the first one), or an invalid guid for a landscape without. */
	FGuid EditLayerGuid(ALandscapeProxy* Landscape)
	{
		const ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
		const ULandscapeEditLayerBase* Layer = Primary ? Primary->GetEditLayerConst(0) : nullptr;
		return Layer ? Layer->GetGuid() : FGuid();
	}

	void RefreshLayers(ALandscapeProxy* Landscape)
	{
		if (ALandscape* Primary = Landscape->GetLandscapeActor())
			Primary->RequestLayersContentUpdateForceAll();
	}

	/** "Grass #3a7d2c, Rock" as (name, colour) pairs; false after Fail. Names without a colour get a distinct default. */
	bool ParseLayers(const FString& Text, TArray<TPair<FName, FLinearColor>>& Out)
	{
		static const FLinearColor Defaults[] = { FLinearColor(0.10f, 0.35f, 0.08f), FLinearColor(0.35f, 0.35f, 0.35f),
			FLinearColor(0.45f, 0.33f, 0.18f), FLinearColor(0.80f, 0.75f, 0.55f), FLinearColor(0.9f, 0.9f, 0.95f) };
		TArray<FString> Entries;
		Text.ParseIntoArray(Entries, TEXT(","), /*InCullEmpty*/ true);
		for (FString Entry : Entries)
		{
			Entry.TrimStartAndEndInline();
			if (Entry.IsEmpty())
				continue;
			FString Name = Entry;
			FString Colour;
			Entry.Split(TEXT(" "), &Name, &Colour, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			Name.TrimStartAndEndInline();
			Colour.TrimStartAndEndInline();
			if (Name.IsEmpty() || Name.Contains(TEXT(" ")))
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("'%s' is not a layer: write a name and an optional #rrggbb colour (\"Grass #3a7d2c\")"), *Entry));
			FLinearColor Linear = Defaults[Out.Num() % UE_ARRAY_COUNT(Defaults)];
			if (!Colour.IsEmpty())
			{
				if (!Colour.StartsWith(TEXT("#")) || (Colour.Len() != 7 && Colour.Len() != 9))
					return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("'%s' is not a #rrggbb colour (layer %s)"), *Colour, *Name));
				Linear = FLinearColor::FromSRGBColor(FColor::FromHex(Colour));
			}
			if (Out.ContainsByPredicate([&Name](const TPair<FName, FLinearColor>& Layer) { return Layer.Key == FName(*Name); }))
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("layer %s is listed twice"), *Name));
			Out.Emplace(FName(*Name), Linear);
		}
		if (Out.Num() == 0)
			return UnrealMcpRql::Failed<bool>(TEXT("list at least one layer: \"Grass #3a7d2c, Rock #808080\""));
		return true;
	}

	/** The layer info of @p Layer on the landscape, created (asset beside the level) and added as a target layer when missing. */
	ULandscapeLayerInfoObject* LayerInfoFor(ALandscapeProxy* Landscape, ULandscapeInfo* Info, const FName Layer)
	{
		const int32 Existing = Info->GetLayerInfoIndex(Layer);
		if (Existing != INDEX_NONE && Info->Layers[Existing].LayerInfoObj)
			return Info->Layers[Existing].LayerInfoObj;
		ALandscape* Primary = Landscape->GetLandscapeActor();
		if (!Primary)
			return UnrealMcpRql::Failed<ULandscapeLayerInfoObject*>(TEXT("the landscape has no primary landscape actor to add a layer to"));
		// Beside a saved level ("<Map>_sharedassets/", as Landscape mode does), else under /Game/Landscape (an unsaved level).
		const FString LevelPackage = Primary->GetLevel() ? Primary->GetLevel()->GetOutermost()->GetName() : FString();
		const FString Folder = LevelPackage.StartsWith(TEXT("/Game/")) ? UE::Landscape::GetSharedAssetsPath(LevelPackage) : FString(TEXT("/Game/Landscape/LayerInfos"));
		ULandscapeLayerInfoObject* LayerInfo = UE::Landscape::CreateTargetLayerInfo(Layer, Folder);
		if (!LayerInfo)
			return UnrealMcpRql::Failed<ULandscapeLayerInfoObject*>(FString::Printf(TEXT("could not create the layer info of %s"), *Layer.ToString()));
		Primary->Modify();
		if (Primary->GetTargetLayers().Contains(Layer))
			Primary->UpdateTargetLayer(Layer, FLandscapeTargetLayerSettings(LayerInfo));
		else
			Primary->AddTargetLayer(Layer, FLandscapeTargetLayerSettings(LayerInfo));
		Info->UpdateLayerInfoMap(Primary);
		const int32 Index = Info->GetLayerInfoIndex(Layer);
		if (Index != INDEX_NONE)
			Info->Layers[Index].LayerInfoObj = LayerInfo;
		return LayerInfo;
	}
}

using namespace UnrealAILandscapeRqlApiLocal;

ALandscape* UUnrealAILandscapeRqlApi::Create(const FString& Label, FVector Location, double SizeMeters, UMaterialInterface* Material)
{
	if (!(SizeMeters >= 8.0 && SizeMeters <= 2000.0))
		return UnrealMcpRql::Failed<ALandscape*>(FString::Printf(TEXT("SizeMeters %g is outside 8 .. 2000"), SizeMeters));
	// Whole components of one 63-quad section, then a horizontal scale that makes the side exactly SizeMeters.
	const int32 Components = FMath::Clamp(FMath::CeilToInt(SizeMeters / QuadsPerSection), 1, MaxComponentsPerSide);
	const int32 Quads = Components * QuadsPerSection;
	const double Scale = SizeMeters * 100.0 / Quads;
	const FVector Corner = Location - FVector(Quads * Scale / 2.0, Quads * Scale / 2.0, 0.0);

	ALandscape* Landscape = Cast<ALandscape>(UnrealMcpRql::SpawnActor(ALandscape::StaticClass(), Corner, FRotator::ZeroRotator, Label));
	if (!Landscape)
		return nullptr; // SpawnActor failed with the reason
	Landscape->SetActorRelativeScale3D(FVector(Scale, Scale, 100.0));
	Landscape->LandscapeMaterial = Material;
	Landscape->StaticLightingLOD = 0;

	const int32 Size = Quads + 1;
	TArray<uint16> Heights;
	Heights.Init(LandscapeDataAccess::MidValue, Size * Size);
	TMap<FGuid, TArray<uint16>> HeightPerLayer;
	HeightPerLayer.Add(FGuid(), MoveTemp(Heights));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayersPerLayer;
	MaterialLayersPerLayer.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
	Landscape->Import(FGuid::NewGuid(), 0, 0, Quads, Quads, /*InNumSubsections*/ 1, QuadsPerSection, HeightPerLayer, nullptr,
		MaterialLayersPerLayer, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
		Info->UpdateLayerInfoMap(Landscape);
	if (!Label.IsEmpty())
		Landscape->SetActorLabel(Label);
	return Landscape;
}

int32 UUnrealAILandscapeRqlApi::Sculpt(ALandscapeProxy* Landscape, FVector Center, double Radius, double Height)
{
	if (!(Radius > 0.0))
		return UnrealMcpRql::Failed<int32>(TEXT("Radius must be greater than 0 (cm)"));
	if (!FMath::IsFinite(Height) || FMath::Abs(Height) > 100000.0)
		return UnrealMcpRql::Failed<int32>(TEXT("Height must be within -100000 .. 100000 cm"));
	ULandscapeInfo* Info = nullptr;
	FIntRect Whole;
	if (!Extent(Landscape, Info, Whole))
		return 0;
	FIntRect Rect;
	FVector2D Local;
	double LocalRadius = 0.0;
	if (!Region(Landscape, Whole, Center, Radius, Rect, Local, LocalRadius))
		return UnrealMcpRql::Failed<int32>(TEXT("the circle does not touch the landscape"));
	// Height units: LANDSCAPE_ZSCALE (1/128) of the actor's Z scale per unit, around the mid value 32768.
	const double ZScale = FMath::Max(FMath::Abs(Landscape->LandscapeActorToWorld().GetScale3D().Z), UE_KINDA_SMALL_NUMBER);
	const double Units = Height / (ZScale * LANDSCAPE_ZSCALE);

	FHeightmapAccessor<false> Accessor(Info);
	Accessor.SetEditLayer(EditLayerGuid(Landscape));
	int32 X1 = Rect.Min.X, Y1 = Rect.Min.Y, X2 = Rect.Max.X, Y2 = Rect.Max.Y;
	const int32 Width = X2 - X1 + 1;
	TArray<uint16> Data;
	Data.SetNumZeroed(Width * (Y2 - Y1 + 1));
	Accessor.GetDataFast(X1, Y1, X2, Y2, Data.GetData());
	int32 Changed = 0;
	for (int32 Y = Y1; Y <= Y2; ++Y)
	{
		for (int32 X = X1; X <= X2; ++X)
		{
			const double Weight = Falloff(X, Y, Local, LocalRadius);
			if (Weight <= 0.0)
				continue;
			uint16& Value = Data[(Y - Y1) * Width + (X - X1)];
			const uint16 New = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(Value + Units * Weight), 0, 65535));
			Changed += New != Value ? 1 : 0;
			Value = New;
		}
	}
	Landscape->Modify();
	Accessor.SetData(X1, Y1, X2, Y2, Data.GetData());
	Accessor.Flush();
	RefreshLayers(Landscape);
	return Changed;
}

int32 UUnrealAILandscapeRqlApi::Paint(ALandscapeProxy* Landscape, const FString& Layer, FVector Center, double Radius, double Strength)
{
	const FString Name = Layer.TrimStartAndEnd();
	if (Name.IsEmpty() || Name.Contains(TEXT(" ")))
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("'%s' is not a layer name (one word, e.g. Grass)"), *Layer));
	if (!(Strength > 0.0 && Strength <= 1.0))
		return UnrealMcpRql::Failed<int32>(TEXT("Strength must be within 0 (exclusive) .. 1"));
	ULandscapeInfo* Info = nullptr;
	FIntRect Whole;
	if (!Extent(Landscape, Info, Whole))
		return 0;
	FIntRect Rect;
	FVector2D Local;
	double LocalRadius = 0.0;
	if (!Region(Landscape, Whole, Center, Radius, Rect, Local, LocalRadius))
		return UnrealMcpRql::Failed<int32>(TEXT("the circle does not touch the landscape"));
	ULandscapeLayerInfoObject* LayerInfo = LayerInfoFor(Landscape, Info, FName(*Name));
	if (!LayerInfo)
		return 0;

	TAlphamapAccessor<false> Accessor(Info, LayerInfo);
	Accessor.SetEditLayer(EditLayerGuid(Landscape));
	const int32 X1 = Rect.Min.X, Y1 = Rect.Min.Y, X2 = Rect.Max.X, Y2 = Rect.Max.Y;
	const int32 Width = X2 - X1 + 1;
	TArray<uint8> Data;
	Data.SetNumZeroed(Width * (Y2 - Y1 + 1));
	Accessor.GetDataFast(X1, Y1, X2, Y2, Data.GetData());
	int32 Changed = 0;
	for (int32 Y = Y1; Y <= Y2; ++Y)
	{
		for (int32 X = X1; X <= X2; ++X)
		{
			uint8& Value = Data[(Y - Y1) * Width + (X - X1)];
			const uint8 Target = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(255.0 * Strength * Falloff(X, Y, Local, LocalRadius)), 0, 255));
			if (Target > Value)
			{
				Value = Target;
				++Changed;
			}
		}
	}
	Landscape->Modify();
	Accessor.SetData(X1, Y1, X2, Y2, Data.GetData(), ELandscapeLayerPaintingRestriction::None);
	Accessor.Flush();
	RefreshLayers(Landscape);
	return Changed;
}

UMaterial* UUnrealAILandscapeRqlApi::CreateLayerMaterial(const FString& Path, const FString& Layers)
{
	TArray<TPair<FName, FLinearColor>> Parsed;
	if (!ParseLayers(Layers, Parsed))
		return nullptr;
	UMaterial* Material = Cast<UMaterial>(UnrealMcpRql::CreateAsset(UMaterial::StaticClass(), Path));
	if (!Material)
		return nullptr; // CreateAsset failed with the reason
	UMaterialExpressionLandscapeLayerBlend* Blend = Cast<UMaterialExpressionLandscapeLayerBlend>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionLandscapeLayerBlend::StaticClass(), -400, 0));
	for (const TPair<FName, FLinearColor>& Layer : Parsed)
	{
		FLayerBlendInput& Input = Blend->Layers.AddDefaulted_GetRef();
		Input.LayerName = Layer.Key;
		Input.BlendType = LB_WeightBlend;
		Input.PreviewWeight = 0.0f;
		Input.ConstLayerInput = FVector(Layer.Value.R, Layer.Value.G, Layer.Value.B);
		Input.ConstHeightInput = 0.0f;
	}
	UMaterialEditingLibrary::ConnectMaterialProperty(Blend, FString(), MP_BaseColor);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	return Material;
}
