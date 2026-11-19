// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIWaterRqlApi.h"
#include "UnrealAIWaterSafety.h"
#include "UnrealAILandscapeEdit.h"

#include "IUnrealMcpRqlModule.h"

#include "ActorFactories/ActorFactory.h"
#include "Algo/Count.h"
#include "Editor.h"
#include "Editor/EditorEngine.h" // FActorLabelUtilities
#include "Engine/CollisionProfile.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
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
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "WaterZoneActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIWater, Log, All);

namespace UnrealAIWaterRqlLocal
{
	/** The first actor of class T in @p World (the water zone and the ocean: one per world), or null. */
	template <typename T>
	T* FindFirst(UWorld* World)
	{
		for (TActorIterator<T> It(World); It; ++It)
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

	/**
	 * Spawn @p Class the way the Place Actors panel does (its actor factory applies the Water defaults: material, waves,
	 * spline, zone fill), labelled @p Label (made unique). The body never edits the landscape (FWaterSpawnScope: no water
	 * brush, no edit layer). Null after Fail.
	 */
	AActor* SpawnWater(UWorld* World, UClass* Class, const FVector& Location, const FString& Label)
	{
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
			FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
		return Actor;
	}

	/** A water material (Material Domain Surface with "Used with Water"), or false after Fail. */
	bool RequireWaterMaterial(UMaterialInterface* Material)
	{
		if (UnrealAIWaterSafety::IsWaterMaterial(Material))
			return true;
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not a water material (it needs \"Used with Water\"): use /Water/Materials/WaterSurface/Water_Material, or pass no material for the Water default"),
			Material ? *Material->GetPathName() : TEXT("null")));
	}

	/**
	 * Give @p Body @p Material (validated by the caller; null keeps the factory's) and rebuild it now: shape, collision and
	 * water info mesh. The water zone draws a body only when that mesh exists, so building it here, before the zone
	 * rebuilds on the next tick, avoids the Water plugin's "Ensure condition failed: StaticMesh" (WaterMeshComponent.cpp)
	 * that a material set without a rebuild (set_water_material in Python) raises.
	 */
	bool FinishBody(AWaterBody* Body, UMaterialInterface* Material)
	{
		UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		if (!Component)
			return UnrealMcpRql::Failed<bool>(TEXT("the water body has no water body component"));
		if (Material)
		{
			Component->Modify();
			Component->SetWaterMaterial(Material);
		}
		if (!Component->GetWaterMaterial())
			return UnrealMcpRql::Failed<bool>(TEXT("the Water plugin has no default water material (Project Settings > Water Editor): pass one, e.g. /Water/Materials/WaterSurface/Water_Material"));
		UnrealAIWaterSafety::Rebuild(Body);
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

	/**
	 * Place @p Points (world) on @p Body's spline, stored relative to the actor. Quietly (no change broadcast, which would
	 * rebuild the body with the defaults' width): FinishBody rebuilds it once, with its final profile.
	 */
	void SetSpline(AWaterBody* Body, const TArray<FVector>& Points)
	{
		UWaterSplineComponent* Spline = Body->GetWaterSpline();
		if (!Spline)
			return;
		Spline->Modify();
		const FTransform ToWorld = Spline->GetComponentTransform();
		Spline->ClearSplinePoints(false);
		for (const FVector& Point : Points)
			Spline->AddSplinePoint(ToWorld.InverseTransformPosition(Point), ESplineCoordinateSpace::Local, false);
		Spline->UpdateSpline();
		Spline->SynchronizeWaterProperties(); // one width / depth metadata entry per point
	}

	/** Fill the zone with @p Ocean again after the zone moved or grew. */
	void RefitOcean(AWaterBodyOcean* Ocean, const AWaterZone* Zone)
	{
		UWaterBodyOceanComponent* Component = Ocean ? Cast<UWaterBodyOceanComponent>(Ocean->GetWaterBodyComponent()) : nullptr;
		if (!Component || !Zone)
			return;
		// The ocean renders centred on its zone but collides centred on its actor: move it with the zone.
		Ocean->Modify();
		Ocean->SetActorLocation(FVector(Zone->GetActorLocation().X, Zone->GetActorLocation().Y, Ocean->GetActorLocation().Z));
		Component->Modify();
		Component->SetCollisionExtents(FVector(Zone->GetZoneExtent() / 2.0, Component->GetCollisionExtents().Z));
		Component->FillWaterZoneWithOcean();
	}

	/** A label (water bodies are found again with unreal.find(label)) and an optional water material; false after Fail. */
	bool ValidBody(const FString& Label, UMaterialInterface* Material)
	{
		if (Label.TrimStartAndEnd().IsEmpty())
			return UnrealMcpRql::Failed<bool>(TEXT("give the water body a label (found again with unreal.find(\"<label>\"))"));
		return !Material || RequireWaterMaterial(Material);
	}

	bool ValidWidth(double Width)
	{
		return (Width >= 100.0 && Width <= 20000.0) || UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("Width %g is outside 100 .. 20000 cm"), Width));
	}

	/**
	 * Create or resize @p World's water zone: SizeMeters 0 covers every landscape plus 25% (Center ignored; refused
	 * without a landscape), else a square SizeMeters wide around Center. Refits the ocean. Null after Fail.
	 */
	AWaterZone* FitZone(UWorld* World, double SizeMeters, FVector Center)
	{
		if (!FMath::IsFinite(SizeMeters) || Center.ContainsNaN())
			return UnrealMcpRql::Failed<AWaterZone*>(TEXT("SizeMeters and Center must be finite numbers"));
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
		AWaterZone* Zone = FindFirst<AWaterZone>(World);
		if (!Zone)
			Zone = Cast<AWaterZone>(SpawnWater(World, AWaterZone::StaticClass(), FVector(Center.X, Center.Y, 0.0), TEXT("WaterZone")));
		if (!Zone)
			return nullptr;
		Zone->Modify();
		Zone->SetActorLocation(FVector(Center.X, Center.Y, Zone->GetActorLocation().Z));
		Zone->SetZoneExtent(FVector2D(Extent, Extent));
		RefitOcean(FindFirst<AWaterBodyOcean>(World), Zone);
		return Zone;
	}

	/** @p World's water zone, made when there is none: over the landscapes, else 2 km around @p Center. Null after Fail. */
	AWaterZone* EnsureZone(UWorld* World, const FVector& Center)
	{
		if (AWaterZone* Zone = FindFirst<AWaterZone>(World))
			return Zone;
		return FitZone(World, LandscapesBounds(World).IsValid ? 0.0 : 2000.0, Center);
	}

	/** A river through @p Points (world, upstream first) @p Width wide (and @p Depth deep when > 0), its zone ensured. */
	AWaterBodyRiver* SpawnRiver(const FString& Label, const TArray<FVector>& Points, double Width, double Depth, UMaterialInterface* Material)
	{
		UWorld* World = UnrealMcpRql::EditorWorld();
		if (!EnsureZone(World, Points[0]))
			return nullptr;
		AWaterBodyRiver* River = Cast<AWaterBodyRiver>(SpawnWater(World, AWaterBodyRiver::StaticClass(), Points[0], Label));
		if (!River)
			return nullptr;
		SetSpline(River, Points);
		SetRiverProfile(River, Width, Depth);
		return FinishBody(River, Material) ? River : nullptr;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// CreateRiverBetween: the lowest path over a landscape and the bed it carves

	/**
	 * A landscape's ground heights (cm, world Z) on its vertex grid: its only height edit layer, or the merged heights when
	 * it has several (edit layers add up). Vertices of components that are not loaded (World Partition) are not Valid.
	 */
	struct FHeightGrid
	{
		ULandscapeInfo* Info = nullptr;
		FGuid EditLayer;    // the layer CarveBed writes to
		FIntRect Rect;      // the loaded landscape's bounding rectangle, in vertices
		FTransform ToWorld; // vertex -> world
		TArray<uint16> Raw;
		TBitArray<> Loaded; // per vertex: its component is loaded
		double ZScale = 1.0; // cm per height unit
		double ZOrigin = 0.0;
		double QuadCm = 1.0; // cm between vertices

		int32 Width() const { return Rect.Width() + 1; }
		int32 Height() const { return Rect.Height() + 1; }
		int32 Index(int32 X, int32 Y) const { return (Y - Rect.Min.Y) * Width() + (X - Rect.Min.X); }
		bool Valid(int32 X, int32 Y) const { return X >= Rect.Min.X && Y >= Rect.Min.Y && X <= Rect.Max.X && Y <= Rect.Max.Y && Loaded[Index(X, Y)]; }
		double At(int32 X, int32 Y) const { return ZOrigin + (static_cast<double>(Raw[Index(X, Y)]) - LandscapeDataAccess::MidValue) * ZScale; }
		uint16 Units(double Cm) const { return static_cast<uint16>(FMath::Clamp(FMath::RoundToInt((Cm - ZOrigin) / ZScale + LandscapeDataAccess::MidValue), 0, 65535)); }
		FVector2D ToLocal(const FVector& World) const { const FVector L = ToWorld.InverseTransformPosition(World); return FVector2D(L.X, L.Y); }
		FVector ToWorldPoint(double X, double Y, double Z) const { FVector P = ToWorld.TransformPosition(FVector(X, Y, 0.0)); P.Z = Z; return P; }
	};

	bool ReadGrid(ALandscapeProxy* Landscape, FHeightGrid& Out)
	{
		if (!UnrealAILandscapeRqlApiLocal::LoadLandscape(Landscape))
			return false;
		Out.Info = Landscape->GetLandscapeInfo();
		if (!Out.Info || !Out.Info->GetLandscapeExtent(Out.Rect))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no landscape components"), *Landscape->GetActorNameOrLabel()));
		Out.EditLayer = UnrealAILandscapeRqlApiLocal::EditLayerGuid(Landscape, ELandscapeToolTargetType::Heightmap);
		if (!Out.EditLayer.IsValid())
			return false;
		Out.ToWorld = Landscape->LandscapeActorToWorld();
		const FVector Scale = Out.ToWorld.GetScale3D().GetAbs();
		if (!FMath::IsNearlyEqual(Scale.X, Scale.Y, 0.01 * Scale.X))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is scaled unevenly on X and Y (%g, %g): give it equal X and Y scale, or place the river with CreateRiver"), *Landscape->GetActorNameOrLabel(), Scale.X, Scale.Y));
		Out.ZScale = Out.ToWorld.GetScale3D().Z * LANDSCAPE_ZSCALE;
		Out.ZOrigin = Out.ToWorld.GetLocation().Z;
		Out.QuadCm = Scale.X;
		Out.Loaded.Init(false, Out.Width() * Out.Height());
		const int32 ComponentQuads = Out.Info->ComponentSizeQuads;
		for (const TPair<FIntPoint, ULandscapeComponent*>& Pair : Out.Info->XYtoComponentMap)
		{
			if (!Pair.Value)
				continue;
			for (int32 Y = Pair.Key.Y * ComponentQuads; Y <= (Pair.Key.Y + 1) * ComponentQuads; ++Y)
			{
				for (int32 X = Pair.Key.X * ComponentQuads; X <= (Pair.Key.X + 1) * ComponentQuads; ++X)
				{
					if (X >= Out.Rect.Min.X && Y >= Out.Rect.Min.Y && X <= Out.Rect.Max.X && Y <= Out.Rect.Max.Y)
						Out.Loaded[Out.Index(X, Y)] = true;
				}
			}
		}
		// Edit layers add up: with one height layer it is the ground, with several read the merged ground.
		Out.Raw = UnrealAILandscapeRqlApiLocal::ReadGround(Landscape, Out.Info, Out.EditLayer, Out.Rect);
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
		const auto Vertex = [&Grid, Step](int32 X, int32 Y) { return FIntPoint(Grid.Rect.Min.X + FMath::Min(X * Step, Grid.Rect.Width()), Grid.Rect.Min.Y + FMath::Min(Y * Step, Grid.Rect.Height())); };
		TArray<double> Z;
		Z.SetNumUninitialized(W * H);
		TBitArray<> Open(false, W * H); // a loaded cell (a route never crosses a component that is not loaded)
		double Low = TNumericLimits<double>::Max(), High = TNumericLimits<double>::Lowest();
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const FIntPoint V = Vertex(X, Y);
				if (!Grid.Valid(V.X, V.Y))
					continue;
				const double Value = Grid.At(V.X, V.Y);
				Z[Y * W + X] = Value;
				Open[Y * W + X] = true;
				Low = FMath::Min(Low, Value);
				High = FMath::Max(High, Value);
			}
		}
		const double Range = FMath::Max(High - Low, 1.0);
		const double Quad = Grid.QuadCm * Step; // cm per cell
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
		TArray<FNode> Frontier;
		Cost[Start] = 0.0;
		Frontier.HeapPush(FNode{ 0.0, Start });
		while (Frontier.Num())
		{
			FNode Node;
			Frontier.HeapPop(Node, EAllowShrinking::No);
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
					if ((DX == 0 && DY == 0) || NX < 0 || NY < 0 || NX >= W || NY >= H || !Open[NY * W + NX])
						continue;
					const int32 Next = NY * W + NX;
					const double Length = Quad * ((DX != 0 && DY != 0) ? UE_DOUBLE_SQRT_2 : 1.0);
					const double Climb = FMath::Max(0.0, Z[Next] - Z[Node.Index]);
					const double NewCost = Node.Cost + Length * (1.0 + 6.0 * (Z[Next] - Low) / Range) + 20.0 * Climb;
					if (NewCost < Cost[Next])
					{
						Cost[Next] = NewCost;
						Previous[Next] = Node.Index;
						Frontier.HeapPush(FNode{ NewCost, Next });
					}
				}
			}
		}
		TArray<FVector2D> Path;
		if (Goal != Start && Previous[Goal] == INDEX_NONE)
			return Path; // no route over the loaded components
		for (int32 Index = Goal; Index != INDEX_NONE; Index = Previous[Index])
		{
			const FIntPoint V = Vertex(Index % W, Index / W);
			Path.Add(FVector2D(V.X, V.Y));
			if (Index == Start)
				break;
		}
		Algo::Reverse(Path);
		if (Path.Num() < 2)
			return { From, To }; // source and mouth in one cell
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
	 * @p Path (vertex space, resampled) winding from side to side: each point moved across the path's direction by a sine of
	 * wavelength 12 river widths (at least 6 samples), up to @p Amount x 1.5 widths, fading in and out over half a wavelength
	 * at both ends (Source and Mouth stay). A point whose moved ground lies more than @p Climb cm above the water's ground
	 * there (the lowest ground upstream on the route) or more than @p Climb below the route's swings less (a half, a
	 * quarter, not at all): the river stays on the valley floor, never climbs its sides for a bend and never drops into a
	 * side gully (which would lower its whole bed downstream). Where the route turns sharply it does not swing (the bend
	 * would fold the spline back on itself).
	 */
	TArray<FVector2D> Meander(const FHeightGrid& Grid, const TArray<FVector2D>& Path, double WidthCm, double Amount, double Climb)
	{
		if (Amount <= 0.0 || Path.Num() < 3)
			return Path;
		TArray<double> Along; // distance from the source, vertices
		Along.Add(0.0);
		for (int32 Index = 1; Index < Path.Num(); ++Index)
			Along.Add(Along.Last() + FVector2D::Distance(Path[Index - 1], Path[Index]));
		const double Length = Along.Last();
		const double Spacing = Length / (Path.Num() - 1);
		const double Wave = FMath::Max(12.0 * WidthCm / Grid.QuadCm, 6.0 * Spacing); // vertices
		const double Swing = Amount * 1.5 * WidthCm / Grid.QuadCm;                  // vertices
		const auto Ground = [&Grid](const FVector2D& P) -> TOptional<double>
		{
			const int32 X = FMath::RoundToInt(P.X), Y = FMath::RoundToInt(P.Y);
			return Grid.Valid(X, Y) ? TOptional<double>(Grid.At(X, Y)) : TOptional<double>();
		};
		TArray<FVector2D> Out = Path;
		double Running = TNumericLimits<double>::Max(); // the lowest route ground so far: where the water sits
		for (int32 Index = 1; Index + 1 < Path.Num(); ++Index)
		{
			if (const TOptional<double> Before = Ground(Path[Index - 1]))
				Running = FMath::Min(Running, Before.GetValue());
			const FVector2D In = (Path[Index] - Path[Index - 1]).GetSafeNormal(), Onward = (Path[Index + 1] - Path[Index]).GetSafeNormal();
			if (FVector2D::DotProduct(In, Onward) < 0.7) // a turn sharper than about 45 degrees
				continue;
			const FVector2D Direction = (Path[Index + 1] - Path[Index - 1]).GetSafeNormal();
			const FVector2D Across(-Direction.Y, Direction.X);
			const double Fade = FMath::Clamp(FMath::Min(Along[Index], Length - Along[Index]) / (0.5 * Wave), 0.0, 1.0);
			const double Offset = Swing * Fade * FMath::Sin(2.0 * PI * Along[Index] / Wave);
			const TOptional<double> Here = Ground(Path[Index]);
			if (!Here.IsSet())
				continue;
			const double Water = FMath::Min(Running, Here.GetValue());
			for (const double Part : { 1.0, 0.5, 0.25 })
			{
				const FVector2D Moved = Path[Index] + Across * Offset * Part;
				const TOptional<double> There = Ground(Moved);
				if (There.IsSet() && There.GetValue() <= Water + Climb && There.GetValue() >= Here.GetValue() - Climb)
				{
					Out[Index] = Moved;
					break;
				}
			}
		}
		// One smoothing pass: neighbours that swung by different parts would zigzag.
		TArray<FVector2D> Smooth = Out;
		for (int32 Index = 1; Index + 1 < Out.Num(); ++Index)
			Smooth[Index] = (Out[Index - 1] + Out[Index] * 2.0 + Out[Index + 1]) * 0.25;
		return Smooth;
	}

	/**
	 * Lower the landscape along the river (vertex-space @p Path with bed heights @p Bed, cm): flat bed HalfWidth either
	 * side of the centre line, then banks rising 1:1 (at most 2 x the width) to the original ground; only ever lowers.
	 * Works on the band around the path only; one write, merged before returning. How many vertices it lowered (0: none).
	 */
	int32 CarveBed(ALandscapeProxy* Landscape, const FHeightGrid& Grid, const TArray<FVector2D>& Path, const TArray<double>& Bed, double HalfWidthCm)
	{
		const double Half = HalfWidthCm / Grid.QuadCm;       // vertices
		const double Bank = 4.0 * HalfWidthCm / Grid.QuadCm; // vertices
		const double Rise = Grid.QuadCm;                     // cm per vertex of bank (1:1)
		FBox2D Box(ForceInit);
		for (const FVector2D& P : Path)
			Box += P;
		const FIntRect Band(
			FMath::Max(Grid.Rect.Min.X, FMath::FloorToInt(Box.Min.X - Half - Bank)), FMath::Max(Grid.Rect.Min.Y, FMath::FloorToInt(Box.Min.Y - Half - Bank)),
			FMath::Min(Grid.Rect.Max.X, FMath::CeilToInt(Box.Max.X + Half + Bank)), FMath::Min(Grid.Rect.Max.Y, FMath::CeilToInt(Box.Max.Y + Half + Bank)));
		// The ground lowered per vertex (height units), then subtracted from the edit layer: with several height layers the
		// ground is their sum, so the layer is lowered by the difference rather than set to the target. Each vertex takes the
		// bed of its NEAREST point of the path: the lower bed of a path sample further on would cut its 1:1 bank through a
		// slope steeper than that bank (a channel climbing a rise came out deeper than Depth there).
		const int32 Width = Band.Width() + 1;
		TArray<uint16> Lower;
		Lower.SetNumZeroed(Width * (Band.Height() + 1));
		TArray<double> Nearest;
		Nearest.Init(TNumericLimits<double>::Max(), Lower.Num());
		for (int32 Index = 0; Index + 1 < Path.Num(); ++Index)
		{
			const FVector2D A = Path[Index], B = Path[Index + 1];
			const int32 MinX = FMath::Max(Band.Min.X, FMath::FloorToInt(FMath::Min(A.X, B.X) - Half - Bank));
			const int32 MaxX = FMath::Min(Band.Max.X, FMath::CeilToInt(FMath::Max(A.X, B.X) + Half + Bank));
			const int32 MinY = FMath::Max(Band.Min.Y, FMath::FloorToInt(FMath::Min(A.Y, B.Y) - Half - Bank));
			const int32 MaxY = FMath::Min(Band.Max.Y, FMath::CeilToInt(FMath::Max(A.Y, B.Y) + Half + Bank));
			const FVector2D AB = B - A;
			const double LengthSquared = FMath::Max(AB.SizeSquared(), UE_SMALL_NUMBER);
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const FVector2D P(X, Y);
					const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LengthSquared, 0.0, 1.0);
					const double Distance = FVector2D::Distance(P, A + AB * T);
					if (Distance > Half + Bank || !Grid.Valid(X, Y))
						continue;
					const int32 Cell = (Y - Band.Min.Y) * Width + (X - Band.Min.X);
					if (Distance >= Nearest[Cell])
						continue;
					Nearest[Cell] = Distance;
					const uint16 Target = Grid.Units(FMath::Lerp(Bed[Index], Bed[Index + 1], T) + FMath::Max(0.0, Distance - Half) * Rise);
					const uint16 Ground = Grid.Raw[Grid.Index(X, Y)];
					Lower[Cell] = Target < Ground ? Ground - Target : 0;
				}
			}
		}
		const int32 Changed = Algo::CountIf(Lower, [](uint16 Amount) { return Amount > 0; });
		if (Changed == 0)
			return 0;
		UnrealAILandscapeRqlApiLocal::AddToHeightLayer(Landscape, Grid.Info, Grid.EditLayer, Band,
			[&Lower](int32 Index) { return -static_cast<int32>(Lower[Index]); });
		return Changed;
	}
}

int32 UnrealAILandscapeRqlApiLocal::CarveAlong(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Depth)
{
	using namespace UnrealAIWaterRqlLocal;
	FHeightGrid Grid;
	if (!ReadGrid(Landscape, Grid))
		return 0;
	// Points far off the landscape would make millions of samples: allow the channel's reach beyond the edge, no more.
	const double Reach = 5.0 * Width / Grid.QuadCm + 2.0; // vertices: the bed plus its banks (CarveBed)
	TArray<FVector2D> Local;
	for (const FVector& Point : Points)
	{
		const FVector2D P = Grid.ToLocal(Point);
		if (P.X < Grid.Rect.Min.X - Reach || P.Y < Grid.Rect.Min.Y - Reach || P.X > Grid.Rect.Max.X + Reach || P.Y > Grid.Rect.Max.Y + Reach)
			return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("point [%.0f, %.0f] is off the landscape (its Bounds): keep the points on it or just past its edge"), Point.X, Point.Y));
		Local.Add(P);
	}
	// One sample per vertex along the path: the bed follows the ground under it, Depth below.
	const TArray<FVector2D> Path = Resample(Local, 1.0);
	constexpr double Off = TNumericLimits<double>::Max(); // a sample off the (loaded) landscape
	TArray<double> Bed;
	double Low = Off, High = TNumericLimits<double>::Lowest();
	for (const FVector2D& P : Path)
	{
		const int32 X = FMath::RoundToInt(P.X);
		const int32 Y = FMath::RoundToInt(P.Y);
		if (!Grid.Valid(X, Y))
		{
			Bed.Add(Off);
			continue;
		}
		const double Z = Grid.At(X, Y) - Depth;
		Bed.Add(Z);
		Low = FMath::Min(Low, Z);
		High = FMath::Max(High, Z);
	}
	if (Low > High)
		return UnrealMcpRql::Failed<int32>(TEXT("the points do not cross the landscape (its Bounds)"));
	if (!FitsHeightRange(Landscape, Low, High))
		return 0;
	// A sample off the landscape takes its neighbour's bed (the channel runs straight on to the edge).
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (int32 Index = 0; Index < Bed.Num(); ++Index)
		{
			const int32 From = Pass == 0 ? Index : Bed.Num() - 1 - Index;
			const int32 Prev = Pass == 0 ? From - 1 : From + 1;
			if (Bed[From] == Off && Bed.IsValidIndex(Prev) && Bed[Prev] != Off)
				Bed[From] = Bed[Prev];
		}
	}
	const int32 Changed = CarveBed(Landscape, Grid, Path, Bed, Width * 0.5);
	return Changed > 0 ? Changed : UnrealMcpRql::Failed<int32>(TEXT("nothing to carve: the points do not cross a loaded part of the landscape"));
}

int32 UnrealAILandscapeRqlApiLocal::RampAlong(ALandscapeProxy* Landscape, const TArray<FVector>& Points, double Width, double Falloff)
{
	using namespace UnrealAIWaterRqlLocal;
	FHeightGrid Grid;
	if (!ReadGrid(Landscape, Grid))
		return 0;
	const double Half = Width * 0.5 / Grid.QuadCm; // vertices
	const double Band = Falloff / Grid.QuadCm;     // vertices
	const double Reach = Half + Band + 2.0;
	TArray<FVector2D> Path;
	double Low = TNumericLimits<double>::Max(), High = TNumericLimits<double>::Lowest();
	FBox2D Box(ForceInit);
	for (const FVector& Point : Points)
	{
		const FVector2D P = Grid.ToLocal(Point);
		if (P.X < Grid.Rect.Min.X - Reach || P.Y < Grid.Rect.Min.Y - Reach || P.X > Grid.Rect.Max.X + Reach || P.Y > Grid.Rect.Max.Y + Reach)
			return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("point [%.0f, %.0f] is off the landscape (its Bounds): keep the points on it or just past its edge"), Point.X, Point.Y));
		Path.Add(P);
		Box += P;
		Low = FMath::Min(Low, Point.Z);
		High = FMath::Max(High, Point.Z);
	}
	if (!FitsHeightRange(Landscape, Low, High))
		return 0;
	const FIntRect Area(
		FMath::Max(Grid.Rect.Min.X, FMath::FloorToInt(Box.Min.X - Half - Band)), FMath::Max(Grid.Rect.Min.Y, FMath::FloorToInt(Box.Min.Y - Half - Band)),
		FMath::Min(Grid.Rect.Max.X, FMath::CeilToInt(Box.Max.X + Half + Band)), FMath::Min(Grid.Rect.Max.Y, FMath::CeilToInt(Box.Max.Y + Half + Band)));
	if (Area.Min.X > Area.Max.X || Area.Min.Y > Area.Max.Y)
		return UnrealMcpRql::Failed<int32>(TEXT("the points do not cross the landscape (its Bounds)"));
	// Bounded work on the game thread: every stretch scans its own box (a long diagonal stretch is a big square).
	int64 Work = 0;
	for (int32 Index = 0; Index + 1 < Path.Num(); ++Index)
	{
		const FVector2D Size = (Path[Index] - Path[Index + 1]).GetAbs() + FVector2D(2.0 * (Half + Band) + 1.0);
		Work += static_cast<int64>(Size.X) * static_cast<int64>(Size.Y);
	}
	if (Work > 200000000)
		return UnrealMcpRql::Failed<int32>(TEXT("the path covers too much ground for one call (long diagonal stretches): give it more, shorter points or split it into several Ramp calls"));
	// Each vertex takes the road height of its NEAREST point of the path (a bend's inside would otherwise get the far leg's height).
	const int32 Width2 = Area.Width() + 1;
	TArray<double> Nearest;
	Nearest.Init(TNumericLimits<double>::Max(), Width2 * (Area.Height() + 1));
	TArray<double> Road;
	Road.SetNumZeroed(Nearest.Num());
	for (int32 Index = 0; Index + 1 < Path.Num(); ++Index)
	{
		const FVector2D A = Path[Index], B = Path[Index + 1];
		const FVector2D AB = B - A;
		const double LengthSquared = FMath::Max(AB.SizeSquared(), UE_SMALL_NUMBER);
		const int32 MinX = FMath::Max(Area.Min.X, FMath::FloorToInt(FMath::Min(A.X, B.X) - Half - Band));
		const int32 MaxX = FMath::Min(Area.Max.X, FMath::CeilToInt(FMath::Max(A.X, B.X) + Half + Band));
		const int32 MinY = FMath::Max(Area.Min.Y, FMath::FloorToInt(FMath::Min(A.Y, B.Y) - Half - Band));
		const int32 MaxY = FMath::Min(Area.Max.Y, FMath::CeilToInt(FMath::Max(A.Y, B.Y) + Half + Band));
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FVector2D P(X, Y);
				const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LengthSquared, 0.0, 1.0);
				const double Distance = FVector2D::Distance(P, A + AB * T);
				const int32 Cell = (Y - Area.Min.Y) * Width2 + (X - Area.Min.X);
				if (Distance > Half + Band || Distance >= Nearest[Cell] || !Grid.Valid(X, Y))
					continue;
				Nearest[Cell] = Distance;
				Road[Cell] = FMath::Lerp(Points[Index].Z, Points[Index + 1].Z, T);
			}
		}
	}
	// The change per vertex (height units), added to the edit layer: with several height layers the ground is their sum (the
	// layer then holds only its own part, so a large cut can clamp at its bottom, as with Carve).
	TArray<int32> Delta;
	Delta.SetNumZeroed(Nearest.Num());
	int32 Changed = 0;
	for (int32 Y = Area.Min.Y; Y <= Area.Max.Y; ++Y)
	{
		for (int32 X = Area.Min.X; X <= Area.Max.X; ++X)
		{
			const int32 Cell = (Y - Area.Min.Y) * Width2 + (X - Area.Min.X);
			if (Nearest[Cell] == TNumericLimits<double>::Max())
				continue;
			const double Distance = Nearest[Cell];
			const double Weight = Distance <= Half ? 1.0 : Band > 0.0 ? Cosine((Distance - Half) / Band) : 0.0;
			const double Ground = Grid.At(X, Y);
			Delta[Cell] = static_cast<int32>(Grid.Units(FMath::Lerp(Ground, Road[Cell], Weight))) - static_cast<int32>(Grid.Raw[Grid.Index(X, Y)]);
			Changed += Delta[Cell] != 0 ? 1 : 0;
		}
	}
	if (Changed == 0)
		return UnrealMcpRql::Failed<int32>(TEXT("nothing to change: the ground along the points is already at their Z (or off the loaded landscape)"));
	AddToHeightLayer(Landscape, Grid.Info, Grid.EditLayer, Area, [&Delta](int32 Index) { return Delta[Index]; });
	return Changed;
}

bool UnrealAILandscapeRqlApiLocal::PlanRoad(ALandscapeProxy* Landscape, const FVector& Start, const FVector& End, double Width, double MaxSlope, TArray<FVector>& OutPoints, bool& bReached)
{
	using namespace UnrealAIWaterRqlLocal;
	FHeightGrid Grid;
	if (!ReadGrid(Landscape, Grid))
		return false;
	// A grid of nodes half the road's width apart (each leg of a switchback a stretch of road, yet fine enough to wind up
	// a narrow hilltop), at most 400 x 400.
	const int32 Step = FMath::Max3(1, static_cast<int32>(FMath::CeilToInt(0.5 * Width / Grid.QuadCm)), static_cast<int32>(FMath::CeilToInt(FMath::Max(Grid.Width(), Grid.Height()) / 400.0)));
	const int32 GW = (Grid.Width() - 1) / Step + 1, GH = (Grid.Height() - 1) / Step + 1;
	const auto NodeOf = [&](const FVector& World) -> FIntPoint
	{
		const FVector2D L = Grid.ToLocal(World);
		return FIntPoint(FMath::Clamp(FMath::RoundToInt((L.X - Grid.Rect.Min.X) / Step), 0, GW - 1), FMath::Clamp(FMath::RoundToInt((L.Y - Grid.Rect.Min.Y) / Step), 0, GH - 1));
	};
	const auto VX = [&](double I) { return FMath::Min(Grid.Rect.Min.X + I * Step, double(Grid.Rect.Max.X)); };
	const auto VY = [&](double J) { return FMath::Min(Grid.Rect.Min.Y + J * Step, double(Grid.Rect.Max.Y)); };
	const auto GroundAt = [&](double X, double Y) -> TOptional<double> // vertex space, bilinear
	{
		const int32 X0 = FMath::Clamp(FMath::FloorToInt(X), Grid.Rect.Min.X, Grid.Rect.Max.X), Y0 = FMath::Clamp(FMath::FloorToInt(Y), Grid.Rect.Min.Y, Grid.Rect.Max.Y);
		const int32 X1 = FMath::Min(X0 + 1, Grid.Rect.Max.X), Y1 = FMath::Min(Y0 + 1, Grid.Rect.Max.Y);
		if (!Grid.Valid(X0, Y0) || !Grid.Valid(X1, Y0) || !Grid.Valid(X0, Y1) || !Grid.Valid(X1, Y1))
			return {};
		const double FX = FMath::Clamp(X - X0, 0.0, 1.0), FY = FMath::Clamp(Y - Y0, 0.0, 1.0);
		return FMath::Lerp(FMath::Lerp(Grid.At(X0, Y0), Grid.At(X1, Y0), FX), FMath::Lerp(Grid.At(X0, Y1), Grid.At(X1, Y1), FX), FY);
	};
	const FVector2D StartAt = Grid.ToLocal(Start), EndAt = Grid.ToLocal(End);
	const FIntPoint From = NodeOf(Start), To = NodeOf(End);
	const auto On = [&](const FVector2D& At) { return At.X >= Grid.Rect.Min.X && At.Y >= Grid.Rect.Min.Y && At.X <= Grid.Rect.Max.X && At.Y <= Grid.Rect.Max.Y && GroundAt(At.X, At.Y).IsSet(); };
	if (!On(StartAt) || !On(EndAt) || !GroundAt(VX(From.X), VY(From.Y)).IsSet() || !GroundAt(VX(To.X), VY(To.Y)).IsSet())
		return UnrealMcpRql::Failed<bool>(TEXT("Start and End must be on the (loaded) landscape"));
	// Moves in 176 directions (offsets up to 8 nodes, 2 to 7 degrees apart). On a flank far steeper than MaxSlope only
	// headings within a few degrees of the contour climb no more than it allows: with 48 directions (7.5 degrees apart) few of
	// them did, and roads up a 44 degree hill ran four to six times longer than the climb needs.
	constexpr int32 Reach = 8;
	TArray<FIntPoint> Moves;
	for (int32 DY = -Reach; DY <= Reach; ++DY)
		for (int32 DX = -Reach; DX <= Reach; ++DX)
			if ((DX || DY) && FMath::GreatestCommonDivisor(FMath::Abs(DX), FMath::Abs(DY)) == 1)
				Moves.Add(FIntPoint(DX, DY));
	const double Limit = FMath::Tan(FMath::DegreesToRadians(MaxSlope * 0.9)); // a margin for the grading between nodes
	constexpr double MaxCut = 150.0; // cm the grading may cut or fill under one move
	const double NodeCm = Step * Grid.QuadCm;
	TArray<double> Cost;
	Cost.Init(TNumericLimits<double>::Max(), GW * GH);
	TArray<int32> Came;
	Came.Init(INDEX_NONE, GW * GH);
	struct FOpen { double F; int32 Node; bool operator<(const FOpen& O) const { return F < O.F; } };
	TArray<FOpen> Heap;
	const auto H = [&](int32 Node) { return FVector2D::Distance(FVector2D(Node % GW, Node / GW), FVector2D(To)) * NodeCm; };
	const int32 StartNode = From.Y * GW + From.X, GoalNode = To.Y * GW + To.X;
	Cost[StartNode] = 0.0;
	Heap.HeapPush({ H(StartNode), StartNode });
	int32 Best = StartNode; // nearest to the goal reached so far
	const double Deadline = FPlatformTime::Seconds() + 20.0;
	while (Heap.Num() && FPlatformTime::Seconds() < Deadline)
	{
		FOpen Top;
		Heap.HeapPop(Top, EAllowShrinking::No);
		const int32 Node = Top.Node;
		if (Top.F - H(Node) > Cost[Node] + 1e-6)
			continue; // stale
		if (H(Node) < H(Best))
			Best = Node;
		if (Node == GoalNode)
			break;
		const int32 NI = Node % GW, NJ = Node / GW;
		const double Here = GroundAt(VX(NI), VY(NJ)).Get(0.0);
		// The way the best route came in: a move may turn at most 60 degrees from it, so the road winds in curves and its
		// switchbacks stay wide (a sharper turn would lay the next leg on the last one).
		const FVector2D In = Came[Node] == INDEX_NONE ? FVector2D::ZeroVector : FVector2D(NI - Came[Node] % GW, NJ - Came[Node] / GW).GetSafeNormal();
		for (const FIntPoint& Move : Moves)
		{
			const int32 I = NI + Move.X, J = NJ + Move.Y;
			if (I < 0 || J < 0 || I >= GW || J >= GH)
				continue;
			if (!In.IsZero() && FVector2D::DotProduct(In, FVector2D(Move).GetSafeNormal()) < 0.5 && J * GW + I != GoalNode)
				continue; // the last step onto End may turn as it must
			// The move climbs gently enough from end to end, and the ground under it stays near that straight line (the grading
			// cuts and fills at most MaxCut): a chord along a curved contour dips a little in the middle, which a stretch-by-stretch
			// slope check read as too steep, so long contour moves were refused and roads wandered.
			const double Length = FVector2D(Move).Size() * NodeCm;
			const int32 Stretches = FMath::Max(FMath::Abs(Move.X), FMath::Abs(Move.Y));
			const TOptional<double> There = GroundAt(VX(NI + Move.X), VY(NJ + Move.Y));
			if (!There.IsSet())
				continue;
			const double Worst = FMath::Abs(There.GetValue() - Here) / Length;
			bool bOk = Worst <= Limit;
			for (int32 K = 1; K < Stretches && bOk; ++K)
			{
				const double T = double(K) / Stretches;
				const TOptional<double> Z = GroundAt(VX(NI + Move.X * T), VY(NJ + Move.Y * T));
				bOk = Z.IsSet() && FMath::Abs(Z.GetValue() - FMath::Lerp(Here, There.GetValue(), T)) <= MaxCut;
			}
			if (!bOk)
				continue;
			const int32 Next = J * GW + I;
			// Length, a little more for steeper ground: a road climbs near its limit rather than wandering over gentle ground (a
			// cost of up to twice the length for a steep move made routes four to six times longer than the climb needs).
			const double NewCost = Cost[Node] + Length * (1.0 + 0.1 * Worst / FMath::Max(Limit, UE_SMALL_NUMBER));
			if (NewCost < Cost[Next])
			{
				Cost[Next] = NewCost;
				Came[Next] = Node;
				Heap.HeapPush({ NewCost + H(Next), Next });
			}
		}
	}
	const int32 Last = Came[GoalNode] != INDEX_NONE || GoalNode == StartNode ? GoalNode : Best;
	TArray<FVector2D> Route; // vertex space, start first
	for (int32 Node = Last; Node != INDEX_NONE; Node = Came[Node])
		Route.Add(FVector2D(VX(Node % GW), VY(Node / GW)));
	Algo::Reverse(Route);
	if (Route.Num() < 2 && Last != GoalNode)
		return UnrealMcpRql::Failed<bool>(TEXT("no route leaves Start within MaxSlope: the ground around it is steeper (start on gentler ground or allow a larger MaxSlope)"));
	if (Route.Num() < 2)
		Route.Add(EndAt); // Start and End in one node
	Route[0] = StartAt;
	if (Last == GoalNode)
		Route.Last() = EndAt;
	// A point at least every half node (at most 5 m, at most ~1000 points), lightly smoothed (rounds each turn).
	double RouteLength = 0.0;
	for (int32 Index = 1; Index < Route.Num(); ++Index)
		RouteLength += FVector2D::Distance(Route[Index - 1], Route[Index]);
	const TArray<FVector2D> Path = Resample(Route, FMath::Max3(1.0, FMath::Min(500.0 / Grid.QuadCm, Step * 0.5), RouteLength / 990.0));
	// Each point's Z: the ground, limited to MaxSlope between neighbours. Both ends are pinned to the ground when a road can
	// join them within MaxSlope (else only the start): forward and backward passes clamp each stretch until none is steeper.
	const double Rise = FMath::Tan(FMath::DegreesToRadians(MaxSlope)) * Grid.QuadCm; // cm per vertex of length
	const int32 Count = Path.Num();
	TArray<double> Z, Run;
	Z.SetNumUninitialized(Count);
	Run.SetNumZeroed(Count); // Run[i]: the stretch from point i - 1, vertices
	double Total = 0.0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Run[Index] = Index ? FVector2D::Distance(Path[Index - 1], Path[Index]) : 0.0;
		Total += Run[Index];
		Z[Index] = GroundAt(Path[Index].X, Path[Index].Y).Get(Index ? Z[Index - 1] : 0.0);
	}
	const TOptional<double> EndGround = GroundAt(EndAt.X, EndAt.Y);
	const bool bAtEnd = Last == GoalNode && EndGround.IsSet();
	const bool bPinEnd = bAtEnd && FMath::Abs(Z.Last() - Z[0]) <= Rise * Total;
	for (int32 Pass = 0; Pass < 100; ++Pass)
	{
		double Moved = 0.0;
		for (int32 Index = 1; Index < (bPinEnd ? Count - 1 : Count); ++Index)
		{
			const double Clamped = FMath::Clamp(Z[Index], Z[Index - 1] - Rise * Run[Index], Z[Index - 1] + Rise * Run[Index]);
			Moved = FMath::Max(Moved, FMath::Abs(Clamped - Z[Index]));
			Z[Index] = Clamped;
		}
		if (!bPinEnd)
			break; // the forward pass alone leaves no stretch steeper
		for (int32 Index = Count - 2; Index > 0; --Index)
		{
			const double Clamped = FMath::Clamp(Z[Index], Z[Index + 1] - Rise * Run[Index + 1], Z[Index + 1] + Rise * Run[Index + 1]);
			Moved = FMath::Max(Moved, FMath::Abs(Clamped - Z[Index]));
			Z[Index] = Clamped;
		}
		if (Moved < 0.01)
			break;
	}
	OutPoints.Reset();
	for (int32 Index = 0; Index < Count; ++Index)
		OutPoints.Add(Grid.ToWorldPoint(Path[Index].X, Path[Index].Y, Z[Index]));
	bReached = bAtEnd && FMath::Abs(Z.Last() - EndGround.GetValue()) <= 100.0;
	return true;
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
	return World ? FitZone(World, SizeMeters, Center) : UnrealMcpRql::Failed<AWaterZone*>(TEXT("no editor world"));
}

AWaterBodyOcean* UUnrealAIWaterRqlApi::CreateOcean(const FString& Label, double Height, UMaterialInterface* Material)
{
	UWorld* World = UnrealMcpRql::EditorWorld();
	if (!World)
		return UnrealMcpRql::Failed<AWaterBodyOcean*>(TEXT("no editor world"));
	if (!ValidBody(Label, Material))
		return nullptr;
	if (!FMath::IsFinite(Height) || FMath::Abs(Height) > 1000000.0)
		return UnrealMcpRql::Failed<AWaterBodyOcean*>(TEXT("Height must be within -1000000 .. 1000000 cm"));
	if (const AWaterBodyOcean* Existing = FindFirst<AWaterBodyOcean>(World))
		return UnrealMcpRql::Failed<AWaterBodyOcean*>(FString::Printf(TEXT("the level already has an ocean (%s; one per world): move it with set Location.Z, or Destroy it first"), *Existing->GetActorLabel()));
	const AWaterZone* Zone = EnsureZone(World, FVector::ZeroVector);
	if (!Zone)
		return nullptr;
	// The ocean's actor factory fills the zone (collision and ocean extents), as the Place Actors panel does.
	const FVector Location(Zone->GetActorLocation().X, Zone->GetActorLocation().Y, Height);
	AWaterBodyOcean* Ocean = Cast<AWaterBodyOcean>(SpawnWater(World, AWaterBodyOcean::StaticClass(), Location, Label));
	return Ocean && FinishBody(Ocean, Material) ? Ocean : nullptr;
}

AWaterBodyLake* UUnrealAIWaterRqlApi::CreateLake(const FString& Label, FVector Center, double Radius, UMaterialInterface* Material)
{
	if (!ValidBody(Label, Material))
		return nullptr;
	if (!(Radius >= 100.0 && Radius <= 200000.0))
		return UnrealMcpRql::Failed<AWaterBodyLake*>(FString::Printf(TEXT("Radius %g is outside 100 .. 200000 cm"), Radius));
	UWorld* World = UnrealMcpRql::EditorWorld();
	if (Center.ContainsNaN() || !EnsureZone(World, Center))
		return Center.ContainsNaN() ? UnrealMcpRql::Failed<AWaterBodyLake*>(TEXT("Center must be finite numbers")) : nullptr;
	AWaterBodyLake* Lake = Cast<AWaterBodyLake>(SpawnWater(World, AWaterBodyLake::StaticClass(), Center, Label));
	if (!Lake)
		return nullptr;
	TArray<FVector> Shore;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const double Angle = UE_DOUBLE_TWO_PI * Index / 8.0;
		Shore.Add(Center + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0));
	}
	SetSpline(Lake, Shore);
	return FinishBody(Lake, Material) ? Lake : nullptr;
}

AWaterBodyRiver* UUnrealAIWaterRqlApi::CreateRiver(const FString& Label, const TArray<FVector>& Points, double Width, UMaterialInterface* Material)
{
	if (!ValidBody(Label, Material) || !ValidWidth(Width))
		return nullptr;
	if (Points.Num() < 2 || Points.Num() > 200)
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(FString::Printf(TEXT("a river needs 2 .. 200 points (upstream first), got %d"), Points.Num()));
	if (Points.ContainsByPredicate([](const FVector& Point) { return Point.ContainsNaN(); }))
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(TEXT("river points must be finite numbers"));
	return SpawnRiver(Label, Points, Width, 0.0, Material);
}

AWaterBodyRiver* UUnrealAIWaterRqlApi::CreateRiverBetween(const FString& Label, ALandscapeProxy* Landscape, FVector Source, FVector Mouth, double Width, double Depth, UMaterialInterface* Material, double Meander)
{
	if (!Landscape)
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(TEXT("Landscape is null: pass unreal.find(\"<label>\")"));
	if (!ValidBody(Label, Material) || !ValidWidth(Width))
		return nullptr;
	if (!(Depth >= 0.0 && Depth <= 10000.0))
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(FString::Printf(TEXT("Depth %g is outside 0 .. 10000 cm"), Depth));
	if (!(Meander >= 0.0 && Meander <= 1.0))
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(FString::Printf(TEXT("Meander %g is outside 0 .. 1"), Meander));
	FHeightGrid Grid;
	if (!ReadGrid(Landscape, Grid))
		return nullptr;
	const FVector2D From = Grid.ToLocal(Source), To = Grid.ToLocal(Mouth);
	const auto Inside = [&Grid](const FVector2D& P) { return Grid.Valid(FMath::RoundToInt(P.X), FMath::RoundToInt(P.Y)); };
	if (!Inside(From) || !Inside(To))
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(TEXT("Source and Mouth must be on the landscape (its Bounds; on a World Partition map, in a loaded region): a river mouth at the coast stays a little inside the landscape's edge"));
	if (FVector2D::Distance(From, To) * Grid.QuadCm < Width * 2.0)
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(TEXT("Source and Mouth are too close for a river of this Width"));

	// The route, resampled at most 150 points, every 2 river widths or more.
	const TArray<FVector2D> Route = LowestPath(Grid, From, To);
	if (Route.IsEmpty())
		return UnrealMcpRql::Failed<AWaterBodyRiver*>(TEXT("no route over the loaded landscape joins Source and Mouth: load the region between them (World Partition) first"));
	double Length = 0.0;
	for (int32 Index = 1; Index < Route.Num(); ++Index)
		Length += FVector2D::Distance(Route[Index - 1], Route[Index]);
	// A bend may lift the river onto ground no higher than its surface would sit above the old ground (a third of the bed
	// with a bed, else the 20 cm it floats).
	const TArray<FVector2D> Path = UnrealAIWaterRqlLocal::Meander(Grid, Resample(Route, FMath::Max(2.0 * Width / Grid.QuadCm, Length / 149.0)), Width, Meander,
		Depth > 0.0 ? Depth / 3.0 : 20.0);

	// The water surface flows downhill: never above any ground upstream of it. With a bed it sits a third of the depth
	// below the old ground (the bed Depth below that ground), so the water shows even where the route crosses a rise.
	TArray<double> Surface, Bed;
	double Running = TNumericLimits<double>::Max();
	for (const FVector2D& P : Path)
	{
		const int32 X = FMath::Clamp(FMath::RoundToInt(P.X), Grid.Rect.Min.X, Grid.Rect.Max.X);
		const int32 Y = FMath::Clamp(FMath::RoundToInt(P.Y), Grid.Rect.Min.Y, Grid.Rect.Max.Y);
		if (Grid.Valid(X, Y))
			Running = FMath::Min(Running, Grid.At(X, Y));
		Bed.Add(Running - Depth);
		Surface.Add(Depth > 0.0 ? Running - Depth / 3.0 : Running + 20.0);
	}
	const int32 Carved = Depth > 0.0 ? CarveBed(Landscape, Grid, Path, Bed, Width * 0.5) : 0;

	TArray<FVector> Points;
	for (int32 Index = 0; Index < Path.Num(); ++Index)
		Points.Add(Grid.ToWorldPoint(Path[Index].X, Path[Index].Y, Surface[Index]));
	if (Carved > 0) // its bed is carved into the landscape (whether or not the river then spawns): logged like a Carve
	{
		UnrealAILandscapeRqlApiLocal::WarnRoadsCut(Landscape, Points, Width + 2.0 * Depth, TEXT("the river's bed")); // its bottom and banks
		UnrealAILandscapeRqlApiLocal::RecordEdit(Landscape, TEXT("CreateRiverBetween"), Label, Points, 0.0, Width, Depth);
	}
	return SpawnRiver(Label, Points, Width, Depth > 0.0 ? Depth * 2.0 / 3.0 : 0.0, Material);
}

AWaterBody* UUnrealAIWaterRqlApi::SetMaterial(AWaterBody* Body, UMaterialInterface* Material)
{
	if (!Body)
		return UnrealMcpRql::Failed<AWaterBody*>(TEXT("Body is null: pass unreal.find(\"<label>\") or $ from waterBodies"));
	if ((Material && !RequireWaterMaterial(Material)) || !EnsureZone(Body->GetWorld(), Body->GetActorLocation()))
		return nullptr;
	if (Material)
		return FinishBody(Body, Material) ? Body : nullptr;
	if (!UnrealAIWaterSafety::ApplyWaterDefaults(Body)) // rebuilds it
		return UnrealMcpRql::Failed<AWaterBody*>(FString::Printf(TEXT("the Water plugin has no Place Actors defaults for %s: pass a material"), *Body->GetClass()->GetName()));
	return Body->GetWaterBodyComponent()->GetWaterMaterial() ? Body
		: UnrealMcpRql::Failed<AWaterBody*>(TEXT("the Water plugin has no default water material (Project Settings > Water Editor): pass one, e.g. /Water/Materials/WaterSurface/Water_Material"));
}
