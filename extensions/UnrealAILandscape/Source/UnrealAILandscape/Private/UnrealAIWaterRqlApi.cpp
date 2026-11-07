// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIWaterRqlApi.h"
#include "UnrealAIWaterSafety.h"
#include "UnrealAILandscapeEdit.h"

#include "IUnrealMcpRqlModule.h"

#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInterface.h"
#include "ProjectDescriptor.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyOceanComponent.h"
#include "WaterBodyRiverActor.h"
#include "WaterEditorSettings.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "WaterZoneActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIWater, Log, All);

namespace UnrealAIWaterRqlLocal
{
	/** The water zone of @p World (the first one; the editor keeps one per world), or null. */
	AWaterZone* FindZone(UWorld* World)
	{
		for (TActorIterator<AWaterZone> It(World); It; ++It)
		{
			if (IsValid(*It))
				return *It;
		}
		return nullptr;
	}

	/** The world box of every loaded landscape of @p World (invalid when there is none). */
	FBox LandscapesBounds(UWorld* World)
	{
		FBox Bounds(ForceInit);
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetLandscapeGuid().IsValid())
				Bounds += It->GetCompleteBounds();
		}
		return Bounds;
	}

	/** The material the Water plugin's Place Actors defaults give a new body of @p Body's type (null when unset). */
	UMaterialInterface* DefaultWaterMaterial(const AWaterBody* Body)
	{
		const UWaterEditorSettings* Settings = GetDefault<UWaterEditorSettings>();
		const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		switch (Component ? Component->GetWaterBodyType() : EWaterBodyType::Lake)
		{
		case EWaterBodyType::River: return Settings->WaterBodyRiverDefaults.GetWaterMaterial();
		case EWaterBodyType::Ocean: return Settings->WaterBodyOceanDefaults.GetWaterMaterial();
		case EWaterBodyType::Transition: return Settings->WaterBodyCustomDefaults.GetWaterMaterial();
		default: return Settings->WaterBodyLakeDefaults.GetWaterMaterial();
		}
	}

	/**
	 * Spawn @p Class the way the Place Actors panel does (its actor factory applies the Water defaults: material, waves,
	 * spline, zone fill), labelled @p Label. The body never edits the landscape (bAffectsLandscape is cleared before the
	 * Water editor sees it, so no water brush is created) and any automatic "Water" edit layer is created without the
	 * modal dialog (UnrealAIWaterSafety). Null after Fail.
	 */
	AActor* SpawnWater(UClass* Class, const FVector& Location, const FString& Label)
	{
		UWorld* World = UnrealMcpRql::EditorWorld();
		if (!World)
			return UnrealMcpRql::Failed<AActor*>(TEXT("no editor world"));
		ULevel* Level = World->GetCurrentLevel() ? World->GetCurrentLevel() : World->PersistentLevel.Get();
		const UnrealAIWaterSafety::FWaterSpawnScope Scope(World);

		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transactional;
		UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryForActorClass(Class) : nullptr;
		AActor* Actor = Factory
			? Factory->CreateActor(Class, Level, FTransform(Location), Params) // the class is the placed "asset", as in Place Actors
			: World->SpawnActor(Class, &Location, nullptr, Params);
		if (!Actor)
			return UnrealMcpRql::Failed<AActor*>(FString::Printf(TEXT("could not spawn %s"), *Class->GetName()));
		if (!Factory)
			UE_LOG(LogUnrealAIWater, Warning, TEXT("%s: no actor factory, spawned without the Water defaults"), *Class->GetName());
		if (!Label.IsEmpty())
			Actor->SetActorLabel(Label);
		return Actor;
	}

	/** A water material (Material Domain Surface with "Used with Water") or null after Fail. */
	bool RequireWaterMaterial(UMaterialInterface* Material)
	{
		if (UnrealAIWaterSafety::IsWaterMaterial(Material))
			return true;
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not a water material (it needs \"Used with Water\"): use /Water/Materials/WaterSurface/Water_Material, or pass no material for the Water default"),
			Material ? *Material->GetPathName() : TEXT("null")));
	}

	/**
	 * Give @p Body @p Material (null: keep the factory's or take the type's default) and rebuild it now: its shape,
	 * collision and water info mesh. The water zone draws a body only when that mesh exists, so building it here, before
	 * the zone rebuilds on the next tick, avoids the Water plugin's "Ensure condition failed: StaticMesh"
	 * (WaterMeshComponent.cpp) that a material set without a rebuild (set_water_material in Python) raises.
	 */
	bool FinishBody(AWaterBody* Body, UMaterialInterface* Material)
	{
		UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		if (!Component)
			return UnrealMcpRql::Failed<bool>(TEXT("the water body has no water body component"));
		if (Material && !RequireWaterMaterial(Material))
			return false;
		UMaterialInterface* Chosen = Material ? Material : (Component->GetWaterMaterial() ? Component->GetWaterMaterial() : DefaultWaterMaterial(Body));
		if (!Chosen)
			return UnrealMcpRql::Failed<bool>(TEXT("the Water plugin has no default water material (Project Settings > Water Editor): pass one, e.g. /Water/Materials/WaterSurface/Water_Material"));
		Body->Modify();
		Component->Modify();
		Component->SetWaterMaterial(Chosen);
		if (UWaterSplineComponent* Spline = Component->GetWaterSpline())
			Spline->UpdateSpline();
		FOnWaterBodyChangedParams Params;
		Params.bShapeOrPositionChanged = true;
		Params.bUserTriggered = true;
		Component->OnWaterBodyChanged(Params); // shape, collision, zone assignment and the water info mesh (render data)
		Component->UpdateWaterBodyRenderData();
		return true;
	}

	/** Width (and depth, when > 0) at every spline point of a river: the Water metadata curves. */
	void SetRiverProfile(AWaterBody* River, double Width, double Depth)
	{
		UWaterBodyComponent* Component = River->GetWaterBodyComponent();
		UWaterSplineMetadata* Metadata = Component ? Component->GetWaterSplineMetadata() : nullptr;
		if (!Metadata)
			return;
		Metadata->Modify();
		for (FInterpCurvePoint<float>& Point : Metadata->RiverWidth.Points)
			Point.OutVal = static_cast<float>(Width);
		if (Depth > 0.0)
		{
			for (FInterpCurvePoint<float>& Point : Metadata->Depth.Points)
				Point.OutVal = static_cast<float>(Depth);
		}
	}

	/** Move @p Body to @p Origin and place @p Points (world) on its spline (stored relative to the actor). */
	void SetSpline(AWaterBody* Body, const FVector& Origin, const TArray<FVector>& Points)
	{
		UWaterSplineComponent* Spline = Body->GetWaterSpline();
		if (!Spline)
			return;
		Body->SetActorLocation(Origin);
		Spline->Modify();
		TArray<FVector> Local;
		Local.Reserve(Points.Num());
		const FTransform ToWorld = Spline->GetComponentTransform();
		for (const FVector& Point : Points)
			Local.Add(ToWorld.InverseTransformPosition(Point));
		Spline->ResetSpline(Local);
	}

	/** The world's ocean, if any (the Water plugin supports one per world). */
	AWaterBodyOcean* FindOcean(UWorld* World)
	{
		for (TActorIterator<AWaterBodyOcean> It(World); It; ++It)
		{
			if (IsValid(*It))
				return *It;
		}
		return nullptr;
	}

	/** Fill the zone with @p Ocean again after the zone moved or grew. */
	void RefitOcean(AWaterBodyOcean* Ocean, const AWaterZone* Zone)
	{
		UWaterBodyOceanComponent* Component = Ocean ? Cast<UWaterBodyOceanComponent>(Ocean->GetWaterBodyComponent()) : nullptr;
		if (!Component || !Zone)
			return;
		Component->Modify();
		Component->SetCollisionExtents(FVector(Zone->GetZoneExtent() / 2.0, Component->GetCollisionExtents().Z));
		Component->FillWaterZoneWithOcean();
	}

	/** Validate a label: water bodies are found again with unreal.find(label). */
	bool ValidLabel(const FString& Label)
	{
		return !Label.TrimStartAndEnd().IsEmpty() || UnrealMcpRql::Failed<bool>(TEXT("give the water body a label (found again with unreal.find(\"<label>\"))"));
	}

	// ---------------------------------------------------------------------------------------------------------------
	// CreateRiverBetween: the lowest path over a landscape and the bed it carves

	/** A landscape's heights (cm, world Z) on its vertex grid, read from the edit layer the operations write to. */
	struct FHeightGrid
	{
		ULandscapeInfo* Info = nullptr;
		FGuid EditLayer;
		FIntRect Rect;      // the whole landscape, in vertices
		FTransform ToWorld; // vertex -> world
		TArray<uint16> Raw;
		double ZScale = 1.0; // cm per height unit
		double ZOrigin = 0.0;

		int32 Width() const { return Rect.Width() + 1; }
		int32 Height() const { return Rect.Height() + 1; }
		double At(int32 X, int32 Y) const { return ZOrigin + (static_cast<double>(Raw[(Y - Rect.Min.Y) * Width() + (X - Rect.Min.X)]) - LandscapeDataAccess::MidValue) * ZScale; }
		uint16 Units(double Cm) const { return static_cast<uint16>(FMath::Clamp(FMath::RoundToInt((Cm - ZOrigin) / ZScale + LandscapeDataAccess::MidValue), 0, 65535)); }
		FVector2D ToLocal(const FVector& World) const { const FVector L = ToWorld.InverseTransformPosition(World); return FVector2D(L.X, L.Y); }
		FVector ToWorldPoint(double X, double Y, double Z) const { FVector P = ToWorld.TransformPosition(FVector(X, Y, 0.0)); P.Z = Z; return P; }
	};

	bool ReadGrid(ALandscapeProxy* Landscape, FHeightGrid& Out)
	{
		Out.Info = Landscape->GetLandscapeInfo();
		if (!Out.Info || !Out.Info->GetLandscapeExtent(Out.Rect))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no landscape components"), *Landscape->GetActorNameOrLabel()));
		Out.EditLayer = UnrealAILandscapeRqlApiLocal::EditLayerGuid(Landscape, ELandscapeToolTargetType::Heightmap);
		if (!Out.EditLayer.IsValid())
			return false;
		Out.ToWorld = Landscape->LandscapeActorToWorld();
		Out.ZScale = Out.ToWorld.GetScale3D().Z * LANDSCAPE_ZSCALE;
		Out.ZOrigin = Out.ToWorld.GetLocation().Z;
		Out.Raw.SetNumZeroed(Out.Width() * Out.Height());
		FHeightmapAccessor<false> Accessor(Out.Info);
		Accessor.SetEditLayer(Out.EditLayer);
		Accessor.GetDataFast(Out.Rect.Min.X, Out.Rect.Min.Y, Out.Rect.Max.X, Out.Rect.Max.Y, Out.Raw.GetData());
		return true;
	}

	/**
	 * The lowest route from @p From to @p To (vertex coordinates) on a coarse grid (at most 400 x 400 cells, so a 2 km
	 * landscape routes in well under a second): Dijkstra over 8 neighbours, each step costing its length weighted by
	 * how high the cell lies and by any climb. Returns vertex-space points, source first.
	 */
	TArray<FVector2D> LowestPath(const FHeightGrid& Grid, const FVector2D& From, const FVector2D& To)
	{
		const int32 Step = FMath::Max(1, FMath::CeilToInt(FMath::Max(Grid.Width(), Grid.Height()) / 400.0));
		const int32 W = (Grid.Width() - 1) / Step + 1;
		const int32 H = (Grid.Height() - 1) / Step + 1;
		TArray<double> Z;
		Z.SetNumUninitialized(W * H);
		double Low = TNumericLimits<double>::Max(), High = TNumericLimits<double>::Lowest();
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const double Value = Grid.At(Grid.Rect.Min.X + FMath::Min(X * Step, Grid.Rect.Width()), Grid.Rect.Min.Y + FMath::Min(Y * Step, Grid.Rect.Height()));
				Z[Y * W + X] = Value;
				Low = FMath::Min(Low, Value);
				High = FMath::Max(High, Value);
			}
		}
		const double Range = FMath::Max(High - Low, 1.0);
		const double Quad = Grid.ToWorld.GetScale3D().X * Step; // cm per cell
		const auto Cell = [&](const FVector2D& P)
		{
			const int32 X = FMath::Clamp(FMath::RoundToInt((P.X - Grid.Rect.Min.X) / Step), 0, W - 1);
			const int32 Y = FMath::Clamp(FMath::RoundToInt((P.Y - Grid.Rect.Min.Y) / Step), 0, H - 1);
			return Y * W + X;
		};
		const int32 Start = Cell(From), Goal = Cell(To);
		TArray<double> Cost;
		Cost.Init(TNumericLimits<double>::Max(), W * H);
		TArray<int32> Previous;
		Previous.Init(INDEX_NONE, W * H);
		struct FNode { double Cost; int32 Index; bool operator<(const FNode& Other) const { return Cost < Other.Cost; } };
		TArray<FNode> Open;
		Cost[Start] = 0.0;
		Open.HeapPush(FNode{ 0.0, Start });
		while (Open.Num())
		{
			FNode Node;
			Open.HeapPop(Node, EAllowShrinking::No);
			if (Node.Index == Goal)
				break;
			if (Node.Cost > Cost[Node.Index])
				continue;
			const int32 X = Node.Index % W, Y = Node.Index / W;
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const int32 NX = X + DX, NY = Y + DY;
					if ((DX == 0 && DY == 0) || NX < 0 || NY < 0 || NX >= W || NY >= H)
						continue;
					const int32 Next = NY * W + NX;
					const double Length = Quad * ((DX != 0 && DY != 0) ? UE_DOUBLE_SQRT_2 : 1.0);
					const double Climb = FMath::Max(0.0, Z[Next] - Z[Node.Index]);
					const double NewCost = Node.Cost + Length * (1.0 + 6.0 * (Z[Next] - Low) / Range) + 20.0 * Climb;
					if (NewCost < Cost[Next])
					{
						Cost[Next] = NewCost;
						Previous[Next] = Node.Index;
						Open.HeapPush(FNode{ NewCost, Next });
					}
				}
			}
		}
		TArray<FVector2D> Path;
		for (int32 Index = Goal; Index != INDEX_NONE; Index = Previous[Index])
		{
			Path.Add(FVector2D(Grid.Rect.Min.X + FMath::Min((Index % W) * Step, Grid.Rect.Width()), Grid.Rect.Min.Y + FMath::Min((Index / W) * Step, Grid.Rect.Height())));
			if (Index == Start)
				break;
		}
		Algo::Reverse(Path);
		Path[0] = From;
		Path.Last() = To;
		return Path;
	}

	/** @p Path resampled every @p Spacing (vertex units) and smoothed (the 8-neighbour grid zigzags), ends kept. */
	TArray<FVector2D> Resample(const TArray<FVector2D>& Path, double Spacing)
	{
		TArray<FVector2D> Out;
		Out.Add(Path[0]);
		double Carry = 0.0;
		for (int32 Index = 1; Index < Path.Num(); ++Index)
		{
			const FVector2D A = Path[Index - 1], B = Path[Index];
			const double Length = FVector2D::Distance(A, B);
			double At = Spacing - Carry;
			while (At < Length)
			{
				Out.Add(FMath::Lerp(A, B, At / Length));
				At += Spacing;
			}
			Carry = Length - (At - Spacing);
		}
		if (FVector2D::Distance(Out.Last(), Path.Last()) < Spacing * 0.5 && Out.Num() > 1)
			Out.Last() = Path.Last();
		else
			Out.Add(Path.Last());
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			TArray<FVector2D> Smooth = Out;
			for (int32 Index = 1; Index + 1 < Out.Num(); ++Index)
				Smooth[Index] = (Out[Index - 1] + Out[Index] * 2.0 + Out[Index + 1]) * 0.25;
			Out = MoveTemp(Smooth);
		}
		return Out;
	}

	/**
	 * Lower the landscape along the river (vertex-space @p Path with bed heights @p Bed, cm): flat bed HalfWidth either
	 * side of the centre line, then banks rising 1:1 (at most 2 x the width) to the original ground; only ever lowers.
	 * One write of the changed rectangle, merged before returning. How many vertices changed.
	 */
	int32 CarveBed(ALandscapeProxy* Landscape, FHeightGrid& Grid, const TArray<FVector2D>& Path, const TArray<double>& Bed, double HalfWidthCm)
	{
		const double Quad = Grid.ToWorld.GetScale3D().X;
		const double Half = HalfWidthCm / Quad;              // vertices
		const double Bank = 4.0 * HalfWidthCm / Quad;        // vertices
		const double Rise = Quad;                            // cm per vertex of bank (1:1)
		FIntRect Changed(TNumericLimits<int32>::Max(), TNumericLimits<int32>::Max(), TNumericLimits<int32>::Lowest(), TNumericLimits<int32>::Lowest());
		TArray<uint16> Data = Grid.Raw;
		int32 Count = 0;
		for (int32 Index = 0; Index + 1 < Path.Num(); ++Index)
		{
			const FVector2D A = Path[Index], B = Path[Index + 1];
			const int32 MinX = FMath::Max(Grid.Rect.Min.X, FMath::FloorToInt(FMath::Min(A.X, B.X) - Half - Bank));
			const int32 MaxX = FMath::Min(Grid.Rect.Max.X, FMath::CeilToInt(FMath::Max(A.X, B.X) + Half + Bank));
			const int32 MinY = FMath::Max(Grid.Rect.Min.Y, FMath::FloorToInt(FMath::Min(A.Y, B.Y) - Half - Bank));
			const int32 MaxY = FMath::Min(Grid.Rect.Max.Y, FMath::CeilToInt(FMath::Max(A.Y, B.Y) + Half + Bank));
			const FVector2D AB = B - A;
			const double LengthSquared = FMath::Max(AB.SizeSquared(), UE_SMALL_NUMBER);
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const FVector2D P(X, Y);
					const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LengthSquared, 0.0, 1.0);
					const double Distance = FVector2D::Distance(P, A + AB * T);
					if (Distance > Half + Bank)
						continue;
					const double Target = FMath::Lerp(Bed[Index], Bed[Index + 1], T) + FMath::Max(0.0, Distance - Half) * Rise;
					uint16& Value = Data[(Y - Grid.Rect.Min.Y) * Grid.Width() + (X - Grid.Rect.Min.X)];
					const uint16 New = Grid.Units(Target);
					if (New < Value)
					{
						Count += Value == Grid.Raw[(Y - Grid.Rect.Min.Y) * Grid.Width() + (X - Grid.Rect.Min.X)] ? 1 : 0;
						Value = New;
						Changed.Min.X = FMath::Min(Changed.Min.X, X);
						Changed.Min.Y = FMath::Min(Changed.Min.Y, Y);
						Changed.Max.X = FMath::Max(Changed.Max.X, X);
						Changed.Max.Y = FMath::Max(Changed.Max.Y, Y);
					}
				}
			}
		}
		if (Count == 0)
			return 0;
		const int32 Width = Changed.Width() + 1;
		TArray<uint16> Block;
		Block.SetNumUninitialized(Width * (Changed.Height() + 1));
		for (int32 Y = Changed.Min.Y; Y <= Changed.Max.Y; ++Y)
			FMemory::Memcpy(&Block[(Y - Changed.Min.Y) * Width], &Data[(Y - Grid.Rect.Min.Y) * Grid.Width() + (Changed.Min.X - Grid.Rect.Min.X)], Width * sizeof(uint16));
		Landscape->Modify();
		FHeightmapAccessor<false> Accessor(Grid.Info);
		Accessor.SetEditLayer(Grid.EditLayer);
		Accessor.SetData(Changed.Min.X, Changed.Min.Y, Changed.Max.X, Changed.Max.Y, Block.GetData());
		Accessor.Flush();
		Grid.Raw = MoveTemp(Data);
		UnrealAILandscapeRqlApiLocal::MergeNow(Landscape, ELandscapeLayerUpdateMode::Update_Heightmap_All);
		return Count;
	}
}

using namespace UnrealAIWaterRqlLocal;

FString UUnrealAIWaterRqlApi::EnableWater()
{
	TArray<FString> Done;
	TSharedPtr<IPlugin> Water = IPluginManager::Get().FindPlugin(TEXT("Water"));
	if (!Water || !Water->IsEnabled())
		return UnrealMcpRql::Failed<FString>(TEXT("the engine's Water plugin is not installed or not enabled: enable it (Edit > Plugins > Water) and restart the editor"));
	TArray<FString> Missing;
	for (const FPluginReferenceDescriptor& Dependency : Water->GetDescriptor().Plugins)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Dependency.Name);
		if (Dependency.bEnabled && (!Plugin || !Plugin->IsEnabled()))
			Missing.Add(Dependency.Name);
	}
	if (Missing.Num())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("plugins the Water plugin needs are not enabled: %s (enable them, restart the editor)"), *FString::Join(Missing, TEXT(", "))));
	const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
	const bool bListed = Project && Project->Plugins.ContainsByPredicate([](const FPluginReferenceDescriptor& Plugin) { return Plugin.Name == TEXT("Water") && Plugin.bEnabled; });
	if (!bListed)
	{
		FText Reason;
		if (!IProjectManager::Get().SetPluginEnabled(TEXT("Water"), true, Reason) || !IProjectManager::Get().SaveCurrentProjectToDisk(Reason))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not enable Water in the project file: %s"), *Reason.ToString()));
		Done.Add(TEXT("Water is now enabled in the project file (packaged games and cooks include the water actors; its plugins Landmass, Niagara, GeometryProcessing and BlueprintMaterialTextureNodes come with it)"));
	}
	else
	{
		Done.Add(TEXT("Water is enabled in the project file"));
	}
	FCollisionResponseTemplate Profile;
	Done.Add(UCollisionProfile::Get()->GetProfileTemplate(TEXT("WaterBodyCollision"), Profile)
		? TEXT("the WaterBodyCollision profile exists")
		: TEXT("the WaterBodyCollision profile is missing: Project Settings > Collision, or Water's \"Add entry to DefaultEngine.ini\" prompt"));
	Done.Add(TEXT("loaded now, no restart needed: water | call CreateOcean(\"Ocean\")"));
	return FString::Join(Done, TEXT("; "));
}

AWaterZone* UUnrealAIWaterRqlApi::SetZone(double SizeMeters, FVector Center)
{
	UWorld* World = UnrealMcpRql::EditorWorld();
	if (!World)
		return UnrealMcpRql::Failed<AWaterZone*>(TEXT("no editor world"));
	double Extent = SizeMeters * 100.0;
	if (SizeMeters <= 0.0)
	{
		const FBox Bounds = LandscapesBounds(World);
		if (!Bounds.IsValid)
			return UnrealMcpRql::Failed<AWaterZone*>(TEXT("there is no landscape to cover: pass SizeMeters (and Center)"));
		Extent = FMath::Max(Bounds.GetSize().X, Bounds.GetSize().Y) * 1.25;
		Center = Bounds.GetCenter();
	}
	else if (SizeMeters < 10.0 || SizeMeters > 100000.0)
	{
		return UnrealMcpRql::Failed<AWaterZone*>(FString::Printf(TEXT("SizeMeters %g is outside 10 .. 100000 (0 covers the landscapes)"), SizeMeters));
	}
	AWaterZone* Zone = FindZone(World);
	if (!Zone)
		Zone = Cast<AWaterZone>(SpawnWater(AWaterZone::StaticClass(), FVector(Center.X, Center.Y, 0.0), TEXT("WaterZone")));
	if (!Zone)
		return nullptr;
	Zone->Modify();
	Zone->SetActorLocation(FVector(Center.X, Center.Y, Zone->GetActorLocation().Z));
	Zone->SetZoneExtent(FVector2D(Extent, Extent));
	RefitOcean(FindOcean(World), Zone);
	return Zone;
}

AWaterBodyOcean* UUnrealAIWaterRqlApi::CreateOcean(const FString& Label, double Height, UMaterialInterface* Material)
{
	UWorld* World = UnrealMcpRql::EditorWorld();
	if (!World)
		return UnrealMcpRql::Failed<AWaterBodyOcean*>(TEXT("no editor world"));
	if (!ValidLabel(Label))
		return nullptr;
	if (!FMath::IsFinite(Height) || FMath::Abs(Height) > 1000000.0)
		return UnrealMcpRql::Failed<AWaterBodyOcean*>(TEXT("Height must be within -1000000 .. 1000000 cm"));
	if (Material && !RequireWaterMaterial(Material))
		return nullptr;
	if (const AWaterBodyOcean* Existing = FindOcean(World))
		return UnrealMcpRql::Failed<AWaterBodyOcean*>(FString::Printf(TEXT("the level already has an ocean (%s; one per world): move it with set Location.Z, or Destroy it first"), *Existing->GetActorLabel()));
	AWaterZone* Zone = FindZone(World);
	if (!Zone)
	{
		Zone = LandscapesBounds(World).IsValid ? SetZone(0.0, FVector::ZeroVector) : SetZone(2000.0, FVector::ZeroVector);
		if (!Zone)
			return nullptr;
	}
	const FVector Location(Zone->GetActorLocation().X, Zone->GetActorLocation().Y, Height);
	AWaterBodyOcean* Ocean = Cast<AWaterBodyOcean>(SpawnWater(AWaterBodyOcean::StaticClass(), Location, Label));
	if (!Ocean)
		return nullptr;
	RefitOcean(Ocean, Zone);
	return FinishBody(Ocean, Material) ? Ocean : nullptr;
}

AWaterBodyLake* UUnrealAIWaterRqlApi::CreateLake(const FString& Label, FVector Center, double Radius, UMaterialInterface* Material)
{
	if (!ValidLabel(Label))
		return nullptr;
	if (!(Radius >= 100.0 && Radius <= 200000.0))
		return UnrealMcpRql::Failed<AWaterBodyLake*>(FString::Printf(TEXT("Radius %g is outside 100 .. 200000 cm"), Radius));
	if (Material && !RequireWaterMaterial(Material))
		return nullptr;
	if (!FindZone(UnrealMcpRql::EditorWorld()) && !SetZone(LandscapesBounds(UnrealMcpRql::EditorWorld()).IsValid ? 0.0 : 2000.0, Center))
		return nullptr;
	AWaterBodyLake* Lake = Cast<AWaterBodyLake>(SpawnWater(AWaterBodyLake::StaticClass(), Center, Label));
	if (!Lake)
		return nullptr;
	TArray<FVector> Shore;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const double Angle = UE_DOUBLE_TWO_PI * Index / 8.0;
		Shore.Add(Center + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0));
	}
	SetSpline(Lake, Center, Shore);
	return FinishBody(Lake, Material) ? Lake : nullptr;
}

AWaterBodyRiver* UUnrealAIWaterRqlApi::CreateRiver(const FString& Label, const TArray<FVector>& Points, double Width, UMaterialInterface* Material)
{
	if (!ValidLabel(Label))
		return nullptr;
	if (Points.Num() < 2 || Points.Num() > 200)
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(FString::Printf(TEXT("a river needs 2 .. 200 points (upstream first), got %d"), Points.Num()));
	if (!(Width >= 100.0 && Width <= 20000.0))
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(FString::Printf(TEXT("Width %g is outside 100 .. 20000 cm"), Width));
	if (Material && !RequireWaterMaterial(Material))
		return nullptr;
	UWorld* World = UnrealMcpRql::EditorWorld();
	if (!FindZone(World) && !SetZone(LandscapesBounds(World).IsValid ? 0.0 : 2000.0, Points[0]))
		return nullptr;
	AWaterBodyRiver* River = Cast<AWaterBodyRiver>(SpawnWater(AWaterBodyRiver::StaticClass(), Points[0], Label));
	if (!River)
		return nullptr;
	SetSpline(River, Points[0], Points);
	SetRiverProfile(River, Width, 0.0);
	return FinishBody(River, Material) ? River : nullptr;
}

AWaterBodyRiver* UUnrealAIWaterRqlApi::CreateRiverBetween(const FString& Label, ALandscapeProxy* Landscape, FVector Source, FVector Mouth, double Width, double Depth, UMaterialInterface* Material)
{
	if (!Landscape)
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
	if (!(Width >= 100.0 && Width <= 20000.0))
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(FString::Printf(TEXT("Width %g is outside 100 .. 20000 cm"), Width));
	if (!(Depth >= 0.0 && Depth <= 10000.0))
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(FString::Printf(TEXT("Depth %g is outside 0 .. 10000 cm"), Depth));
	if (!ValidLabel(Label) || (Material && !RequireWaterMaterial(Material)))
		return nullptr;
	FHeightGrid Grid;
	if (!ReadGrid(Landscape, Grid))
		return nullptr;
	const FVector2D From = Grid.ToLocal(Source), To = Grid.ToLocal(Mouth);
	const auto Inside = [&Grid](const FVector2D& P) { return P.X >= Grid.Rect.Min.X && P.Y >= Grid.Rect.Min.Y && P.X <= Grid.Rect.Max.X && P.Y <= Grid.Rect.Max.Y; };
	if (!Inside(From) || !Inside(To))
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(TEXT("Source and Mouth must be on the landscape (its Bounds): a river mouth at the coast stays a little inside the landscape's edge"));
	if (FVector2D::Distance(From, To) * Grid.ToWorld.GetScale3D().X < Width * 2.0)
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(TEXT("Source and Mouth are too close for a river of this Width"));

	// The route, resampled at most 150 points, every 2 river widths or more.
	const TArray<FVector2D> Route = LowestPath(Grid, From, To);
	double Length = 0.0;
	for (int32 Index = 1; Index < Route.Num(); ++Index)
		Length += FVector2D::Distance(Route[Index - 1], Route[Index]);
	const double Quad = Grid.ToWorld.GetScale3D().X;
	const TArray<FVector2D> Path = Resample(Route, FMath::Max(2.0 * Width / Quad, Length / 149.0));

	// The water surface flows downhill: never above any ground upstream of it. With a bed it sits a third of the depth
	// below the old ground (the bed Depth below that ground), so the water shows even where the route crosses a rise.
	TArray<double> Surface, Bed;
	double Running = TNumericLimits<double>::Max();
	for (const FVector2D& P : Path)
	{
		const int32 X = FMath::Clamp(FMath::RoundToInt(P.X), Grid.Rect.Min.X, Grid.Rect.Max.X);
		const int32 Y = FMath::Clamp(FMath::RoundToInt(P.Y), Grid.Rect.Min.Y, Grid.Rect.Max.Y);
		Running = FMath::Min(Running, Grid.At(X, Y));
		Bed.Add(Running - Depth);
		Surface.Add(Depth > 0.0 ? Running - Depth / 3.0 : Running + 20.0);
	}
	if (Depth > 0.0)
		CarveBed(Landscape, Grid, Path, Bed, Width * 0.5);

	TArray<FVector> Points;
	for (int32 Index = 0; Index < Path.Num(); ++Index)
		Points.Add(Grid.ToWorldPoint(Path[Index].X, Path[Index].Y, Surface[Index]));
	UWorld* World = UnrealMcpRql::EditorWorld();
	if (!FindZone(World) && !SetZone(0.0, FVector::ZeroVector))
		return nullptr;
	AWaterBodyRiver* River = Cast<AWaterBodyRiver>(SpawnWater(AWaterBodyRiver::StaticClass(), Points[0], Label));
	if (!River)
		return nullptr;
	SetSpline(River, Points[0], Points);
	SetRiverProfile(River, Width, Depth > 0.0 ? Depth * 2.0 / 3.0 : 0.0);
	return FinishBody(River, Material) ? River : nullptr;
}

AWaterBody* UUnrealAIWaterRqlApi::SetMaterial(AWaterBody* Body, UMaterialInterface* Material)
{
	if (!Body)
		return UnrealMcpRql::Failed<AWaterBody*>(TEXT("Body is null: pass unreal.find(\"<label>\") or $ from waterBodies"));
	if (!FindZone(Body->GetWorld()) && !SetZone(LandscapesBounds(Body->GetWorld()).IsValid ? 0.0 : 2000.0, Body->GetActorLocation()))
		return nullptr;
	return FinishBody(Body, Material ? Material : DefaultWaterMaterial(Body)) ? Body : nullptr;
}
