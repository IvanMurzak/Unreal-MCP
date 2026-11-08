// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAILandscapeRqlApi.h"
#include "UnrealAILandscapeEdit.h"

#include "IUnrealMcpRqlModule.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeEditTypes.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"
#include "LandscapeUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "AssetCompilingManager.h"
#include "Containers/Ticker.h"
#include "Misc/App.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAILandscape, Log, All);

namespace UnrealAILandscapeRqlApiLocal
{
	/**
	 * The component layout of a created landscape SizeMeters wide (about one vertex per metre): sections of 63 quads, one or
	 * 2 x 2 per component, then 127-quad sections, so a side has at most 8 components. Many small components make a big
	 * landscape slow: a World Partition map's streaming proxies (ULandscapeSubsystem::ChangeGridSize) took 130 s for
	 * 2000 m in 32 x 32 components of 63 quads and 4 s in 8 x 8 of 254.
	 */
	struct FComponentLayout
	{
		int32 Subsections = 1;
		int32 SubsectionQuads = 63;
		int32 Quads() const { return Subsections * SubsectionQuads; }
	};

	FComponentLayout LayoutFor(double SizeMeters)
	{
		constexpr int32 MaxComponentsPerSide = 8;
		for (const FComponentLayout Layout : { FComponentLayout{ 1, 63 }, FComponentLayout{ 2, 63 } })
		{
			if (SizeMeters <= Layout.Quads() * MaxComponentsPerSide)
				return Layout;
		}
		return FComponentLayout{ 2, 127 };
	}

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

	/**
	 * The edit layer heights or weights (@p Target) are written to: the first one the standard tools can edit (not locked,
	 * not procedural such as splines); an invalid guid after Fail when there is none.
	 */
	FGuid EditLayerGuid(ALandscapeProxy* Landscape, ELandscapeToolTargetType Target)
	{
		const ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
		if (Primary)
		{
			for (const ULandscapeEditLayerBase* Layer : Primary->GetEditLayersConst())
			{
				if (Layer && Layer->SupportsEditingTools() && !Layer->IsLocked() && Layer->SupportsTargetType(Target))
					return Layer->GetGuid();
			}
		}
		UnrealMcpRql::Fail(TEXT("the landscape has no unlocked edit layer to write to (unlock one in Landscape mode)"));
		return FGuid();
	}

	void WriteData(FHeightmapAccessor<false>& Accessor, const FIntRect& R, const uint16* Data)
	{
		Accessor.SetData(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Data);
	}

	void WriteData(TAlphamapAccessor<false>& Accessor, const FIntRect& R, const uint8* Data)
	{
		Accessor.SetData(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Data, ELandscapeLayerPaintingRestriction::None);
	}

	/** Whether a component still waits for its requested in-stroke merge (heights or weights not yet in the final maps). */
	bool MergePending(const ALandscape* Primary)
	{
		constexpr uint32 InStroke = ELandscapeLayerUpdateMode::Update_Heightmap_Editing | ELandscapeLayerUpdateMode::Update_Heightmap_Editing_NoCollision
			| ELandscapeLayerUpdateMode::Update_Weightmap_Editing | ELandscapeLayerUpdateMode::Update_Weightmap_Editing_NoCollision;
		bool bPending = false;
		if (const ULandscapeInfo* Info = Primary->GetLandscapeInfo())
			Info->ForAllLandscapeComponents([&bPending](ULandscapeComponent* Component) { bPending |= (Component->GetLayerUpdateFlagPerMode() & InStroke) != 0; });
		return bPending;
	}

	/**
	 * Request @p Mode (a full pass: collision recreated, navigation and foliage refreshed) for @p Primary at the next tick.
	 * Requested at once, it would make every further in-stroke merge of the same frame (a batch of calls) a full one.
	 */
	void QueueFullUpdate(ALandscape* Primary, ELandscapeLayerUpdateMode Mode)
	{
		static TMap<TWeakObjectPtr<ALandscape>, uint32> Queued;
		if (Mode == ELandscapeLayerUpdateMode::Update_None)
			return;
		if (Queued.IsEmpty())
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				for (const TPair<TWeakObjectPtr<ALandscape>, uint32>& Entry : Queued)
				{
					if (ALandscape* Landscape = Entry.Key.Get())
						Landscape->RequestLayersContentUpdate(static_cast<ELandscapeLayerUpdateMode>(Entry.Value));
				}
				Queued.Reset();
				return false; // once
			}));
		}
		Queued.FindOrAdd(Primary) |= Mode;
	}

	/**
	 * Merge the edits now instead of over the next editor frames (a GPU merge, then a readback): the final heightmap, the
	 * collision heights (line traces, GetHeightAtLocation) and the rendered surface then match when the operation returns.
	 * The in-stroke merge the accessors requested covers only the components they touched; @p FollowUp then queues the full
	 * pass (collision recreated, navigation and foliage refreshed, as when a Landscape mode stroke ends) for the next tick,
	 * once for a whole batch of calls (QueueFullUpdate). Without rendering (-nullrhi, commandlets) nothing merges: the edit stays in its layer.
	 */
	void MergeNow(ALandscapeProxy* Landscape, ELandscapeLayerUpdateMode FollowUp)
	{
		constexpr double MergeWaitSeconds = 30.0; // well inside a tool call's 60 s timeout
		ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
		if (!Primary)
		{
			UE_LOG(LogUnrealAILandscape, Warning, TEXT("%s: its landscape actor is not loaded, so the edit merges when it is"), Landscape ? *Landscape->GetActorNameOrLabel() : TEXT("null"));
			return;
		}
		if (!FApp::CanEverRender())
		{
			UE_LOG(LogUnrealAILandscape, Warning, TEXT("%s: this editor does not render (-nullrhi), so the landscape edit stays unmerged (no collision or visible change) until a rendering editor opens it"), *Primary->GetActorNameOrLabel());
			return;
		}
		Primary->ForceUpdateLayersContent();
		if (MergePending(Primary))
		{
			// Waiting on shaders or textures still compiling (the first edit of a session): let them finish and merge again,
			// while anything compiles, for at most MergeWaitSeconds. FinishAllCompilation would also wait for every unrelated
			// shader (the first water material compiles for minutes on a cold cache), far past the tool call's timeout; with
			// nothing compiling the merge cannot progress here (e.g. a mobile preview feature level), so it stops at once.
			const double Deadline = FPlatformTime::Seconds() + MergeWaitSeconds;
			while (MergePending(Primary) && FAssetCompilingManager::Get().GetNumRemainingAssets() > 0 && FPlatformTime::Seconds() < Deadline)
			{
				FAssetCompilingManager::Get().ProcessAsyncTasks(true);
				FPlatformProcess::Sleep(0.02f);
				Primary->ForceUpdateLayersContent();
			}
			if (MergePending(Primary))
				UE_LOG(LogUnrealAILandscape, Warning, TEXT("%s: the landscape edit could not merge yet; it shows (and collides) after the next editor frames"), *Primary->GetActorNameOrLabel());
		}
		QueueFullUpdate(Primary, FollowUp);
	}

	/**
	 * Read the circle's vertices through @p Accessor (heights or one layer's weights, on the first edit layer), let @p Edit
	 * change each one with its falloff weight (true when it changed), write them back and refresh; how many changed.
	 */
	template <typename T, typename TAccessor>
	int32 EditCircle(ALandscapeProxy* Landscape, TAccessor& Accessor, const FCircle& Circle, TFunctionRef<bool(T&, double)> Edit)
	{
		const FGuid EditLayer = EditLayerGuid(Landscape, std::is_same_v<T, uint16> ? ELandscapeToolTargetType::Heightmap : ELandscapeToolTargetType::Weightmap);
		if (!EditLayer.IsValid())
			return 0;
		Accessor.SetEditLayer(EditLayer);
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
		if (Changed == 0)
			return 0;
		Landscape->Modify();
		WriteData(Accessor, R, Data.GetData()); // requests the in-stroke merge of the components it touched
		Accessor.Flush();
		MergeNow(Landscape, std::is_same_v<T, uint16> ? ELandscapeLayerUpdateMode::Update_Heightmap_All : ELandscapeLayerUpdateMode::Update_Weightmap_All);
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
		ALandscape* Primary = Landscape->GetLandscapeActor();
		if (!Primary)
			return UnrealMcpRql::Failed<ULandscapeLayerInfoObject*>(TEXT("the landscape has no primary landscape actor to add a layer to"));
		// The target layers are the source of truth (ULandscapeInfo::Layers is derived and can lag behind an undo).
		if (const FLandscapeTargetLayerSettings* Settings = Primary->GetTargetLayers().Find(Layer))
		{
			if (ULandscapeLayerInfoObject* Existing = Settings->LayerInfoObj.Get())
				return Existing;
		}
		// Beside a saved level ("<Map>_sharedassets/", as Landscape mode does), else under /Game/Landscape (an unsaved level).
		// An asset of the name already there (an earlier Paint, even unsaved or undone) is reused, never re-created in place.
		const FString LevelPackage = Primary->GetLevel() ? Primary->GetLevel()->GetOutermost()->GetName() : FString();
		const FString Folder = LevelPackage.StartsWith(TEXT("/Game/")) ? UE::Landscape::GetSharedAssetsPath(LevelPackage) : FString(TEXT("/Game/Landscape/LayerInfos"));
		const FString AssetName = Layer.ToString() + TEXT("_LayerInfo");
		const FString Package = FPaths::Combine(Folder, AssetName);
		ULandscapeLayerInfoObject* LayerInfo = Cast<ULandscapeLayerInfoObject>(UnrealMcpRql::LoadObject(ULandscapeLayerInfoObject::StaticClass(), Package));
		if (LayerInfo && LayerInfo->GetLayerName() != Layer)
			LayerInfo = nullptr; // a different layer's asset took the name: let the engine pick a unique one
		if (!LayerInfo)
			LayerInfo = UE::Landscape::CreateTargetLayerInfo(Layer, Folder);
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
	// Whole components (at most 8 per side), then a horizontal scale that makes the side exactly SizeMeters.
	const FComponentLayout Layout = LayoutFor(SizeMeters);
	const int32 Quads = FMath::CeilToInt(SizeMeters / Layout.Quads()) * Layout.Quads();
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
	Landscape->Import(FGuid::NewGuid(), 0, 0, Quads, Quads, Layout.Subsections, Layout.SubsectionQuads, HeightPerLayer, nullptr,
		MaterialLayersPerLayer, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (Info)
	{
		Info->UpdateLayerInfoMap(Landscape);
		// A World Partition map streams a landscape by grid cells, as Landscape mode's new landscape does (its default: 2 components).
		ULandscapeSubsystem* Subsystem = Landscape->GetWorld() ? Landscape->GetWorld()->GetSubsystem<ULandscapeSubsystem>() : nullptr;
		if (Subsystem && Subsystem->IsGridBased())
			Subsystem->ChangeGridSize(Info, 2);
	}
	Landscape->RequestLayersContentUpdateForceAll(); // a new landscape: merge everything at once
	MergeNow(Landscape, ELandscapeLayerUpdateMode::Update_None);
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
	if (!(Radius >= 0.0))
		return UnrealMcpRql::Failed<int32>(TEXT("Radius must be 0 (the whole landscape) or more (cm)"));
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
	if (!Blend)
		return UnrealMcpRql::Failed<UMaterial*>(FString::Printf(TEXT("could not add a Landscape Layer Blend to %s"), *Path));
	Blend->Modify();
	for (const TPair<FName, FLinearColor>& Layer : Parsed)
	{
		FLayerBlendInput& Input = Blend->Layers.AddDefaulted_GetRef();
		Input.LayerName = Layer.Key;
		Input.BlendType = LB_WeightBlend;
		Input.PreviewWeight = 0.0f;
		Input.ConstLayerInput = FVector(Layer.Value.R, Layer.Value.G, Layer.Value.B);
		Input.ConstHeightInput = 0.0f;
	}
	Blend->PostEditChange(); // the node's input pins follow the layer list
	UMaterialEditingLibrary::ConnectMaterialProperty(Blend, FString(), MP_BaseColor);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	return Material;
}

FUnrealAILandscapeSurface UUnrealAILandscapeRqlApi::Surface(ALandscapeProxy* Landscape, FVector Point)
{
	if (!Landscape)
		return UnrealMcpRql::Failed<FUnrealAILandscapeSurface>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
	FUnrealAILandscapeSurface Out;
	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	FIntRect Whole;
	if (!Info || !Info->GetLandscapeExtent(Whole))
		return Out;
	// The full-resolution collision heightfield (the editor's copy; the physics one when it has none): what line traces and
	// actors placed on the ground meet at the default collision mip 0, and what the editor renders up close. A point on the
	// far edge is nudged inside: the engine looks it up in a component past the last one.
	const FTransform ToWorld = Landscape->LandscapeActorToWorld();
	const auto HeightAt = [Landscape, &ToWorld, &Whole](double X, double Y) -> TOptional<float>
	{
		FVector Local = ToWorld.InverseTransformPosition(FVector(X, Y, ToWorld.GetLocation().Z));
		constexpr double Edge = 1e-3; // quads
		if (Local.X < Whole.Min.X - Edge || Local.Y < Whole.Min.Y - Edge || Local.X > Whole.Max.X + Edge || Local.Y > Whole.Max.Y + Edge)
			return TOptional<float>();
		Local.X = FMath::Clamp(Local.X, static_cast<double>(Whole.Min.X), Whole.Max.X - Edge);
		Local.Y = FMath::Clamp(Local.Y, static_cast<double>(Whole.Min.Y), Whole.Max.Y - Edge);
		const FVector Inside = ToWorld.TransformPosition(Local);
		const TOptional<float> Editor = Landscape->GetHeightAtLocation(Inside, EHeightfieldSource::Editor);
		return Editor.IsSet() ? Editor : Landscape->GetHeightAtLocation(Inside, EHeightfieldSource::Complex);
	};
	const TOptional<float> Height = HeightAt(Point.X, Point.Y);
	if (!Height.IsSet())
		return Out;
	// Slope over one quad on each side (one side at the landscape's edge).
	const FVector Quad = Landscape->LandscapeActorToWorld().GetScale3D().GetAbs().ComponentMax(FVector::OneVector);
	const auto Gradient = [&HeightAt, &Height, &Point](double DX, double DY)
	{
		const TOptional<float> Ahead = HeightAt(Point.X + DX, Point.Y + DY);
		const TOptional<float> Behind = HeightAt(Point.X - DX, Point.Y - DY);
		const double Span = (DX + DY) * ((Ahead.IsSet() ? 1.0 : 0.0) + (Behind.IsSet() ? 1.0 : 0.0));
		return Span > 0.0 ? (Ahead.Get(Height.GetValue()) - Behind.Get(Height.GetValue())) / Span : 0.0;
	};
	const double SlopeX = Gradient(Quad.X, 0.0);
	const double SlopeY = Gradient(0.0, Quad.Y);
	Out.Inside = true;
	Out.Height = Height.GetValue();
	Out.Location = FVector(Point.X, Point.Y, Out.Height);
	Out.Normal = FVector(-SlopeX, -SlopeY, 1.0).GetSafeNormal();
	Out.Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Out.Normal.Z, -1.0, 1.0)));
	return Out;
}

int32 UUnrealAILandscapeRqlApi::Rebuild(ALandscapeProxy* Landscape)
{
	ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
	ULandscapeInfo* Info = Primary ? Primary->GetLandscapeInfo() : nullptr;
	if (!Info)
		return UnrealMcpRql::Failed<int32>(TEXT("the landscape has no primary landscape actor to rebuild"));
	if (!FApp::CanEverRender())
		return UnrealMcpRql::Failed<int32>(TEXT("this editor does not render (-nullrhi): a landscape merges its edit layers only in a rendering editor"));
	Primary->ForceLayersFullUpdate(); // every edit layer merged again, collision recreated
	if (Primary->IsNaniteEnabled())
	{
		if (ULandscapeSubsystem* Subsystem = Primary->GetWorld() ? Primary->GetWorld()->GetSubsystem<ULandscapeSubsystem>() : nullptr)
		{
			ALandscapeProxy* Proxies[] = { Primary };
			Subsystem->BuildNanite(UE::Landscape::EBuildFlags::ForceRebuild, Proxies);
		}
	}
	int32 Components = 0;
	Info->ForEachLandscapeProxy([&Components](ALandscapeProxy* Proxy) { Components += Proxy->LandscapeComponents.Num(); return true; });
	return Components;
}
