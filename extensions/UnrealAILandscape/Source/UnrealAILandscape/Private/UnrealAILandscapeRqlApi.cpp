// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAILandscapeRqlApi.h"
#include "UnrealAILandscapeEdit.h"

#include "IUnrealMcpRqlModule.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeComponent.h"
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

	/**
	 * Load the World Partition actors of the editor world's descs @p Wanted accepts (only unloaded ones are offered), as the
	 * editor's region loading does, through the core loader (it skips actors deleted in this session and adds no undo step
	 * and no unsaved change); false after Fail when the engine cannot load them.
	 */
	bool LoadDescs(UClass* Class, TFunctionRef<bool(const FWorldPartitionActorDescInstance*)> Wanted)
	{
		UWorld* World = UnrealMcpRql::EditorWorld();
		UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
		if (!Partition)
			return true;
		TArray<FGuid> ToLoad;
		FWorldPartitionHelpers::ForEachActorDescInstance(Partition, Class, [&](const FWorldPartitionActorDescInstance* Instance)
		{
			if (!Instance->IsLoaded() && Wanted(Instance))
				ToLoad.Add(Instance->GetGuid());
			return true;
		});
		FString Error;
		return UnrealMcpRql::LoadWorldPartitionActors(ToLoad, &Error) || UnrealMcpRql::Failed<bool>(Error);
	}

	bool LoadLandscape(ALandscapeProxy* Landscape, const FBox2D* Area)
	{
		// LoadActors loads into the editor world only (another world, e.g. play in editor, streams by itself).
		if (!Landscape || Landscape->GetWorld() != UnrealMcpRql::EditorWorld())
			return true;
		const FGuid Guid = Landscape->GetLandscapeGuid();
		return LoadDescs(ALandscapeStreamingProxy::StaticClass(), [&Guid, Area](const FWorldPartitionActorDescInstance* Instance)
		{
			if (static_cast<const FPartitionActorDesc*>(Instance->GetActorDesc())->GridGuid != Guid)
				return false;
			const FBox Box = Instance->GetEditorBounds();
			return !Area || FBox2D(FVector2D(Box.Min), FVector2D(Box.Max)).Intersect(*Area);
		});
	}

	void LoadAll(UClass* Class)
	{
		// Listing a source has no call to fail: the reason goes to the log, the loaded actors are still listed.
		if (!LoadDescs(Class, [](const FWorldPartitionActorDescInstance*) { return true; }))
			UE_LOG(LogUnrealAILandscape, Warning, TEXT("could not load the World Partition actors of class %s: only the loaded ones are listed"), *GetNameSafe(Class));
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
	 * The ground's heights over @p R (row by row): with one height edit layer that layer (@p EditLayer, the one edits are
	 * written to), with several their sum (the merged heights).
	 */
	TArray<uint16> ReadGround(ALandscapeProxy* Landscape, ULandscapeInfo* Info, const FGuid& EditLayer, const FIntRect& R)
	{
		int32 HeightLayers = 0;
		if (const ALandscape* Primary = Landscape->GetLandscapeActor())
		{
			for (const ULandscapeEditLayerBase* Layer : Primary->GetEditLayersConst())
				HeightLayers += Layer && Layer->SupportsTargetType(ELandscapeToolTargetType::Heightmap) ? 1 : 0;
		}
		TArray<uint16> Ground;
		Ground.SetNumZeroed((R.Width() + 1) * (R.Height() + 1));
		FHeightmapAccessor<false> Reader(Info);
		if (HeightLayers <= 1)
			Reader.SetEditLayer(EditLayer);
		Reader.GetDataFast(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Ground.GetData());
		return Ground;
	}

	/** Which vertices of @p R (row by row) have a loaded component under them (the rest of a ReadGround reads 0, not ground). */
	TBitArray<> GroundVertices(const ULandscapeInfo* Info, const FIntRect& R)
	{
		const int32 W = R.Width() + 1, H = R.Height() + 1;
		TBitArray<> Valid(false, W * H);
		const int32 ComponentQuads = Info->ComponentSizeQuads;
		const auto Has = [Info](int32 CX, int32 CY) { return Info->XYtoComponentMap.Contains(FIntPoint(CX, CY)); };
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const int32 VX = R.Min.X + X, VY = R.Min.Y + Y;
				const int32 CX = FMath::FloorToInt(static_cast<double>(VX) / ComponentQuads), CY = FMath::FloorToInt(static_cast<double>(VY) / ComponentQuads);
				// A vertex on a seam belongs to the components on both sides: any of them will do.
				Valid[Y * W + X] = Has(CX, CY) || (VX % ComponentQuads == 0 && Has(CX - 1, CY)) || (VY % ComponentQuads == 0 && Has(CX, CY - 1))
					|| (VX % ComponentQuads == 0 && VY % ComponentQuads == 0 && Has(CX - 1, CY - 1));
			}
		}
		return Valid;
	}

	/**
	 * Read the vertices of @p R through @p Accessor (heights or one layer's weights, on the first edit layer), let @p Edit
	 * change each one given its vertex X, Y (true when it changed), let @p BeforeWrite check the new values of the rectangle
	 * (false after Fail: nothing is written) or adjust what goes with them, write them back and refresh; how many changed.
	 */
	template <typename T, typename TAccessor>
	int32 EditRect(ALandscapeProxy* Landscape, TAccessor& Accessor, const FIntRect& R, TFunctionRef<bool(T&, int32, int32)> Edit,
		TFunctionRef<bool(const FGuid&, const FIntRect&, const TArray<T>&)> BeforeWrite)
	{
		const FGuid EditLayer = EditLayerGuid(Landscape, std::is_same_v<T, uint16> ? ELandscapeToolTargetType::Heightmap : ELandscapeToolTargetType::Weightmap);
		if (!EditLayer.IsValid())
			return 0;
		Accessor.SetEditLayer(EditLayer);
		const int32 Width = R.Max.X - R.Min.X + 1;
		TArray<T> Data;
		Data.SetNumZeroed(Width * (R.Max.Y - R.Min.Y + 1));
		Accessor.GetDataFast(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Data.GetData());
		int32 Changed = 0;
		for (int32 Y = R.Min.Y; Y <= R.Max.Y; ++Y)
		{
			for (int32 X = R.Min.X; X <= R.Max.X; ++X)
				Changed += Edit(Data[(Y - R.Min.Y) * Width + (X - R.Min.X)], X, Y) ? 1 : 0;
		}
		if (Changed == 0 || !BeforeWrite(EditLayer, R, Data))
			return 0;
		Landscape->Modify();
		WriteData(Accessor, R, Data.GetData()); // requests the in-stroke merge of the components it touched
		Accessor.Flush();
		MergeNow(Landscape, std::is_same_v<T, uint16> ? ELandscapeLayerUpdateMode::Update_Heightmap_All : ELandscapeLayerUpdateMode::Update_Weightmap_All);
		return Changed;
	}

	/** EditRect over a circle's rectangle, @p Edit given each vertex's distance from the centre in vertices. */
	template <typename T, typename TAccessor>
	int32 EditCircle(ALandscapeProxy* Landscape, TAccessor& Accessor, const FCircle& Circle, TFunctionRef<bool(T&, double)> Edit,
		TFunctionRef<bool(const FGuid&, const FIntRect&, const TArray<T>&)> BeforeWrite = [](const FGuid&, const FIntRect&, const TArray<T>&) { return true; })
	{
		return EditRect<T>(Landscape, Accessor, Circle.Rect, [&Circle, &Edit](T& Value, int32 X, int32 Y)
		{
			return Edit(Value, FVector2D::Distance(FVector2D(X, Y), Circle.Center));
		}, BeforeWrite);
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

	/** The plane z = C[0] + C[1] u + C[2] v that fits points in least squares, from its normal equations (sums of 1, u, v, uu, uv, vv, z, uz, vz); the mean when degenerate. */
	void FitPlane(double N, double Su, double Sv, double Suu, double Suv, double Svv, double Sz, double Suz, double Svz, double C[3])
	{
		const double M[3][3] = { { N, Su, Sv }, { Su, Suu, Suv }, { Sv, Suv, Svv } };
		const double V[3] = { Sz, Suz, Svz };
		const auto Det = [](const double A[3][3]) { return A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]); };
		const double D = Det(M);
		C[0] = N > 0.0 ? Sz / N : 0.0;
		C[1] = C[2] = 0.0;
		if (FMath::Abs(D) <= UE_SMALL_NUMBER)
			return;
		for (int32 Col = 0; Col < 3; ++Col)
		{
			double Mc[3][3];
			for (int32 Row = 0; Row < 3; ++Row)
				for (int32 K = 0; K < 3; ++K)
					Mc[Row][K] = K == Col ? V[Row] : M[Row][K];
			C[Col] = Det(Mc) / D;
		}
	}

	/**
	 * The connected regions (8 neighbours) of a W x H sample grid: each starts at a sample @p Starts accepts and grows over the
	 * neighbours @p Joins accepts for that start; @p Visit gets each region's samples once.
	 */
	void ForEachRegion(int32 W, int32 H, TFunctionRef<bool(int32)> Starts, TFunctionRef<bool(int32 Start, int32 Next)> Joins, TFunctionRef<void(const TArray<int32>&)> Visit)
	{
		TBitArray<> Seen(false, W * H);
		TArray<int32> Stack, Cells;
		for (int32 Start = 0; Start < W * H; ++Start)
		{
			if (Seen[Start] || !Starts(Start))
				continue;
			Cells.Reset();
			Stack.Reset();
			Stack.Add(Start);
			Seen[Start] = true;
			while (Stack.Num())
			{
				const int32 Cell = Stack.Pop(EAllowShrinking::No);
				Cells.Add(Cell);
				const int32 CI = Cell % W, CJ = Cell / W;
				for (int32 DJ = -1; DJ <= 1; ++DJ)
				{
					for (int32 DI = -1; DI <= 1; ++DI)
					{
						const int32 NI = CI + DI, NJ = CJ + DJ;
						const int32 Next = NJ * W + NI;
						if (NI >= 0 && NJ >= 0 && NI < W && NJ < H && !Seen[Next] && Joins(Start, Next))
						{
							Seen[Next] = true;
							Stack.Add(Next);
						}
					}
				}
			}
			Visit(Cells);
		}
	}

	/** @p Changed (an operation's changed vertices), after logging the operation when it changed anything. */
	int32 Logged(int32 Changed, ALandscapeProxy* Landscape, const TCHAR* Op, const FString& Label, const TArray<FVector>& Points, double Radius,
		double Width, double Height, const FString& Layer = FString(), const FString& Rule = FString())
	{
		if (Changed > 0)
			RecordEdit(Landscape, Op, Label, Points, Radius, Width, Height, Layer, Rule);
		return Changed;
	}

	const TArray<FUnrealAILandscapeEdit>& EditLog(const ALandscapeProxy* Landscape)
	{
		static const TArray<FUnrealAILandscapeEdit> None;
		const ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
		const UUnrealAILandscapeEditLog* Log = Primary ? Primary->FindComponentByClass<UUnrealAILandscapeEditLog>() : nullptr;
		return Log ? Log->Edits : None;
	}

	TArray<FVector> ThinPath(const TArray<FVector>& Points, double Tolerance)
	{
		if (Points.Num() <= 2)
			return Points;
		TBitArray<> Keep(false, Points.Num());
		Keep[0] = true;
		Keep[Points.Num() - 1] = true;
		// Douglas-Peucker without recursion: keep the farthest point of a span when it is off the chord by more than Tolerance.
		TArray<TPair<int32, int32>> Spans{ { 0, Points.Num() - 1 } };
		while (!Spans.IsEmpty())
		{
			const TPair<int32, int32> Span = Spans.Pop();
			double Farthest = Tolerance;
			int32 At = INDEX_NONE;
			for (int32 Index = Span.Key + 1; Index < Span.Value; ++Index)
			{
				const double Off = FMath::PointDistToSegment(Points[Index], Points[Span.Key], Points[Span.Value]);
				if (Off > Farthest)
				{
					Farthest = Off;
					At = Index;
				}
			}
			if (At == INDEX_NONE)
				continue;
			Keep[At] = true;
			Spans.Add({ Span.Key, At });
			Spans.Add({ At, Span.Value });
		}
		TArray<FVector> Out;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			if (Keep[Index])
				Out.Add(Points[Index]);
		}
		return Out;
	}

	void RecordEdit(ALandscapeProxy* Landscape, const TCHAR* Op, const FString& Label, const TArray<FVector>& Points, double Radius,
		double Width, double Height, const FString& Layer, const FString& Rule)
	{
		ALandscape* Primary = Landscape ? Landscape->GetLandscapeActor() : nullptr;
		if (!Primary)
			return;
		UUnrealAILandscapeEditLog* Log = Primary->FindComponentByClass<UUnrealAILandscapeEditLog>();
		if (!Log)
		{
			// An instance component of the landscape actor: saved with it, undone with the operation that made it.
			Primary->Modify();
			Log = NewObject<UUnrealAILandscapeEditLog>(Primary, UUnrealAILandscapeEditLog::StaticClass(), MakeUniqueObjectName(Primary, UUnrealAILandscapeEditLog::StaticClass(), TEXT("AIEditLog")), RF_Transactional);
			Primary->AddInstanceComponent(Log);
			Log->OnComponentCreated();
			Log->RegisterComponent();
		}
		Log->Modify();
		FUnrealAILandscapeEdit& Edit = Log->Edits.AddDefaulted_GetRef();
		Edit.Op = Op;
		Edit.Label = Label.TrimStartAndEnd();
		// A path (a river is one point per vertex, a road one every few metres) keeps the points that hold it within
		// LogPathTolerance of the real one: a reader of the log sees the path's real corners and slopes.
		TArray<FVector> Kept = ThinPath(Points, LogPathTolerance);
		for (double Tolerance = LogPathTolerance * 2.0; Kept.Num() > MaxLogPathPoints; Tolerance *= 2.0)
			Kept = ThinPath(Points, Tolerance);
		for (const FVector& Point : Kept)
			Edit.Points.Add(FVector(FMath::RoundToDouble(Point.X), FMath::RoundToDouble(Point.Y), FMath::RoundToDouble(Point.Z)));
		Edit.Radius = Radius;
		Edit.Width = Width;
		Edit.Height = Height;
		Edit.Layer = Layer;
		Edit.Rule = Rule;
		Edit.Time = FDateTime::UtcNow().ToIso8601();
		int32 Stored = 0;
		for (const FUnrealAILandscapeEdit& Each : Log->Edits)
			Stored += Each.Points.Num();
		int32 Drop = FMath::Max(0, Log->Edits.Num() - MaxEdits);
		for (int32 Index = 0; Index < Drop; ++Index)
			Stored -= Log->Edits[Index].Points.Num();
		while (Stored > MaxLogPoints && Drop < Log->Edits.Num() - 1)
			Stored -= Log->Edits[Drop++].Points.Num();
		if (Drop > 0)
			Log->Edits.RemoveAt(0, Drop);
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

int32 UUnrealAILandscapeRqlApi::Sculpt(ALandscapeProxy* Landscape, FVector Center, double Radius, double Height, const FString& Label)
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
	const int32 Changed = EditCircle<uint16>(Landscape, Accessor, Area, [Units, &Area, &Low, &High](uint16& Value, double Distance)
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
	return Logged(Changed, Landscape, TEXT("Sculpt"), Label, { FVector(Center.X, Center.Y, 0.0) }, Radius, 0.0, Height);
}

int32 UUnrealAILandscapeRqlApi::Flatten(ALandscapeProxy* Landscape, FVector Center, double Radius, double Height, double Falloff, const FString& Label)
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
	const int32 Changed = EditCircle<uint16>(Landscape, Accessor, Area, [Target, Inner, Band](uint16& Value, double Distance)
	{
		const double Weight = Distance <= Inner ? 1.0 : Band > 0.0 ? Cosine((Distance - Inner) / Band) : 0.0;
		const uint16 New = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(FMath::Lerp(static_cast<double>(Value), Target, Weight)), 0, 65535));
		const bool bChanged = New != Value;
		Value = New;
		return bChanged;
	});
	return Logged(Changed, Landscape, TEXT("Flatten"), Label, { FVector(Center.X, Center.Y, 0.0) }, Radius + Falloff, 0.0, Height);
}

int32 UUnrealAILandscapeRqlApi::Carve(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Depth, const FString& Label)
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
	const int32 Changed = CarveAlong(Landscape, Points, Width, Depth);
	return Logged(Changed, Landscape, TEXT("Carve"), Label, Points, 0.0, Width, Depth);
}

int32 UUnrealAILandscapeRqlApi::Paint(ALandscapeProxy* Landscape, const FString& Layer, FVector Center, double Radius, double Strength, const FString& Label)
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
	const int32 Changed = EditCircle<uint8>(Landscape, Accessor, Area, [Strength, &Area](uint8& Value, double Distance)
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
	return Logged(Changed, Landscape, TEXT("Paint"), Label, { FVector(Center.X, Center.Y, 0.0) }, Radius, 0.0, Strength, Name);
}

int32 UUnrealAILandscapeRqlApi::PaintWhere(ALandscapeProxy* Landscape, const FString& Layer, double MinSlope, double MaxSlope, double MinHeight,
	double MaxHeight, double Strength, const FString& Label, const TArray<double>& Within)
{
	const FString Name = Layer.TrimStartAndEnd();
	if (!ValidLayerName(Name, Layer))
		return 0;
	if (!(Strength > 0.0 && Strength <= 1.0))
		return UnrealMcpRql::Failed<int32>(TEXT("Strength must be within 0 (exclusive) .. 1"));
	if (!(MinSlope >= 0.0 && MaxSlope <= 90.0 && MinSlope <= MaxSlope))
		return UnrealMcpRql::Failed<int32>(TEXT("MinSlope and MaxSlope must be within 0 .. 90 degrees, MinSlope at most MaxSlope"));
	if (!(MinHeight <= MaxHeight))
		return UnrealMcpRql::Failed<int32>(TEXT("MinHeight must be at most MaxHeight (world Z, cm)"));
	if (Within.Num() != 0 && Within.Num() != 4)
		return UnrealMcpRql::Failed<int32>(TEXT("Within is [MinX, MinY, MaxX, MaxY] (world cm), or empty for the whole landscape"));
	if (Within.ContainsByPredicate([](double Value) { return !FMath::IsFinite(Value); }))
		return UnrealMcpRql::Failed<int32>(TEXT("Within must be finite numbers"));
	FCircle Area;
	FBox2D Box(ForceInit);
	if (Within.Num() == 4)
	{
		Box += FVector2D(Within[0], Within[1]);
		Box += FVector2D(Within[2], Within[3]);
		if (!Circle(Landscape, FVector(Box.GetCenter(), 0.0), FMath::Max(Box.GetExtent().Size(), 1.0), Area))
			return 0;
	}
	else if (!Circle(Landscape, FVector::ZeroVector, 0.0, Area))
		return 0;
	const FTransform ToWorld = Landscape->LandscapeActorToWorld();
	FIntRect R = Area.Rect;
	if (Box.bIsValid)
	{
		// The vertex rectangle around the box's four corners (a rotated landscape: each vertex is tested against the box below).
		FBox2D Local(ForceInit);
		for (const FVector2D& Corner : { Box.Min, Box.Max, FVector2D(Box.Min.X, Box.Max.Y), FVector2D(Box.Max.X, Box.Min.Y) })
			Local += FVector2D(ToWorld.InverseTransformPosition(FVector(Corner, 0.0)));
		R.Min.X = FMath::Max(R.Min.X, FMath::FloorToInt(Local.Min.X));
		R.Min.Y = FMath::Max(R.Min.Y, FMath::FloorToInt(Local.Min.Y));
		R.Max.X = FMath::Min(R.Max.X, FMath::CeilToInt(Local.Max.X));
		R.Max.Y = FMath::Min(R.Max.Y, FMath::CeilToInt(Local.Max.Y));
		if (R.Min.X > R.Max.X || R.Min.Y > R.Max.Y)
			return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("Within does not overlap %s"), *Landscape->GetActorNameOrLabel()));
	}
	ULandscapeLayerInfoObject* LayerInfo = LayerInfoFor(Landscape, Area.Info, FName(*Name));
	if (!LayerInfo)
		return 0;

	// The ground over the rectangle and one vertex around it (the slopes at its edge).
	const FGuid HeightLayer = EditLayerGuid(Landscape, ELandscapeToolTargetType::Heightmap);
	if (!HeightLayer.IsValid())
		return 0;
	FIntRect Whole;
	Area.Info->GetLandscapeExtent(Whole);
	const FIntRect G(FIntPoint(FMath::Max(Whole.Min.X, R.Min.X - 1), FMath::Max(Whole.Min.Y, R.Min.Y - 1)),
		FIntPoint(FMath::Min(Whole.Max.X, R.Max.X + 1), FMath::Min(Whole.Max.Y, R.Max.Y + 1)));
	const int32 GW = G.Width() + 1;
	const TArray<uint16> Ground = ReadGround(Landscape, Area.Info, HeightLayer, G);
	const TBitArray<> Valid = GroundVertices(Area.Info, G);
	const FVector Scale = ToWorld.GetScale3D().GetAbs();
	const auto LocalZ = [&](int32 X, int32 Y) { return LandscapeDataAccess::GetLocalHeight(Ground[(Y - G.Min.Y) * GW + (X - G.Min.X)]); };
	// A neighbour in a hole of the landscape (no component) is no ground: the slope is taken one-sided there.
	const auto Inside = [&](int32 X, int32 Y) { return Valid[(Y - G.Min.Y) * GW + (X - G.Min.X)]; };
	// 1 inside [Low, High], a cosine to 0 over Edge past either bound.
	const auto Band = [](double Value, double Low, double High, double Edge)
	{
		return Value < Low ? Cosine((Low - Value) / Edge) : Value > High ? Cosine((Value - High) / Edge) : 1.0;
	};
	TAlphamapAccessor<false> Accessor(Area.Info, LayerInfo);
	const int32 Changed = EditRect<uint8>(Landscape, Accessor, R, [&](uint8& Value, int32 X, int32 Y)
	{
		if (!Inside(X, Y))
			return false;
		const int32 X0 = X > G.Min.X && Inside(X - 1, Y) ? X - 1 : X, X1 = X < G.Max.X && Inside(X + 1, Y) ? X + 1 : X;
		const int32 Y0 = Y > G.Min.Y && Inside(X, Y - 1) ? Y - 1 : Y, Y1 = Y < G.Max.Y && Inside(X, Y + 1) ? Y + 1 : Y;
		const double DzDx = X1 > X0 ? (LocalZ(X1, Y) - LocalZ(X0, Y)) * Scale.Z / ((X1 - X0) * Scale.X) : 0.0;
		const double DzDy = Y1 > Y0 ? (LocalZ(X, Y1) - LocalZ(X, Y0)) * Scale.Z / ((Y1 - Y0) * Scale.Y) : 0.0;
		const double Slope = FMath::RadiansToDegrees(FMath::Atan(FMath::Sqrt(DzDx * DzDx + DzDy * DzDy)));
		const FVector World = ToWorld.TransformPosition(FVector(X, Y, LocalZ(X, Y)));
		if (Box.bIsValid && !Box.IsInsideOrOn(FVector2D(World)))
			return false;
		const double Height = World.Z;
		const double Weight = Strength * Band(Slope, MinSlope, MaxSlope, 2.0) * Band(Height, MinHeight, MaxHeight, 50.0);
		const uint8 Target = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(255.0 * Weight), 0, 255));
		if (Target <= Value)
			return false;
		Value = Target;
		return true;
	}, [&Area, LayerInfo](const FGuid& EditLayer, const FIntRect& Rect, const TArray<uint8>& Weights)
	{
		TakeWeightFromOthers(Area.Info, LayerInfo, EditLayer, Rect, Weights);
		return true;
	});
	const auto Range = [](double Low, double High, double Any, const TCHAR* Unit)
	{
		return Low <= -Any && High >= Any ? FString(TEXT("any")) : FString::Printf(TEXT("%s..%s %s"),
			Low <= -Any ? TEXT("any") : *FString::SanitizeFloat(Low), High >= Any ? TEXT("any") : *FString::SanitizeFloat(High), Unit);
	};
	const FString Rule = FString::Printf(TEXT("slope %s, height %s"),
		*(MinSlope <= 0.0 && MaxSlope >= 90.0 ? FString(TEXT("any")) : FString::Printf(TEXT("%s..%s deg"), *FString::SanitizeFloat(MinSlope), *FString::SanitizeFloat(MaxSlope))),
		*Range(MinHeight, MaxHeight, 1.0e8, TEXT("cm")));
	return Logged(Changed, Landscape, TEXT("PaintWhere"), Label, Box.bIsValid ? TArray<FVector>{ FVector(Box.Min, 0.0), FVector(Box.Max, 0.0) } : TArray<FVector>(),
		0.0, 0.0, Strength, Name, Rule);
}

int32 UUnrealAILandscapeRqlApi::Smooth(ALandscapeProxy* Landscape, FVector Center, double Radius, double Strength, const FString& Label)
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
	// The ground (ReadGround); the change is then added to the layer edits are written to, as Carve and Ramp do.
	TArray<uint16> Ground = ReadGround(Landscape, Area.Info, EditLayer, R);
	// Vertices with a loaded component under them (the rest of the rectangle reads as 0, not ground).
	const TBitArray<> Valid = GroundVertices(Area.Info, R);
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
		double Coef[3];
		FitPlane(N, Sx, Sy, Sxx, Sxy, Syy, Sz, Sxz, Syz, Coef);
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
	RecordEdit(Landscape, TEXT("Smooth"), Label, { FVector(Center.X, Center.Y, 0.0) }, Radius, 0.0, Strength);
	return Changed;
}

int32 UUnrealAILandscapeRqlApi::Ramp(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Falloff, double MaxSlope, const FString& Label)
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
	const int32 Changed = RampAlong(Landscape, Points, Width, Falloff);
	return Logged(Changed, Landscape, TEXT("Ramp"), Label, Points, Falloff, Width, 0.0);
}

FUnrealAILandscapeRoad UUnrealAILandscapeRqlApi::RoadBetween(ALandscapeProxy* Landscape, FVector Start, FVector End, double Width, double MaxSlope, double Falloff, const FString& Label)
{
	if (!Landscape)
		return UnrealMcpRql::Failed<FUnrealAILandscapeRoad>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
	if (Start.ContainsNaN() || End.ContainsNaN())
		return UnrealMcpRql::Failed<FUnrealAILandscapeRoad>(TEXT("Start and End must be finite numbers"));
	if (!(Width >= 100.0 && Width <= 20000.0))
		return UnrealMcpRql::Failed<FUnrealAILandscapeRoad>(FString::Printf(TEXT("Width %g is outside 100 .. 20000 cm"), Width));
	if (!(MaxSlope >= 1.0 && MaxSlope <= 45.0))
		return UnrealMcpRql::Failed<FUnrealAILandscapeRoad>(TEXT("MaxSlope must be within 1 .. 45 degrees"));
	if (!(Falloff >= 0.0 && Falloff <= 20000.0))
		return UnrealMcpRql::Failed<FUnrealAILandscapeRoad>(FString::Printf(TEXT("Falloff %g is outside 0 .. 20000 cm"), Falloff));
	if (FVector2D::Distance(FVector2D(Start), FVector2D(End)) < 1.0)
		return UnrealMcpRql::Failed<FUnrealAILandscapeRoad>(TEXT("Start and End are the same place"));
	FUnrealAILandscapeRoad Out;
	if (!PlanRoad(Landscape, Start, End, Width, MaxSlope, Out.Points, Out.Reached))
		return Out;
	for (int32 Index = 1; Index < Out.Points.Num(); ++Index)
	{
		const double Run = FVector2D::Distance(FVector2D(Out.Points[Index - 1]), FVector2D(Out.Points[Index]));
		Out.LengthMeters += Run / 100.0;
		if (Run > 0.0)
			Out.MaxSlope = FMath::Max(Out.MaxSlope, FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(Out.Points[Index].Z - Out.Points[Index - 1].Z), Run)));
	}
	Out.LengthMeters = FMath::RoundToDouble(Out.LengthMeters * 10.0) / 10.0;
	Out.MaxSlope = FMath::RoundToDouble(Out.MaxSlope * 100.0) / 100.0;
	Out.Changed = Logged(RampAlong(Landscape, Out.Points, Width, Falloff), Landscape, TEXT("RoadBetween"), Label, Out.Points, Falloff, Width, 0.0);
	for (FVector& Point : Out.Points)
		Point = FVector(FMath::RoundToDouble(Point.X), FMath::RoundToDouble(Point.Y), FMath::RoundToDouble(Point.Z));
	return Out;
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
			const double Seam = FMath::RoundToDouble(V / ComponentQuads) * ComponentQuads;
			return FMath::Clamp(FMath::Abs(V - Seam) < 1e-3 ? Seam + 1e-3 : V, Min, Max - 1e-3);
		};
		const auto Read = [Landscape](const FVector& At)
		{
			const TOptional<float> Editor = Landscape->GetHeightAtLocation(At, EHeightfieldSource::Editor);
			return Editor.IsSet() ? Editor : Landscape->GetHeightAtLocation(At, EHeightfieldSource::Complex);
		};
		const TOptional<float> Height = Read(ToWorld.TransformPosition(FVector(Inside(X, Whole.Min.X, Whole.Max.X), Inside(Y, Whole.Min.Y, Whole.Max.Y), 0.0)));
		// No component past the seam (a landscape that is not a full rectangle): the one before it holds the vertex too.
		return Height.IsSet() ? Height : Read(ToWorld.TransformPosition(FVector(FMath::Clamp(X - 1e-3, double(Whole.Min.X), double(Whole.Max.X)), FMath::Clamp(Y - 1e-3, double(Whole.Min.Y), double(Whole.Max.Y)), 0.0)));
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
	// Two passes: the second fits only the samples the first found ordinary, so a spike or pit does not tilt the plane of its
	// own surroundings (which showed as a ghost pit beside a spike, a ghost bump on a pit's rim).
	enum { C1, CI, CJ, CII, CIJ, CJJ, CZ, CIZ, CJZ, CN };
	TArray<double> Residual;
	Residual.SetNumZeroed(GW * GH);
	TBitArray<> Fit = Valid;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		if (Pass == 1)
		{
			for (int32 Index = 0; Index < GW * GH; ++Index)
				Fit[Index] = Valid[Index] && FMath::Abs(Residual[Index]) < MinHeight;
		}
		TArray<double> Sat[CN];
		for (TArray<double>& T : Sat)
			T.SetNumZeroed((GW + 1) * (GH + 1));
		for (int32 J = 0; J < GH; ++J)
		{
			for (int32 I = 0; I < GW; ++I)
			{
				const int32 K = (J + 1) * (GW + 1) + (I + 1);
				const bool bFit = Fit[J * GW + I];
				const double Zv = Z[J * GW + I];
				const double Values[CN] = { 1.0, double(I), double(J), double(I) * I, double(I) * J, double(J) * J, Zv, I * Zv, J * Zv };
				for (int32 C = 0; C < CN; ++C)
					Sat[C][K] = (bFit ? Values[C] : 0.0) + Sat[C][K - 1] + Sat[C][K - (GW + 1)] - Sat[C][K - (GW + 1) - 1];
			}
		}
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
				if (N < 3.0 || (Pass == 1 && N < 0.25 * (I1 - I0) * (J1 - J0)))
					continue; // the second pass keeps the first's residual where too few ordinary samples remain
				// Centred on this sample: u = i - I, v = j - J; fit z = A + Bu u + Bv v, the plane's height here is A.
				const double Su = S[CI] - N * I, Sv = S[CJ] - N * J;
				const double Suu = S[CII] - 2.0 * I * S[CI] + N * I * I, Svv = S[CJJ] - 2.0 * J * S[CJ] + N * J * J;
				const double Suv = S[CIJ] - J * S[CI] - I * S[CJ] + N * I * J;
				const double Sz = S[CZ], Suz = S[CIZ] - I * S[CZ], Svz = S[CJZ] - J * S[CZ];
				double Plane[3];
				FitPlane(N, Su, Sv, Suu, Suv, Svv, Sz, Suz, Svz, Plane);
				Residual[J * GW + I] = Z[J * GW + I] - Plane[0];
			}
		}
	}
	// Connected places (8 neighbours) where the ground stands MinHeight above (or below) that plane, one result each.
	FBumps Out;
	TArray<TArray<int32>> RegionCells; // per result, its samples
	ForEachRegion(GW, GH, [&](int32 Start) { return FMath::Abs(Residual[Start]) >= MinHeight; },
		[&](int32 Start, int32 Next) { return Residual[Next] * (Residual[Start] > 0.0 ? 1.0 : -1.0) >= MinHeight; },
		[&](const TArray<int32>& Cells)
	{
		const double Sign = Residual[Cells[0]] > 0.0 ? 1.0 : -1.0;
		int32 Peak = Cells[0];
		for (const int32 Cell : Cells)
			Peak = Residual[Cell] * Sign > Residual[Peak] * Sign ? Cell : Peak;
		const FVector2D PeakCell(Peak % GW, Peak / GW);
		double Reach = 0.0;
		for (const int32 Cell : Cells)
			Reach = FMath::Max(Reach, FVector2D::Distance(PeakCell, FVector2D(Cell % GW, Cell / GW)));
		// Its long and short extent from the spread of its samples (principal axes; a uniform strip of length L spreads L^2 / 12).
		double MeanI = 0.0, MeanJ = 0.0;
		for (const int32 Cell : Cells)
		{
			MeanI += Cell % GW;
			MeanJ += Cell / GW;
		}
		MeanI /= Cells.Num();
		MeanJ /= Cells.Num();
		double Cii = 0.0, Cij = 0.0, Cjj = 0.0;
		for (const int32 Cell : Cells)
		{
			const double DI = Cell % GW - MeanI, DJ = Cell / GW - MeanJ;
			Cii += DI * DI; Cij += DI * DJ; Cjj += DJ * DJ;
		}
		Cii /= Cells.Num(); Cij /= Cells.Num(); Cjj /= Cells.Num();
		const double Mid = (Cii + Cjj) * 0.5, Spread = FMath::Sqrt(FMath::Square((Cii - Cjj) * 0.5) + Cij * Cij);
		const double Long = FMath::Max(1.0, FMath::Sqrt(12.0 * (Mid + Spread))), Short = FMath::Max(1.0, FMath::Sqrt(12.0 * FMath::Max(0.0, Mid - Spread))); // samples
		const bool bLong = Long > 4.0 * Short;
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
		RegionCells.Add(Cells);
		FUnrealAILandscapeBump& Bump = Out.AddDefaulted_GetRef();
		Bump.Kind = Sign > 0.0 ? (bLong ? TEXT("Ridge") : TEXT("Bump")) : (bLong ? TEXT("Channel") : TEXT("Pit"));
		Bump.Length = FMath::RoundToDouble(Long * Step * Quad);
		const FVector World = ToWorld.TransformPosition(FVector(Best.X, Best.Y, 0.0));
		Bump.Location = FVector(FMath::RoundToDouble(World.X), FMath::RoundToDouble(World.Y), FMath::RoundToDouble(BestZ * 10.0) / 10.0);
		Bump.Height = FMath::RoundToDouble((Residual[Peak] + (BestZ - Z[Peak])) * 10.0) / 10.0;
		Bump.Radius = FMath::RoundToDouble((Reach + 1.0) * Step * Quad);
	});
	TArray<int32> Order;
	for (int32 Index = 0; Index < Out.Num(); ++Index)
		Order.Add(Index);
	Order.Sort([&Out](int32 A, int32 B) { return FMath::Abs(Out[A].Height) > FMath::Abs(Out[B].Height); });
	// The plane fitted beside a big pit stands above it (and beside a big mound below it), so its rim can show as a low
	// result of the opposite kind: one within the taller result's Radius of its samples and under a quarter of its height is
	// that rim, not a place of its own (a real spike next to a pit of similar size stays).
	TArray<int32> Kept;
	for (const int32 Index : Order)
	{
		const FUnrealAILandscapeBump& Bump = Out[Index];
		const bool bRim = Kept.ContainsByPredicate([&](int32 TallerIndex)
		{
			const FUnrealAILandscapeBump& Taller = Out[TallerIndex];
			if ((Taller.Height > 0.0) == (Bump.Height > 0.0) || FMath::Abs(Bump.Height) >= 0.25 * FMath::Abs(Taller.Height))
				return false;
			const double Reach = Taller.Radius / (Quad * Step); // samples
			for (const int32 Cell : RegionCells[Index])
				for (const int32 Other : RegionCells[TallerIndex])
					if (FMath::Square(Cell % GW - Other % GW) + FMath::Square(Cell / GW - Other / GW) <= Reach * Reach)
						return true;
			return false;
		});
		if (!bRim)
			Kept.Add(Index);
		if (Kept.Num() == 50)
			break;
	}
	FBumps Result;
	for (const int32 Index : Kept)
		Result.Add(Out[Index]);
	// Which logged edit made it: the latest height edit whose circle or path reaches the place.
	const TArray<FUnrealAILandscapeEdit>& Edits = EditLog(Landscape);
	for (FUnrealAILandscapeBump& Bump : Result)
	{
		const FVector2D At(Bump.Location);
		for (int32 Index = Edits.Num() - 1; Index >= 0 && Bump.Edit.IsEmpty(); --Index)
		{
			const FUnrealAILandscapeEdit& Edit = Edits[Index];
			if (Edit.Op == TEXT("Paint") || Edit.Points.IsEmpty())
				continue;
			double Distance = TNumericLimits<double>::Max();
			// A circle edit made the place only when it is about its size: a 800 m hill's Sculpt covers a spike in it but did
			// not make it.
			if (Edit.Points.Num() == 1 && Edit.Radius > 0.0 && Edit.Radius <= 3.0 * Bump.Radius)
				Distance = FVector2D::Distance(At, FVector2D(Edit.Points[0])) - Edit.Radius;
			// A path reaches Width / 2 plus its sides: Ramp's falloff (Radius), a carved bed's 1:1 banks (Depth, Height).
			const double Sides = Edit.Radius + ((Edit.Op == TEXT("Carve") || Edit.Op == TEXT("CreateRiverBetween")) ? FMath::Min(Edit.Height, 2.0 * Edit.Width) : 0.0);
			for (int32 P = 0; P + 1 < Edit.Points.Num(); ++P)
			{
				const FVector2D From(Edit.Points[P]), To(Edit.Points[P + 1]);
				const FVector2D Along = To - From;
				const double T = FMath::Clamp(FVector2D::DotProduct(At - From, Along) / FMath::Max(Along.SizeSquared(), UE_SMALL_NUMBER), 0.0, 1.0);
				Distance = FMath::Min(Distance, FVector2D::Distance(At, From + Along * T) - Edit.Width * 0.5 - Sides);
			}
			if (Distance <= Bump.Radius)
				Bump.Edit = Edit.Label.IsEmpty() ? FString::Printf(TEXT("%s #%d"), *Edit.Op, Index) : FString::Printf(TEXT("%s '%s' #%d"), *Edit.Op, *Edit.Label, Index);
		}
	}
	return Result;
}

TArray<FUnrealAILandscapeCoverage> UUnrealAILandscapeRqlApi::Coverage(ALandscapeProxy* Landscape, const FString& Layer, double MinWeight)
{
	using FAreas = TArray<FUnrealAILandscapeCoverage>;
	if (!Landscape)
		return UnrealMcpRql::Failed<FAreas>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
	if (!(MinWeight > 0.0 && MinWeight <= 1.0))
		return UnrealMcpRql::Failed<FAreas>(TEXT("MinWeight must be within 0 (exclusive) .. 1"));
	if (!LoadLandscape(Landscape))
		return {};
	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	FIntRect Whole;
	if (!Info || !Info->GetLandscapeExtent(Whole))
		return UnrealMcpRql::Failed<FAreas>(FString::Printf(TEXT("%s has no landscape components"), *Landscape->GetActorNameOrLabel()));
	const FName Name(*Layer.TrimStartAndEnd());
	ULandscapeLayerInfoObject* LayerInfo = nullptr;
	TArray<FString> Names;
	for (const FLandscapeInfoLayerSettings& Settings : Info->Layers)
	{
		if (!Settings.LayerInfoObj || Settings.LayerInfoObj == ALandscapeProxy::VisibilityLayer)
			continue;
		Names.Add(Settings.GetLayerName().ToString());
		if (Settings.GetLayerName() == Name)
			LayerInfo = Settings.LayerInfoObj;
	}
	if (!LayerInfo)
		return UnrealMcpRql::Failed<FAreas>(FString::Printf(TEXT("%s has no paint layer '%s'; its layers: [%s]"), *Landscape->GetActorNameOrLabel(), *Layer, *FString::Join(Names, TEXT(", "))));
	// The layer's final weights, component by component (one weightmap read each), on a grid of at most about 250 000 samples.
	const FTransform ToWorld = Landscape->LandscapeActorToWorld();
	const double Quad = FMath::Max(FMath::Abs(ToWorld.GetScale3D().X), UE_KINDA_SMALL_NUMBER);
	const int64 Vertices = static_cast<int64>(Whole.Width() + 1) * (Whole.Height() + 1);
	const int32 Step = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(Vertices / 250000.0)));
	// Sample I at vertex Min + I * Step, the last one on the far edge (Width need not be a multiple of Step).
	const int32 GW = FMath::DivideAndRoundUp(Whole.Width(), Step) + 1, GH = FMath::DivideAndRoundUp(Whole.Height(), Step) + 1;
	const auto VertexX = [&Whole, Step](int32 I) { return FMath::Min(Whole.Min.X + I * Step, Whole.Max.X); };
	const auto VertexY = [&Whole, Step](int32 J) { return FMath::Min(Whole.Min.Y + J * Step, Whole.Max.Y); };
	TArray<float> Weight;
	Weight.Init(-1.0f, GW * GH);
	const int32 ComponentQuads = Info->ComponentSizeQuads;
	for (const TPair<FIntPoint, ULandscapeComponent*>& Pair : Info->XYtoComponentMap)
	{
		ULandscapeComponent* Component = Pair.Value;
		if (!Component)
			continue;
		TArray<uint8> Cache;
		const FIntPoint Base = Pair.Key * ComponentQuads;
		for (int32 J = FMath::DivideAndRoundUp(FMath::Max(0, Base.Y - Whole.Min.Y), Step); J < GH && VertexY(J) <= Base.Y + ComponentQuads; ++J)
		{
			for (int32 I = FMath::DivideAndRoundUp(FMath::Max(0, Base.X - Whole.Min.X), Step); I < GW && VertexX(I) <= Base.X + ComponentQuads; ++I)
			{
				const FVector At = ToWorld.TransformPosition(FVector(VertexX(I), VertexY(J), 0.0));
				Weight[J * GW + I] = FMath::Max(Weight[J * GW + I], Component->GetLayerWeightAtLocation(At, LayerInfo, &Cache, false));
				if (Cache.IsEmpty())
					break; // the layer is not painted on this component: the engine leaves the cache empty and would read the texture per sample
			}
			if (Cache.IsEmpty())
				break;
		}
	}
	// Connected areas (8 neighbours) at MinWeight or more.
	FAreas Out;
	ForEachRegion(GW, GH, [&](int32 Start) { return Weight[Start] >= MinWeight; }, [&](int32, int32 Next) { return Weight[Next] >= MinWeight; },
		[&](const TArray<int32>& Cells)
	{
		double SumX = 0.0, SumY = 0.0, SumW = 0.0;
		int32 MinI = GW, MinJ = GH, MaxI = -1, MaxJ = -1;
		for (const int32 Cell : Cells)
		{
			const int32 CI = Cell % GW, CJ = Cell / GW;
			SumX += CI; SumY += CJ; SumW += Weight[Cell];
			MinI = FMath::Min(MinI, CI); MinJ = FMath::Min(MinJ, CJ); MaxI = FMath::Max(MaxI, CI); MaxJ = FMath::Max(MaxJ, CJ);
		}
		const int32 Count = Cells.Num();
		const auto World = [&](double I, double J) { return ToWorld.TransformPosition(FVector(FMath::Min(Whole.Min.X + I * Step, double(Whole.Max.X)), FMath::Min(Whole.Min.Y + J * Step, double(Whole.Max.Y)), 0.0)); };
		FUnrealAILandscapeCoverage& Area = Out.AddDefaulted_GetRef();
		const FVector Middle = World(SumX / Count, SumY / Count);
		TOptional<float> Ground = Landscape->GetHeightAtLocation(Middle, EHeightfieldSource::Editor);
		if (!Ground.IsSet())
			Ground = Landscape->GetHeightAtLocation(Middle, EHeightfieldSource::Complex);
		Area.Location = FVector(FMath::RoundToDouble(Middle.X), FMath::RoundToDouble(Middle.Y), FMath::RoundToDouble(Ground.Get(Middle.Z)));
		// The world box of all four corners (a rotated landscape turns the sample box).
		FBox2D Box(ForceInit);
		for (const FVector& Corner : { World(MinI, MinJ), World(MaxI, MinJ), World(MinI, MaxJ), World(MaxI, MaxJ) })
			Box += FVector2D(Corner);
		Area.Min = FVector(FMath::RoundToDouble(Box.Min.X), FMath::RoundToDouble(Box.Min.Y), 0.0);
		Area.Max = FVector(FMath::RoundToDouble(Box.Max.X), FMath::RoundToDouble(Box.Max.Y), 0.0);
		const FVector Scale = ToWorld.GetScale3D().GetAbs();
		Area.Area = FMath::RoundToDouble(Count * Step * Step * Scale.X * Scale.Y / 10000.0);
		Area.Weight = FMath::RoundToDouble(SumW / Count * 100.0) / 100.0;
	});
	Out.Sort([](const FUnrealAILandscapeCoverage& A, const FUnrealAILandscapeCoverage& B) { return A.Area > B.Area; });
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
	// far edge is nudged inside: the engine looks it up in a component past the last one. A point exactly on the line
	// between two components (every ComponentSizeQuads vertices: on a 2 km landscape every 250 m, X or Y 0, 25000, ...) is
	// nudged into the next one: the engine reads it from the one before and returns the height one vertex back.
	const FTransform ToWorld = Landscape->LandscapeActorToWorld();
	const double ComponentQuads = FMath::Max(1, Landscape->ComponentSizeQuads);
	// The world point the engine reads for X, Y (unset off the landscape).
	const auto InsidePoint = [&ToWorld, &Whole, ComponentQuads](double X, double Y) -> TOptional<FVector>
	{
		FVector Local = ToWorld.InverseTransformPosition(FVector(X, Y, ToWorld.GetLocation().Z));
		constexpr double Edge = 1e-3; // quads
		if (Local.X < Whole.Min.X - Edge || Local.Y < Whole.Min.Y - Edge || Local.X > Whole.Max.X + Edge || Local.Y > Whole.Max.Y + Edge)
			return TOptional<FVector>();
		// Within Edge of a seam on either side (a world coordinate converts to 62.9999999 as easily as to 63): just past it.
		const auto OffSeam = [ComponentQuads](double Value)
		{
			const double Seam = FMath::RoundToDouble(Value / ComponentQuads) * ComponentQuads;
			return FMath::Abs(Value - Seam) < Edge ? Seam + Edge : Value;
		};
		Local.X = FMath::Clamp(OffSeam(Local.X), static_cast<double>(Whole.Min.X), Whole.Max.X - Edge);
		Local.Y = FMath::Clamp(OffSeam(Local.Y), static_cast<double>(Whole.Min.Y), Whole.Max.Y - Edge);
		return ToWorld.TransformPosition(Local);
	};
	const auto HeightAt = [Landscape, &InsidePoint](double X, double Y) -> TOptional<float>
	{
		const TOptional<FVector> Inside = InsidePoint(X, Y);
		if (!Inside.IsSet())
			return TOptional<float>();
		const TOptional<float> Editor = Landscape->GetHeightAtLocation(Inside.GetValue(), EHeightfieldSource::Editor);
		return Editor.IsSet() ? Editor : Landscape->GetHeightAtLocation(Inside.GetValue(), EHeightfieldSource::Complex);
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
	// The paint layers' weights from the final (merged) weightmaps of the component under the point (the point inside it:
	// the engine gives 0 for a point a hair outside, on the landscape's edge).
	const FVector Inside = InsidePoint(Point.X, Point.Y).GetValue();
	const FVector Local = ToWorld.InverseTransformPosition(Inside);
	if (ULandscapeComponent* const* Component = Info->XYtoComponentMap.Find(FIntPoint(FMath::FloorToInt(Local.X / ComponentQuads), FMath::FloorToInt(Local.Y / ComponentQuads))))
	{
		for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
		{
			if (Layer.LayerInfoObj && Layer.LayerInfoObj != ALandscapeProxy::VisibilityLayer)
				Out.Layers.Add(Layer.GetLayerName().ToString(), FMath::RoundToDouble((*Component)->GetLayerWeightAtLocation(Inside, Layer.LayerInfoObj, nullptr, false) * 100.0) / 100.0);
		}
	}
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
