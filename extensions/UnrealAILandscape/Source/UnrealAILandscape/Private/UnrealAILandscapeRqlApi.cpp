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
#include "LandscapeStreamingProxy.h"
#include "LandscapeSubsystem.h"
#include "LandscapeUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "AssetCompilingManager.h"
#include "Containers/Ticker.h"
#include "Misc/App.h"
#include "EngineDefines.h" // UE_LARGE_HALF_WORLD_MAX
#include "Misc/Paths.h"
#include "WorldPartition/ActorPartition/PartitionActorDesc.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

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

	/** cm per heightmap unit: the actor's Z scale / 128. */
	double UnitCm(const ALandscapeProxy* Landscape)
	{
		return FMath::Max(FMath::Abs(Landscape->LandscapeActorToWorld().GetScale3D().Z), UE_KINDA_SMALL_NUMBER) * LANDSCAPE_ZSCALE;
	}

	/** The world Z (cm) of a heightmap value (MidValue is the actor's Z). */
	double UnitsToCm(const ALandscapeProxy* Landscape, double Units)
	{
		return Landscape->LandscapeActorToWorld().GetLocation().Z + (Units - LandscapeDataAccess::MidValue) * UnitCm(Landscape);
	}

	FVector2D HeightRange(const ALandscapeProxy* Landscape)
	{
		return FVector2D(UnitsToCm(Landscape, 0.0), UnitsToCm(Landscape, LandscapeDataAccess::MaxValue));
	}

	bool FitsHeightRange(const ALandscapeProxy* Landscape, double LowCm, double HighCm)
	{
		const FVector2D Range = HeightRange(Landscape);
		if (LowCm >= Range.X && HighCm <= Range.Y)
			return true;
		const double Origin = Landscape->LandscapeActorToWorld().GetLocation().Z;
		const double Needed = FMath::Max(FMath::Abs(HighCm - Origin), FMath::Abs(LowCm - Origin)) / 100.0;
		if (Needed > 10000.0)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("the ground would reach %.0f .. %.0f cm, %.0f m from %s's Z: more than any landscape holds (MaxHeightMeters 10000): nothing was changed"),
				LowCm, HighCm, Needed, *Landscape->GetActorNameOrLabel()));
		const int32 Suggested = FMath::Min(10000, FMath::CeilToInt(Needed * 1.1 / 50.0) * 50);
		return UnrealMcpRql::Failed<bool>(FString::Printf(
			TEXT("the ground would reach %.0f .. %.0f cm, outside %s's height range %.0f .. %.0f cm (%.0f m around its Z %.0f cm): nothing was changed. ")
			TEXT("Its range is fixed when it is created: recreate it with landscape | call Create(\"%s\", <location>, <size>, <material>, %d) (MaxHeightMeters %d), or keep the edit within the range"),
			LowCm, HighCm, *Landscape->GetActorNameOrLabel(), Range.X, Range.Y, (Range.Y - Range.X) / 200.0, Origin,
			*Landscape->GetActorNameOrLabel(), Suggested, Suggested));
	}

	bool LoadLandscape(ALandscapeProxy* Landscape, const FBox2D* Area)
	{
		UWorld* World = Landscape ? Landscape->GetWorld() : nullptr;
		UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
		// LoadActors loads into the editor world only (another world, e.g. play in editor, streams by itself).
		if (!Partition || World != UnrealMcpRql::EditorWorld())
			return true;
		const FGuid Guid = Landscape->GetLandscapeGuid();
		TArray<FGuid> ToLoad;
		FWorldPartitionHelpers::ForEachActorDescInstance<ALandscapeStreamingProxy>(Partition, [&](const FWorldPartitionActorDescInstance* Instance)
		{
			const FPartitionActorDesc* Desc = static_cast<const FPartitionActorDesc*>(Instance->GetActorDesc());
			if (Desc->GridGuid != Guid || Instance->IsLoaded())
				return true;
			const FBox Box = Instance->GetEditorBounds();
			if (!Area || FBox2D(FVector2D(Box.Min), FVector2D(Box.Max)).Intersect(*Area))
				ToLoad.Add(Instance->GetGuid());
			return true;
		});
		if (ToLoad.IsEmpty())
			return true;
		// UWorldPartitionBlueprintLibrary::LoadActors (the editor's pinned actor list; UnloadActors releases them) is not
		// exported from the Engine module: call it through reflection.
		static UClass* const Library = FindObject<UClass>(nullptr, TEXT("/Script/Engine.WorldPartitionBlueprintLibrary"));
		static UFunction* const Load = Library ? Library->FindFunctionByName(TEXT("LoadActors")) : nullptr;
		if (!Load)
			return UnrealMcpRql::Failed<bool>(TEXT("this engine has no WorldPartitionBlueprintLibrary.LoadActors: load the landscape's regions in the World Partition window first"));
		struct { TArray<FGuid> InActorsToLoad; } Params{ MoveTemp(ToLoad) };
		Library->GetDefaultObject()->ProcessEvent(Load, &Params);
		return true;
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
		if (!Landscape)
			return UnrealMcpRql::Failed<bool>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
		if (Center.ContainsNaN() || !FMath::IsFinite(Radius))
			return UnrealMcpRql::Failed<bool>(TEXT("Center and Radius must be finite numbers"));
		const FBox2D Area(FVector2D(Center.X - Radius, Center.Y - Radius), FVector2D(Center.X + Radius, Center.Y + Radius));
		if (!LoadLandscape(Landscape, Radius > 0.0 ? &Area : nullptr))
			return false;
		Out.Info = Landscape->GetLandscapeInfo();
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
		// Clamp in double before converting: a huge radius or far centre would overflow int32.
		const auto Clamped = [](double Value, int32 Low, int32 High) { return FMath::Clamp(Value, Low - 1.0, High + 1.0); };
		Out.Rect.Min.X = FMath::Max(Whole.Min.X, FMath::FloorToInt(Clamped(Out.Center.X - Out.Radius, Whole.Min.X, Whole.Max.X)));
		Out.Rect.Min.Y = FMath::Max(Whole.Min.Y, FMath::FloorToInt(Clamped(Out.Center.Y - Out.Radius, Whole.Min.Y, Whole.Max.Y)));
		Out.Rect.Max.X = FMath::Min(Whole.Max.X, FMath::CeilToInt(Clamped(Out.Center.X + Out.Radius, Whole.Min.X, Whole.Max.X)));
		Out.Rect.Max.Y = FMath::Min(Whole.Max.Y, FMath::CeilToInt(Clamped(Out.Center.Y + Out.Radius, Whole.Min.Y, Whole.Max.Y)));
		if (Out.Rect.Min.X > Out.Rect.Max.X || Out.Rect.Min.Y > Out.Rect.Max.Y)
			return UnrealMcpRql::Failed<bool>(TEXT("the circle does not touch the landscape"));
		return true;
	}

	/** 1 at @p T = 0, a smooth (cosine) fall to 0 at @p T = 1 and beyond. */
	double Cosine(double T)
	{
		return T >= 1.0 ? 0.0 : T <= 0.0 ? 1.0 : 0.5 * (1.0 + FMath::Cos(PI * T));
	}

	/** 1 at the centre, a smooth fall to 0 at the circle's radius (@p Distance in vertices); 1 everywhere for a whole-landscape edit. */
	double Falloff(double Distance, const FCircle& Circle)
	{
		return Circle.Radius <= 0.0 ? 1.0 : Cosine(Distance / Circle.Radius);
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
	 * change each one given its distance from the centre in vertices (true when it changed), let @p BeforeWrite check the
	 * new values of the rectangle (false after Fail: nothing is written) or adjust what goes with them, write them back and
	 * refresh; how many changed.
	 */
	template <typename T, typename TAccessor>
	int32 EditCircle(ALandscapeProxy* Landscape, TAccessor& Accessor, const FCircle& Circle, TFunctionRef<bool(T&, double)> Edit,
		TFunctionRef<bool(const FGuid&, const FIntRect&, const TArray<T>&)> BeforeWrite = [](const FGuid&, const FIntRect&, const TArray<T>&) { return true; })
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
				Changed += Edit(Data[(Y - R.Min.Y) * Width + (X - R.Min.X)], FVector2D::Distance(FVector2D(X, Y), Circle.Center)) ? 1 : 0;
		}
		if (Changed == 0 || !BeforeWrite(EditLayer, R, Data))
			return 0;
		Landscape->Modify();
		WriteData(Accessor, R, Data.GetData()); // requests the in-stroke merge of the components it touched
		Accessor.Flush();
		MergeNow(Landscape, std::is_same_v<T, uint16> ? ELandscapeLayerUpdateMode::Update_Heightmap_All : ELandscapeLayerUpdateMode::Update_Weightmap_All);
		return Changed;
	}

	/**
	 * A weight-blended layer painted to @p Weights over @p R takes its weight from the landscape's other weight-blended layers
	 * there, in proportion, so the layers sum to at most 1 (255). Landscape mode's paint does this; the engine's alphamap
	 * accessor writes one layer only (UE 5.7+ normalises weights only at the final merge, which then mixed Rock painted at
	 * full strength half and half with the Grass under it).
	 */
	void TakeWeightFromOthers(ULandscapeInfo* Info, const ULandscapeLayerInfoObject* Painted, const FGuid& EditLayer, const FIntRect& R, const TArray<uint8>& Weights)
	{
		if (Painted->GetBlendMethod() == ELandscapeTargetLayerBlendMethod::None)
			return;
		TArray<ULandscapeLayerInfoObject*> Others;
		for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
		{
			ULandscapeLayerInfoObject* Other = Layer.LayerInfoObj;
			if (Other && Other != Painted && Other != ALandscapeProxy::VisibilityLayer && Other->GetBlendMethod() != ELandscapeTargetLayerBlendMethod::None)
				Others.AddUnique(Other);
		}
		TArray<TArray<uint8>> Data;
		TArray<int32> Sum;
		Sum.SetNumZeroed(Weights.Num());
		for (ULandscapeLayerInfoObject* Other : Others)
		{
			TAlphamapAccessor<false> Accessor(Info, Other);
			Accessor.SetEditLayer(EditLayer);
			TArray<uint8>& Values = Data.AddDefaulted_GetRef();
			Values.SetNumZeroed(Weights.Num());
			Accessor.GetDataFast(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Values.GetData());
			for (int32 Index = 0; Index < Values.Num(); ++Index)
				Sum[Index] += Values[Index];
		}
		for (int32 Layer = 0; Layer < Others.Num(); ++Layer)
		{
			bool bChanged = false;
			for (int32 Index = 0; Index < Weights.Num(); ++Index)
			{
				const int32 Room = 255 - Weights[Index];
				if (Sum[Index] <= Room || Data[Layer][Index] == 0)
					continue;
				Data[Layer][Index] = static_cast<uint8>(FMath::FloorToInt(static_cast<double>(Data[Layer][Index]) * Room / Sum[Index]));
				bChanged = true;
			}
			if (!bChanged)
				continue;
			TAlphamapAccessor<false> Accessor(Info, Others[Layer]);
			Accessor.SetEditLayer(EditLayer);
			WriteData(Accessor, R, Data[Layer].GetData());
			Accessor.Flush();
		}
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
		if (LayerInfo && LayerInfo->GetBlendMethod() == ELandscapeTargetLayerBlendMethod::None)
			LayerInfo->SetBlendMethod(ELandscapeTargetLayerBlendMethod::FinalWeightBlending, /*bInModify*/ true); // made by an earlier version
		if (!LayerInfo)
		{
			// Weight-blended, as Landscape mode's default "Weight-Blended Layer (normal)": painting one layer takes the weight
			// from the others, so the layers sum to 1 and a LB_WeightBlend material (CreateLayerMaterial) shows one of them.
			// The engine's default layer info blends nothing: Rock painted over Grass then rendered both added together.
			// A project default layer info (Landscape settings) keeps its own blend method unless that blends nothing.
			LayerInfo = UE::Landscape::CreateTargetLayerInfo(Layer, Folder);
			if (LayerInfo && LayerInfo->GetBlendMethod() == ELandscapeTargetLayerBlendMethod::None)
				LayerInfo->SetBlendMethod(ELandscapeTargetLayerBlendMethod::FinalWeightBlending, /*bInModify*/ false);
		}
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

ALandscape* UUnrealAILandscapeRqlApi::Create(const FString& Label, FVector Location, double SizeMeters, UMaterialInterface* Material, double MaxHeightMeters)
{
	if (!(SizeMeters >= 8.0 && SizeMeters <= 2000.0))
		return UnrealMcpRql::Failed<ALandscape*>(FString::Printf(TEXT("SizeMeters %g is outside 8 .. 2000"), SizeMeters));
	if (!(MaxHeightMeters >= 1.0 && MaxHeightMeters <= 10000.0))
		return UnrealMcpRql::Failed<ALandscape*>(FString::Printf(TEXT("MaxHeightMeters %g is outside 1 .. 10000"), MaxHeightMeters));
	// Beyond the engine's large-world bound the renderer loses precision (an ensure every frame, CheckMatrixPrecision).
	const double MaxCoordinate = UE_LARGE_HALF_WORLD_MAX - SizeMeters * 50.0; // the corner stays inside too
	if (Location.ContainsNaN() || Location.GetAbsMax() > MaxCoordinate)
		return UnrealMcpRql::Failed<ALandscape*>(FString::Printf(TEXT("Location must be finite numbers within +-%.0f cm (the engine's world bound)"), MaxCoordinate));
	// Z scale 100 (Landscape mode's default) holds +-256 m in 0.78 cm steps; more room needs a larger scale, coarser steps.
	const double ZScale = FMath::Max(100.0, FMath::CeilToDouble(MaxHeightMeters / 2.56));
	// Whole components (at most 8 per side), then a horizontal scale that makes the side exactly SizeMeters.
	const FComponentLayout Layout = LayoutFor(SizeMeters);
	const int32 Quads = FMath::CeilToInt(SizeMeters / Layout.Quads()) * Layout.Quads();
	const double Scale = SizeMeters * 100.0 / Quads;
	const FVector Corner = Location - FVector(SizeMeters * 50.0, SizeMeters * 50.0, 0.0);

	ALandscape* Landscape = Cast<ALandscape>(UnrealMcpRql::SpawnActor(ALandscape::StaticClass(), Corner, FRotator::ZeroRotator, Label));
	if (!Landscape)
		return nullptr; // SpawnActor failed with the reason
	Landscape->SetActorRelativeScale3D(FVector(Scale, Scale, ZScale));
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
	const double Units = Height / UnitCm(Landscape);
	FHeightmapAccessor<false> Accessor(Area.Info);
	int32 Low = TNumericLimits<int32>::Max(), High = TNumericLimits<int32>::Lowest(); // the unclamped result, height units
	return EditCircle<uint16>(Landscape, Accessor, Area, [Units, &Area, &Low, &High](uint16& Value, double Distance)
	{
		const int32 Raw = FMath::RoundToInt(Value + Units * Falloff(Distance, Area));
		if (Raw == Value)
			return false;
		Low = FMath::Min(Low, Raw);
		High = FMath::Max(High, Raw);
		Value = static_cast<uint16>(FMath::Clamp(Raw, 0, 65535));
		return true;
	}, [Landscape, &Low, &High](const FGuid&, const FIntRect&, const TArray<uint16>&)
	{
		return FitsHeightRange(Landscape, UnitsToCm(Landscape, Low), UnitsToCm(Landscape, High));
	});
}

int32 UUnrealAILandscapeRqlApi::Flatten(ALandscapeProxy* Landscape, FVector Center, double Radius, double Height, double Falloff)
{
	if (!(Radius > 0.0) || !(Falloff >= 0.0) || !FMath::IsFinite(Radius + Falloff))
		return UnrealMcpRql::Failed<int32>(TEXT("Radius must be greater than 0 and Falloff 0 or more (cm)"));
	if (!FMath::IsFinite(Height))
		return UnrealMcpRql::Failed<int32>(TEXT("Height must be a finite world Z (cm)"));
	FCircle Area;
	if (!Circle(Landscape, Center, Radius + Falloff, Area) || !FitsHeightRange(Landscape, Height, Height))
		return 0;
	const double Target = LandscapeDataAccess::MidValue + (Height - Landscape->LandscapeActorToWorld().GetLocation().Z) / UnitCm(Landscape);
	const double Inner = Area.Radius * Radius / (Radius + Falloff); // vertices
	const double Band = Area.Radius - Inner;
	FHeightmapAccessor<false> Accessor(Area.Info);
	return EditCircle<uint16>(Landscape, Accessor, Area, [Target, Inner, Band](uint16& Value, double Distance)
	{
		const double Weight = Distance <= Inner ? 1.0 : Band > 0.0 ? Cosine((Distance - Inner) / Band) : 0.0;
		const uint16 New = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(FMath::Lerp(static_cast<double>(Value), Target, Weight)), 0, 65535));
		const bool bChanged = New != Value;
		Value = New;
		return bChanged;
	});
}

int32 UUnrealAILandscapeRqlApi::Carve(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Depth)
{
	if (!Landscape)
		return UnrealMcpRql::Failed<int32>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
	if (Points.Num() < 2 || Points.Num() > 1000)
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("Carve needs 2 .. 1000 points, got %d"), Points.Num()));
	if (Points.ContainsByPredicate([](const FVector& Point) { return Point.ContainsNaN(); }))
		return UnrealMcpRql::Failed<int32>(TEXT("Points must be finite numbers"));
	if (!(Width >= 100.0 && Width <= 20000.0))
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("Width %g is outside 100 .. 20000 cm"), Width));
	if (!(Depth >= 1.0 && Depth <= 10000.0))
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("Depth %g is outside 1 .. 10000 cm"), Depth));
	return CarveAlong(Landscape, Points, Width, Depth);
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
	return EditCircle<uint8>(Landscape, Accessor, Area, [Strength, &Area](uint8& Value, double Distance)
	{
		const uint8 Target = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(255.0 * Strength * Falloff(Distance, Area)), 0, 255));
		if (Target <= Value)
			return false;
		Value = Target;
		return true;
	}, [&Area, LayerInfo](const FGuid& EditLayer, const FIntRect& R, const TArray<uint8>& Weights)
	{
		TakeWeightFromOthers(Area.Info, LayerInfo, EditLayer, R, Weights);
		return true;
	});
}

int32 UUnrealAILandscapeRqlApi::Smooth(ALandscapeProxy* Landscape, FVector Center, double Radius, double Strength)
{
	if (!(Radius > 0.0 && Radius <= 30000.0))
		return UnrealMcpRql::Failed<int32>(TEXT("Radius must be within 0 (exclusive) .. 30000 cm"));
	if (!(Strength > 0.0 && Strength <= 1.0))
		return UnrealMcpRql::Failed<int32>(TEXT("Strength must be within 0 (exclusive) .. 1"));
	// The circle plus a ring of 3 vertices around it: the ground the new surface is fitted to.
	const double Quad = FMath::Max(Landscape ? FMath::Abs(Landscape->LandscapeActorToWorld().GetScale3D().X) : 1.0, UE_KINDA_SMALL_NUMBER);
	constexpr double MaxInner = 512.0; // vertices: the fill converges within the time limit up to here
	if (Radius / Quad > MaxInner)
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("Radius %.0f cm is %.0f vertices on this landscape, more than Smooth handles (%.0f, %.0f cm): smooth smaller places one by one"), Radius, Radius / Quad, MaxInner, MaxInner * Quad));
	FCircle Area;
	if (!Circle(Landscape, Center, Radius + 3.0 * Quad, Area))
		return 0;
	const double Inner = Radius / Quad; // vertices
	const FGuid EditLayer = EditLayerGuid(Landscape, ELandscapeToolTargetType::Heightmap);
	if (!EditLayer.IsValid())
		return 0;
	const FIntRect& R = Area.Rect;
	const int32 W = R.Width() + 1, H = R.Height() + 1;
	// The ground: with one height layer that layer, with several their sum (the merged heights); the change is then added to
	// the layer edits are written to, as Carve and Ramp do.
	int32 HeightLayers = 0;
	if (const ALandscape* Primary = Landscape->GetLandscapeActor())
	{
		for (const ULandscapeEditLayerBase* Layer : Primary->GetEditLayersConst())
			HeightLayers += Layer && Layer->SupportsTargetType(ELandscapeToolTargetType::Heightmap) ? 1 : 0;
	}
	TArray<uint16> Ground;
	Ground.SetNumZeroed(W * H);
	{
		FHeightmapAccessor<false> Reader(Area.Info);
		if (HeightLayers <= 1)
			Reader.SetEditLayer(EditLayer);
		Reader.GetDataFast(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Ground.GetData());
	}
	// Vertices with a loaded component under them (the rest of the rectangle reads as 0, not ground).
	TBitArray<> Valid(false, W * H);
	const int32 ComponentQuads = Area.Info->ComponentSizeQuads;
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const int32 VX = R.Min.X + X, VY = R.Min.Y + Y;
			const auto Has = [&Area, ComponentQuads](int32 CX, int32 CY) { return Area.Info->XYtoComponentMap.Contains(FIntPoint(CX, CY)); };
			const int32 CX = FMath::FloorToInt(static_cast<double>(VX) / ComponentQuads), CY = FMath::FloorToInt(static_cast<double>(VY) / ComponentQuads);
			// A vertex on a seam belongs to the components on both sides: any of them will do.
			Valid[Y * W + X] = Has(CX, CY) || (VX % ComponentQuads == 0 && Has(CX - 1, CY)) || (VY % ComponentQuads == 0 && Has(CX, CY - 1))
				|| (VX % ComponentQuads == 0 && VY % ComponentQuads == 0 && Has(CX - 1, CY - 1));
		}
	}
	// Inside the circle the ground is unknown; around it, fixed. The fill is the smoothest surface (Laplace's equation, solved by
	// over-relaxation) that meets the ground at the circle's edge: a plane or a hill's flank stays one, a spike or pit is gone.
	TArray<double> Fill;
	Fill.SetNumUninitialized(W * H);
	TBitArray<> Free(false, W * H);
	// Start from the plane through the ring (fewer iterations than from a constant).
	double Sxx = 0, Sxy = 0, Syy = 0, Sx = 0, Sy = 0, Sz = 0, Sxz = 0, Syz = 0, N = 0;
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const int32 I = Y * W + X;
			Fill[I] = Ground[I];
			if (!Valid[I])
				continue;
			const double Dx = R.Min.X + X - Area.Center.X, Dy = R.Min.Y + Y - Area.Center.Y;
			const double D = FMath::Sqrt(Dx * Dx + Dy * Dy);
			// Free: inside, not on the rectangle's border, and all four neighbours on loaded ground.
			const bool bInside = D <= Inner && X > 0 && Y > 0 && X < W - 1 && Y < H - 1
				&& Valid[I - 1] && Valid[I + 1] && Valid[I - W] && Valid[I + W];
			Free[I] = bInside;
			if (!bInside && D <= Inner + 3.0)
			{
				Sxx += Dx * Dx; Sxy += Dx * Dy; Syy += Dy * Dy; Sx += Dx; Sy += Dy; Sz += Ground[I]; Sxz += Dx * Ground[I]; Syz += Dy * Ground[I]; N += 1;
			}
		}
	}
	if (N < 3)
		return UnrealMcpRql::Failed<int32>(TEXT("the circle has no ground around it on the landscape: move Center onto it or make Radius smaller"));
	{
		// Least squares z = A + B dx + C dy (normal equations, Cramer's rule); a degenerate ring falls back to its mean.
		const double M[3][3] = { { N, Sx, Sy }, { Sx, Sxx, Sxy }, { Sy, Sxy, Syy } };
		const double V[3] = { Sz, Sxz, Syz };
		const auto Det = [](const double A[3][3]) { return A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]); };
		const double D0 = Det(M);
		double Coef[3] = { Sz / N, 0.0, 0.0 };
		if (FMath::Abs(D0) > UE_SMALL_NUMBER)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				double Mc[3][3];
				for (int32 Row = 0; Row < 3; ++Row)
					for (int32 K = 0; K < 3; ++K)
						Mc[Row][K] = K == Col ? V[Row] : M[Row][K];
				Coef[Col] = Det(Mc) / D0;
			}
		}
		for (int32 Y = 0; Y < H; ++Y)
			for (int32 X = 0; X < W; ++X)
				if (Free[Y * W + X])
					Fill[Y * W + X] = Coef[0] + Coef[1] * (R.Min.X + X - Area.Center.X) + Coef[2] * (R.Min.Y + Y - Area.Center.Y);
	}
	const double Omega = 2.0 / (1.0 + PI / FMath::Max(W, H)); // near-optimal over-relaxation for the square
	const double Deadline = FPlatformTime::Seconds() + 20.0;
	bool bConverged = false;
	while (!bConverged && FPlatformTime::Seconds() < Deadline)
	{
		double MaxStep = 0.0;
		for (int32 Y = 1; Y < H - 1; ++Y)
		{
			for (int32 X = 1; X < W - 1; ++X)
			{
				const int32 I = Y * W + X;
				if (!Free[I])
					continue;
				const double Step = Omega * ((Fill[I - 1] + Fill[I + 1] + Fill[I - W] + Fill[I + W]) * 0.25 - Fill[I]);
				Fill[I] += Step;
				MaxStep = FMath::Max(MaxStep, FMath::Abs(Step));
			}
		}
		bConverged = MaxStep < 0.02; // height units (~0.02 cm at the default Z scale)
	}
	if (!bConverged)
		return UnrealMcpRql::Failed<int32>(TEXT("the smooth surface did not settle in time: nothing was changed; smooth a smaller Radius"));
	TArray<int32> Delta;
	Delta.SetNumZeroed(W * H);
	int32 Changed = 0;
	int32 Low = TNumericLimits<int32>::Max(), High = TNumericLimits<int32>::Lowest();
	for (int32 I = 0; I < Ground.Num(); ++I)
	{
		if (!Free[I])
			continue;
		const int32 New = FMath::RoundToInt(FMath::Lerp(static_cast<double>(Ground[I]), Fill[I], Strength));
		Low = FMath::Min(Low, New);
		High = FMath::Max(High, New);
		Delta[I] = New - Ground[I];
		Changed += Delta[I] != 0 ? 1 : 0;
	}
	if (Changed == 0)
		return 0;
	if (!FitsHeightRange(Landscape, UnitsToCm(Landscape, Low), UnitsToCm(Landscape, High)))
		return 0;
	FHeightmapAccessor<false> Accessor(Area.Info);
	Accessor.SetEditLayer(EditLayer);
	TArray<uint16> Data;
	Data.SetNumZeroed(W * H);
	Accessor.GetDataFast(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Data.GetData());
	for (int32 I = 0; I < Data.Num(); ++I)
		Data[I] = static_cast<uint16>(FMath::Clamp(static_cast<int32>(Data[I]) + Delta[I], 0, 65535));
	Landscape->Modify();
	WriteData(Accessor, R, Data.GetData());
	Accessor.Flush();
	MergeNow(Landscape, ELandscapeLayerUpdateMode::Update_Heightmap_All);
	return Changed;
}

int32 UUnrealAILandscapeRqlApi::Ramp(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Falloff, double MaxSlope)
{
	if (!Landscape)
		return UnrealMcpRql::Failed<int32>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
	if (Points.Num() < 2 || Points.Num() > 1000)
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("Ramp needs 2 .. 1000 points, got %d"), Points.Num()));
	if (Points.ContainsByPredicate([](const FVector& Point) { return Point.ContainsNaN(); }))
		return UnrealMcpRql::Failed<int32>(TEXT("Points must be finite numbers"));
	if (!(Width >= 100.0 && Width <= 20000.0))
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("Width %g is outside 100 .. 20000 cm"), Width));
	if (!(Falloff >= 0.0 && Falloff <= 20000.0))
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("Falloff %g is outside 0 .. 20000 cm"), Falloff));
	if (!(MaxSlope >= 0.0 && MaxSlope < 90.0))
		return UnrealMcpRql::Failed<int32>(TEXT("MaxSlope must be within 0 (no limit) .. 90 degrees"));
	if (MaxSlope > 0.0)
	{
		const double Limit = FMath::Tan(FMath::DegreesToRadians(MaxSlope));
		for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
		{
			const FVector& A = Points[Index];
			const FVector& B = Points[Index + 1];
			const double Run = FVector2D::Distance(FVector2D(A), FVector2D(B));
			const double Rise = FMath::Abs(B.Z - A.Z);
			if (Rise > Run * Limit + 0.5)
				return UnrealMcpRql::Failed<int32>(FString::Printf(
					TEXT("from point %d [%.0f, %.0f, %.0f] to point %d [%.0f, %.0f, %.0f] the ramp climbs %.1f degrees, more than MaxSlope %g: %.0f cm of rise needs %.0f cm of length at that slope (this stretch is %.0f): nothing was changed. Add points so the path winds (switchbacks, around a hill) or change the Z"),
					Index, A.X, A.Y, A.Z, Index + 1, B.X, B.Y, B.Z, FMath::RadiansToDegrees(FMath::Atan2(Rise, Run)), MaxSlope, Rise, Rise / Limit, Run));
		}
	}
	return RampAlong(Landscape, Points, Width, Falloff);
}

TArray<FUnrealAILandscapeBump> UUnrealAILandscapeRqlApi::Bumps(ALandscapeProxy* Landscape, double MinHeight, double Scale)
{
	using FBumps = TArray<FUnrealAILandscapeBump>;
	if (!Landscape)
		return UnrealMcpRql::Failed<FBumps>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
	if (!(MinHeight > 0.0) || !(Scale >= 500.0 && Scale <= 50000.0))
		return UnrealMcpRql::Failed<FBumps>(TEXT("MinHeight must be greater than 0 cm and Scale within 500 .. 50000 cm"));
	if (!LoadLandscape(Landscape))
		return {};
	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	FIntRect Whole;
	if (!Info || !Info->GetLandscapeExtent(Whole))
		return UnrealMcpRql::Failed<FBumps>(FString::Printf(TEXT("%s has no landscape components"), *Landscape->GetActorNameOrLabel()));
	const FTransform ToWorld = Landscape->LandscapeActorToWorld();
	const double Quad = FMath::Max(FMath::Abs(ToWorld.GetScale3D().X), UE_KINDA_SMALL_NUMBER);
	const double ComponentQuads = FMath::Max(1, Info->ComponentSizeQuads);
	// The ground as collision and Surface read it (not the edit layers: reading those can dirty the landscape), on a grid
	// of at most about a million samples.
	const int64 Vertices = static_cast<int64>(Whole.Width() + 1) * (Whole.Height() + 1);
	const int32 Step = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(Vertices / 1.0e6)));
	const auto GroundAt = [&](double X, double Y) -> TOptional<float>
	{
		// Inside the component that owns the vertex: on a seam the engine reads the vertex before it.
		const auto Inside = [ComponentQuads](double V, double Min, double Max)
		{
			const double Into = V - FMath::FloorToDouble(V / ComponentQuads) * ComponentQuads;
			return FMath::Clamp(Into < 1e-3 ? V + 1e-3 : V, Min, Max - 1e-3);
		};
		const FVector At = ToWorld.TransformPosition(FVector(Inside(X, Whole.Min.X, Whole.Max.X), Inside(Y, Whole.Min.Y, Whole.Max.Y), 0.0));
		const TOptional<float> Editor = Landscape->GetHeightAtLocation(At, EHeightfieldSource::Editor);
		return Editor.IsSet() ? Editor : Landscape->GetHeightAtLocation(At, EHeightfieldSource::Complex);
	};
	// Sample I at vertex Min + I * Step, the last one on the far edge (Width need not be a multiple of Step).
	const int32 GW = FMath::DivideAndRoundUp(Whole.Width(), Step) + 1, GH = FMath::DivideAndRoundUp(Whole.Height(), Step) + 1;
	const auto VertexX = [&Whole, Step](int32 I) { return FMath::Min(Whole.Min.X + I * Step, Whole.Max.X); };
	const auto VertexY = [&Whole, Step](int32 J) { return FMath::Min(Whole.Min.Y + J * Step, Whole.Max.Y); };
	TArray<double> Z;
	Z.SetNumZeroed(GW * GH);
	TBitArray<> Valid(false, GW * GH);
	for (int32 J = 0; J < GH; ++J)
	{
		for (int32 I = 0; I < GW; ++I)
		{
			if (const TOptional<float> H = GroundAt(VertexX(I), VertexY(J)))
			{
				Z[J * GW + I] = H.GetValue();
				Valid[J * GW + I] = true;
			}
		}
	}
	// The plane that fits the ground within Scale around each sample (least squares over a box, from summed-area tables of
	// 1, i, j, i^2, ij, j^2, z, iz, jz): the residual is how far the ground stands out of that plane. A slope, or a box cut
	// off by the landscape's edge, then gives no residual; only a place that rises or sinks out of its surroundings does.
	const int32 B = FMath::Max(1, FMath::RoundToInt(Scale / (Quad * Step)));
	enum { C1, CI, CJ, CII, CIJ, CJJ, CZ, CIZ, CJZ, CN };
	TArray<double> Sat[CN];
	for (TArray<double>& T : Sat)
		T.SetNumZeroed((GW + 1) * (GH + 1));
	for (int32 J = 0; J < GH; ++J)
	{
		for (int32 I = 0; I < GW; ++I)
		{
			const int32 K = (J + 1) * (GW + 1) + (I + 1);
			const bool bValid = Valid[J * GW + I];
			const double Zv = Z[J * GW + I];
			const double Values[CN] = { 1.0, double(I), double(J), double(I) * I, double(I) * J, double(J) * J, Zv, I * Zv, J * Zv };
			for (int32 C = 0; C < CN; ++C)
				Sat[C][K] = (bValid ? Values[C] : 0.0) + Sat[C][K - 1] + Sat[C][K - (GW + 1)] - Sat[C][K - (GW + 1) - 1];
		}
	}
	TArray<double> Residual;
	Residual.SetNumZeroed(GW * GH);
	for (int32 J = 0; J < GH; ++J)
	{
		for (int32 I = 0; I < GW; ++I)
		{
			if (!Valid[J * GW + I])
				continue;
			const int32 I0 = FMath::Max(0, I - B), J0 = FMath::Max(0, J - B), I1 = FMath::Min(GW, I + B + 1), J1 = FMath::Min(GH, J + B + 1);
			double S[CN];
			for (int32 C = 0; C < CN; ++C)
				S[C] = Sat[C][J1 * (GW + 1) + I1] - Sat[C][J0 * (GW + 1) + I1] - Sat[C][J1 * (GW + 1) + I0] + Sat[C][J0 * (GW + 1) + I0];
			const double N = S[C1];
			if (N < 3.0)
				continue;
			// Centred on this sample: u = i - I, v = j - J; fit z = A + Bu u + Bv v, the plane's height here is A.
			const double Su = S[CI] - N * I, Sv = S[CJ] - N * J;
			const double Suu = S[CII] - 2.0 * I * S[CI] + N * I * I, Svv = S[CJJ] - 2.0 * J * S[CJ] + N * J * J;
			const double Suv = S[CIJ] - J * S[CI] - I * S[CJ] + N * I * J;
			const double Sz = S[CZ], Suz = S[CIZ] - I * S[CZ], Svz = S[CJZ] - J * S[CZ];
			const double M[3][3] = { { N, Su, Sv }, { Su, Suu, Suv }, { Sv, Suv, Svv } };
			const double Det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
			double A = Sz / N;
			if (FMath::Abs(Det) > UE_SMALL_NUMBER)
			{
				const double DetA = Sz * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (Suz * M[2][2] - M[1][2] * Svz) + M[0][2] * (Suz * M[2][1] - M[1][1] * Svz);
				A = DetA / Det;
			}
			Residual[J * GW + I] = Z[J * GW + I] - A;
		}
	}
	// Connected places (8 neighbours) where the ground stands MinHeight above (or below) that plane, one result each.
	FBumps Out;
	TBitArray<> Seen(false, GW * GH);
	TArray<int32> Stack;
	for (int32 Start = 0; Start < GW * GH; ++Start)
	{
		if (Seen[Start] || FMath::Abs(Residual[Start]) < MinHeight)
			continue;
		const double Sign = Residual[Start] > 0.0 ? 1.0 : -1.0;
		int32 Peak = Start;
		TArray<int32> Cells;
		Stack.Reset();
		Stack.Add(Start);
		Seen[Start] = true;
		while (Stack.Num())
		{
			const int32 Cell = Stack.Pop(EAllowShrinking::No);
			Cells.Add(Cell);
			if (Residual[Cell] * Sign > Residual[Peak] * Sign)
				Peak = Cell;
			const int32 CI = Cell % GW, CJ = Cell / GW;
			for (int32 DJ = -1; DJ <= 1; ++DJ)
			{
				for (int32 DI = -1; DI <= 1; ++DI)
				{
					const int32 NI = CI + DI, NJ = CJ + DJ;
					if (NI < 0 || NJ < 0 || NI >= GW || NJ >= GH)
						continue;
					const int32 Next = NJ * GW + NI;
					if (!Seen[Next] && Residual[Next] * Sign >= MinHeight)
					{
						Seen[Next] = true;
						Stack.Add(Next);
					}
				}
			}
		}
		const FVector2D PeakCell(Peak % GW, Peak / GW);
		double Reach = 0.0;
		for (const int32 Cell : Cells)
			Reach = FMath::Max(Reach, FVector2D::Distance(PeakCell, FVector2D(Cell % GW, Cell / GW)));
		// The extreme vertex near the coarse peak.
		FVector2D Best(VertexX(Peak % GW), VertexY(Peak / GW));
		double BestZ = Z[Peak];
		for (int32 DJ = -Step; DJ <= Step; ++DJ)
		{
			for (int32 DI = -Step; DI <= Step; ++DI)
			{
				const FVector2D V(Best.X + DI, Best.Y + DJ);
				if (V.X < Whole.Min.X || V.Y < Whole.Min.Y || V.X > Whole.Max.X || V.Y > Whole.Max.Y)
					continue;
				if (const TOptional<float> H = GroundAt(V.X, V.Y); H.IsSet() && (H.GetValue() - BestZ) * Sign > 0.0)
				{
					BestZ = H.GetValue();
					Best = V;
				}
			}
		}
		FUnrealAILandscapeBump& Bump = Out.AddDefaulted_GetRef();
		Bump.Kind = Sign > 0.0 ? TEXT("Bump") : TEXT("Pit");
		const FVector World = ToWorld.TransformPosition(FVector(Best.X, Best.Y, 0.0));
		Bump.Location = FVector(FMath::RoundToDouble(World.X), FMath::RoundToDouble(World.Y), FMath::RoundToDouble(BestZ * 10.0) / 10.0);
		Bump.Height = FMath::RoundToDouble((Residual[Peak] + (BestZ - Z[Peak])) * 10.0) / 10.0;
		Bump.Radius = FMath::RoundToDouble((Reach + 1.0) * Step * Quad);
	}
	Out.Sort([](const FUnrealAILandscapeBump& A, const FUnrealAILandscapeBump& B) { return FMath::Abs(A.Height) > FMath::Abs(B.Height); });
	if (Out.Num() > 50)
		Out.SetNum(50);
	return Out;
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
	if (Point.ContainsNaN())
		return UnrealMcpRql::Failed<FUnrealAILandscapeSurface>(TEXT("Point must be finite numbers"));
	const FBox2D Near(FVector2D(Point) - FVector2D(1000.0), FVector2D(Point) + FVector2D(1000.0));
	if (!LoadLandscape(Landscape, &Near))
		return Out;
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

int32 UUnrealAILandscapeRqlApi::Delete(ALandscapeProxy* Landscape)
{
	ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
	if (!Primary)
		return UnrealMcpRql::Failed<int32>(TEXT("Landscape is null or its landscape actor is not loaded: pass unreal.find(\"<label>\")"));
	UWorld* World = Primary->GetWorld();
	if (!LoadLandscape(Primary)) // the proxies of unloaded regions too: deleting only the loaded ones would orphan the rest
		return 0;
	TArray<ALandscapeProxy*> Proxies;
	if (ULandscapeInfo* Info = Primary->GetLandscapeInfo())
	{
		Info->ForEachLandscapeProxy([Primary, &Proxies](ALandscapeProxy* Proxy)
		{
			if (Proxy != Primary)
				Proxies.Add(Proxy);
			return true;
		});
	}
	Proxies.Add(Primary); // last: the engine refuses a landscape actor whose proxies remain
	int32 Deleted = 0;
	for (ALandscapeProxy* Proxy : Proxies)
		Deleted += World && World->EditorDestroyActor(Proxy, /*bShouldModifyLevel*/ true) ? 1 : 0;
	return Deleted;
}

int32 UUnrealAILandscapeRqlApi::Rebuild(ALandscapeProxy* Landscape)
{
	ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
	ULandscapeInfo* Info = Primary ? Primary->GetLandscapeInfo() : nullptr;
	if (!Info)
		return UnrealMcpRql::Failed<int32>(TEXT("the landscape has no primary landscape actor to rebuild"));
	if (!FApp::CanEverRender())
		return UnrealMcpRql::Failed<int32>(TEXT("this editor does not render (-nullrhi): a landscape merges its edit layers only in a rendering editor"));
	if (!LoadLandscape(Primary))
		return 0;
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
