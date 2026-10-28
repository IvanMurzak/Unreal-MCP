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
	/** Quads of the one section each component of a created landscape has. */
	constexpr int32 QuadsPerSection = 63;

	/** A circle of landscape vertices to edit: the landscape info, the covered vertex rectangle, centre and radius in vertices (0: everything). */
	struct FCircle
	{
		ULandscapeInfo* Info = nullptr;
		FIntRect Rect;
		FVector2D Center = FVector2D::ZeroVector;
		double Radius = 0.0;
	};

	/** The vertices a circle around @p Center (world X, Y) with @p Radius (cm; <= 0 the whole landscape) covers; false after Fail. */
	bool Circle(ALandscapeProxy* Landscape, const FVector& Center, double Radius, FCircle& Out)
	{
		Out.Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
		FIntRect Whole;
		if (!Out.Info || !Out.Info->GetLandscapeExtent(Whole))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no landscape components to edit"), Landscape ? *Landscape->GetActorNameOrLabel() : TEXT("null")));
		const FTransform ToWorld = Landscape->LandscapeActorToWorld();
		const FVector Local = ToWorld.InverseTransformPosition(FVector(Center.X, Center.Y, ToWorld.GetLocation().Z));
		Out.Center = FVector2D(Local.X, Local.Y);
		Out.Rect = Whole;
		if (Radius <= 0.0)
			return true;
		Out.Radius = Radius / FMath::Max(FMath::Abs(ToWorld.GetScale3D().X), UE_KINDA_SMALL_NUMBER);
		Out.Rect.Min.X = FMath::Max(Whole.Min.X, FMath::FloorToInt(Out.Center.X - Out.Radius));
		Out.Rect.Min.Y = FMath::Max(Whole.Min.Y, FMath::FloorToInt(Out.Center.Y - Out.Radius));
		Out.Rect.Max.X = FMath::Min(Whole.Max.X, FMath::CeilToInt(Out.Center.X + Out.Radius));
		Out.Rect.Max.Y = FMath::Min(Whole.Max.Y, FMath::CeilToInt(Out.Center.Y + Out.Radius));
		if (Out.Rect.Min.X > Out.Rect.Max.X || Out.Rect.Min.Y > Out.Rect.Max.Y)
			return UnrealMcpRql::Failed<bool>(TEXT("the circle does not touch the landscape"));
		return true;
	}

	/** 1 at the centre, a smooth (cosine) fall to 0 at the radius; 1 everywhere for a whole-landscape edit. */
	double Falloff(int32 X, int32 Y, const FCircle& Circle)
	{
		if (Circle.Radius <= 0.0)
			return 1.0;
		const double Distance = FVector2D::Distance(FVector2D(X, Y), Circle.Center) / Circle.Radius;
		return Distance >= 1.0 ? 0.0 : 0.5 * (1.0 + FMath::Cos(PI * Distance));
	}

	/** The edit layer heights and weights are written to (the first one), or an invalid guid for a landscape without. */
	FGuid EditLayerGuid(ALandscapeProxy* Landscape)
	{
		const ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
		const ULandscapeEditLayerBase* Layer = Primary ? Primary->GetEditLayerConst(0) : nullptr;
		return Layer ? Layer->GetGuid() : FGuid();
	}

	void WriteData(FHeightmapAccessor<false>& Accessor, const FIntRect& R, const uint16* Data)
	{
		Accessor.SetData(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Data);
	}

	void WriteData(TAlphamapAccessor<false>& Accessor, const FIntRect& R, const uint8* Data)
	{
		Accessor.SetData(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Data, ELandscapeLayerPaintingRestriction::None);
	}

	/**
	 * Read the circle's vertices through @p Accessor (heights or one layer's weights, on the first edit layer), let @p Edit
	 * change each one with its falloff weight (true when it changed), write them back and refresh; how many changed.
	 */
	template <typename T, typename TAccessor>
	int32 EditCircle(ALandscapeProxy* Landscape, TAccessor& Accessor, const FCircle& Circle, TFunctionRef<bool(T&, double)> Edit)
	{
		Accessor.SetEditLayer(EditLayerGuid(Landscape));
		const FIntRect& R = Circle.Rect;
		const int32 Width = R.Max.X - R.Min.X + 1;
		TArray<T> Data;
		Data.SetNumZeroed(Width * (R.Max.Y - R.Min.Y + 1));
		Accessor.GetDataFast(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Data.GetData());
		int32 Changed = 0;
		for (int32 Y = R.Min.Y; Y <= R.Max.Y; ++Y)
		{
			for (int32 X = R.Min.X; X <= R.Max.X; ++X)
				Changed += Edit(Data[(Y - R.Min.Y) * Width + (X - R.Min.X)], Falloff(X, Y, Circle)) ? 1 : 0;
		}
		Landscape->Modify();
		WriteData(Accessor, R, Data.GetData());
		Accessor.Flush();
		if (ALandscape* Primary = Landscape->GetLandscapeActor())
			Primary->RequestLayersContentUpdateForceAll();
		return Changed;
	}

	/** A paint layer name is one word ("Grass"); false after Fail naming @p Entry (the text it came from). */
	bool ValidLayerName(const FString& Name, const FString& Entry)
	{
		if (!Name.IsEmpty() && !Name.Contains(TEXT(" ")))
			return true;
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("'%s' is not a layer: a one-word name (Grass), in CreateLayerMaterial with an optional #rrggbb colour (\"Grass #3a7d2c\")"), *Entry));
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
			if (!ValidLayerName(Name, Entry))
				return false;
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
	// Whole components of one 63-quad section (at most 32 per side, for 2000 m), then a horizontal scale that makes the
	// side exactly SizeMeters.
	const int32 Quads = FMath::CeilToInt(SizeMeters / QuadsPerSection) * QuadsPerSection;
	const double Scale = SizeMeters * 100.0 / Quads;
	const FVector Corner = Location - FVector(SizeMeters * 50.0, SizeMeters * 50.0, 0.0);

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
	return Landscape;
}

int32 UUnrealAILandscapeRqlApi::Sculpt(ALandscapeProxy* Landscape, FVector Center, double Radius, double Height)
{
	if (!(Radius > 0.0))
		return UnrealMcpRql::Failed<int32>(TEXT("Radius must be greater than 0 (cm)"));
	if (!FMath::IsFinite(Height) || FMath::Abs(Height) > 100000.0)
		return UnrealMcpRql::Failed<int32>(TEXT("Height must be within -100000 .. 100000 cm"));
	FCircle Area;
	if (!Circle(Landscape, Center, Radius, Area))
		return 0;
	// Height units: LANDSCAPE_ZSCALE (1/128) of the actor's Z scale per unit, around the mid value 32768.
	const double ZScale = FMath::Max(FMath::Abs(Landscape->LandscapeActorToWorld().GetScale3D().Z), UE_KINDA_SMALL_NUMBER);
	const double Units = Height / (ZScale * LANDSCAPE_ZSCALE);
	FHeightmapAccessor<false> Accessor(Area.Info);
	return EditCircle<uint16>(Landscape, Accessor, Area, [Units](uint16& Value, double Weight)
	{
		const uint16 New = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(Value + Units * Weight), 0, 65535));
		const bool bChanged = New != Value;
		Value = New;
		return bChanged;
	});
}

int32 UUnrealAILandscapeRqlApi::Paint(ALandscapeProxy* Landscape, const FString& Layer, FVector Center, double Radius, double Strength)
{
	const FString Name = Layer.TrimStartAndEnd();
	if (!ValidLayerName(Name, Layer))
		return 0;
	if (!(Strength > 0.0 && Strength <= 1.0))
		return UnrealMcpRql::Failed<int32>(TEXT("Strength must be within 0 (exclusive) .. 1"));
	FCircle Area;
	if (!Circle(Landscape, Center, Radius, Area))
		return 0;
	ULandscapeLayerInfoObject* LayerInfo = LayerInfoFor(Landscape, Area.Info, FName(*Name));
	if (!LayerInfo)
		return 0;
	TAlphamapAccessor<false> Accessor(Area.Info, LayerInfo);
	return EditCircle<uint8>(Landscape, Accessor, Area, [Strength](uint8& Value, double Weight)
	{
		const uint8 Target = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(255.0 * Strength * Weight), 0, 255));
		if (Target <= Value)
			return false;
		Value = Target;
		return true;
	});
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
