// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainWaterRqlApi.h"
#include "UnrealAIMeshTerrainWaterCommon.h"

#include "IUnrealMcpRqlModule.h"

#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "Editor/EditorEngine.h" // FActorLabelUtilities
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Materials/MaterialInterface.h"
#include "ProjectDescriptor.h"
#include "ScopedTransaction.h"

#include "MeshPartition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionWaterModifier.h"

#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyOceanComponent.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "WaterZoneActor.h"

#define LOCTEXT_NAMESPACE "UnrealAIMeshTerrainWater"

namespace UnrealAIMeshTerrainWaterApiLocal
{
	using namespace UnrealAIMeshTerrainWater;

	/**
	 * UActorFactory::PostSpawnActor (what the Place Actors panel runs after spawning: the Water factories' defaults) is
	 * protected; a pointer to it taken in a derived class's scope calls it (virtually) on any factory. Never instantiated.
	 */
	struct FPostSpawnAccess : UActorFactory
	{
		static constexpr auto PostSpawn = &FPostSpawnAccess::PostSpawnActor;
	};

	/** Rebuild @p Body now (shape, collision, zone, water info mesh) and tell its modifiers, as the editor does after an edit. */
	void RebuildBody(AWaterBody* Body)
	{
		UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		if (!Component)
			return;
		FOnWaterBodyChangedParams Params;
		Params.bShapeOrPositionChanged = true;
		Params.bWeightmapSettingsChanged = true;
		Params.bUserTriggered = true;
		Component->OnWaterBodyChanged(Params);
	}

	/**
	 * Spawn @p Class the way the Place Actors panel does (its actor factory applies the Water defaults: material, waves,
	 * spline, zone fill), labelled @p Label (made unique). Null after Fail.
	 */
	AActor* SpawnWater(UWorld* World, UClass* Class, const FVector& Location, const FString& Label)
	{
		UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryForActorClass(Class) : nullptr;
		if (!Factory)
			return UnrealMcpRql::SpawnActor(Class, Location, FRotator::ZeroRotator, Label); // Fails itself
		ULevel* Level = World->GetCurrentLevel() ? World->GetCurrentLevel() : World->PersistentLevel.Get();
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transactional;
		// The class is the placed "asset", as in Place Actors.
		AActor* Actor = Factory->CreateActor(Class, Level, FTransform(Location), Params);
		if (!Actor)
			return UnrealMcpRql::Failed<AActor*>(FString::Printf(TEXT("could not spawn %s"), *Class->GetName()));
		if (!Label.IsEmpty())
			FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
		return Actor;
	}

	/** Give @p Body the Place Actors defaults of its type keeping its spline points; false when the Water plugin has no factory for it. */
	bool ApplyWaterDefaults(AWaterBody* Body)
	{
		UActorFactory* Factory = GEditor && Body ? GEditor->FindActorFactoryForActorClass(Body->GetClass()) : nullptr;
		UWaterSplineComponent* Spline = Body ? Body->GetWaterSpline() : nullptr;
		if (!Factory || !Spline)
			return false;
		TArray<FSplinePoint> Points;
		for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
			Points.Add(Spline->GetSplinePointAt(Index, ESplineCoordinateSpace::Local));
		Body->Modify();
		Body->GetWaterBodyComponent()->Modify();
		Spline->Modify();
		(Factory->*FPostSpawnAccess::PostSpawn)(Body->GetClass(), Body);
		if (Points.Num())
		{
			Spline->ClearSplinePoints(false);
			Spline->AddPoints(Points, false);
			Spline->UpdateSpline();
			Spline->SynchronizeWaterProperties();
		}
		RebuildBody(Body);
		return true;
	}

	bool RequireWaterMaterial(UMaterialInterface* Material)
	{
		return IsWaterMaterial(Material) || UnrealMcpRql::Failed<bool>(FString::Printf(
			TEXT("%s is not a water material (it needs \"Used with Water\"): use /Water/Materials/WaterSurface/Water_Material, or pass no material for the Water default"),
			Material ? *Material->GetPathName() : TEXT("null")));
	}

	/** A label and an optional water material; false after Fail. */
	bool ValidBody(const FString& Label, UMaterialInterface* Material)
	{
		if (Label.TrimStartAndEnd().IsEmpty())
			return UnrealMcpRql::Failed<bool>(TEXT("give the water body a label (found again with unreal.find(\"<label>\"))"));
		return !Material || RequireWaterMaterial(Material);
	}

	bool ValidDepth(double Depth)
	{
		return Depth == 0.0 || (Depth >= 10.0 && Depth <= 20000.0)
			|| UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("Depth %g is outside 10 .. 20000 cm (0 keeps the Water default)"), Depth));
	}

	/** Give @p Body @p Material (null keeps its own); the caller rebuilds the body. */
	void SetBodyMaterial(AWaterBody* Body, UMaterialInterface* Material)
	{
		if (!Material)
			return;
		UWaterBodyComponent* Component = Body->GetWaterBodyComponent();
		Component->Modify();
		Component->SetWaterMaterial(Material);
	}

	/** Place @p Points (world) on @p Body's spline, quietly (RebuildBody rebuilds it once, with its final profile). */
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

	/**
	 * Create or resize @p World's water zone: SizeMeters 0 covers every Mesh Terrain plus 25% (Center ignored; refused
	 * without one), else a square SizeMeters wide around Center. Refits the ocean. Null after Fail.
	 */
	AWaterZone* FitZone(UWorld* World, double SizeMeters, FVector Center)
	{
		if (!FMath::IsFinite(SizeMeters) || Center.ContainsNaN())
			return UnrealMcpRql::Failed<AWaterZone*>(TEXT("SizeMeters and Center must be finite numbers"));
		double Extent = SizeMeters * 100.0;
		if (SizeMeters <= 0.0)
		{
			const FBox Bounds = TerrainsBounds(World);
			if (!Bounds.IsValid)
				return UnrealMcpRql::Failed<AWaterZone*>(TEXT("there is no Mesh Terrain to cover: pass SizeMeters (and Center)"));
			Extent = FMath::Max(Bounds.GetSize().X, Bounds.GetSize().Y) * 1.25;
			Center = Bounds.GetCenter();
		}
		else if (SizeMeters < 10.0 || SizeMeters > 100000.0)
		{
			return UnrealMcpRql::Failed<AWaterZone*>(FString::Printf(TEXT("SizeMeters %g is outside 10 .. 100000 (0 covers the Mesh Terrains)"), SizeMeters));
		}
		AWaterZone* Zone = Cast<AWaterZone>(FindFirst(World, AWaterZone::StaticClass()));
		if (!Zone)
			Zone = Cast<AWaterZone>(SpawnWater(World, AWaterZone::StaticClass(), FVector(Center.X, Center.Y, 0.0), TEXT("WaterZone")));
		if (!Zone)
			return nullptr;
		Zone->Modify();
		Zone->SetActorLocation(FVector(Center.X, Center.Y, Zone->GetActorLocation().Z));
		Zone->SetZoneExtent(FVector2D(Extent, Extent));
		RefitOcean(Cast<AWaterBodyOcean>(FindFirst(World, AWaterBodyOcean::StaticClass())), Zone);
		return Zone;
	}

	/** @p World's water zone, made when there is none (over the Mesh Terrains, else 2 km around @p Center). Null after Fail. */
	AWaterZone* EnsureZone(UWorld* World, const FVector& Center)
	{
		if (AWaterZone* Zone = Cast<AWaterZone>(FindFirst(World, AWaterZone::StaticClass())))
			return Zone;
		return FitZone(World, TerrainsBounds(World).IsValid ? 0.0 : 2000.0, Center);
	}

	/** Fail unless some of @p Points (X, Y) lie on @p Terrain's area (a body off the terrain shapes nothing). */
	bool CheckOnTerrain(const AMeshPartition* Terrain, TConstArrayView<FVector> Points, const TCHAR* What)
	{
		const FBox Box = TerrainBounds(Terrain);
		if (!Box.IsValid)
			return true; // nothing to compare with (no base mesh): the build says so
		const FBox2D Area(FVector2D(Box.Min), FVector2D(Box.Max));
		for (const FVector& Point : Points)
		{
			if (Area.IsInside(FVector2D(Point)))
				return true;
		}
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is off %s, which covers X %.0f .. %.0f, Y %.0f .. %.0f (cm)"),
			What, *Terrain->GetActorNameOrLabel(), Area.Min.X, Area.Max.X, Area.Min.Y, Area.Max.Y));
	}

	/**
	 * Warn (the call still succeeds) where the water surface is well above the terrain's ground (the modifier raises the
	 * ground around the water up to the surface: a lake on a plateau, a river on a causeway over the seabed) or far below it
	 * (a deep trench): read before the body exists, so the ground is the terrain without it.
	 */
	void WarnGround(const AMeshPartition* Terrain, TConstArrayView<FVector> Points)
	{
		constexpr double Above = 200.0, Below = 3000.0;
		const TArray<UPrimitiveComponent*> Colliders = TerrainColliders(Terrain);
		TArray<FString> High, Low;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			const TOptional<double> Ground = GroundHeight(Colliders, Points[Index].X, Points[Index].Y);
			if (!Ground)
				continue;
			const double Over = Points[Index].Z - Ground.GetValue();
			if (Over > Above)
				High.Add(FString::Printf(TEXT("[%.0f, %.0f] %.0f cm above"), Points[Index].X, Points[Index].Y, Over));
			else if (-Over > Below)
				Low.Add(FString::Printf(TEXT("[%.0f, %.0f] %.0f cm below"), Points[Index].X, Points[Index].Y, -Over));
		}
		if (!High.IsEmpty())
		{
			UnrealMcpRql::Warn(FString::Printf(TEXT("the water surface is above the ground at %s: its modifier raises the ground around the water to the surface there (a plateau or a causeway); put the surface at or a little below the ground (meshTerrain | call Surface), and end a river at the coast, not out over the seabed"),
				*FString::Join(High, TEXT(", "))));
		}
		if (!Low.IsEmpty())
		{
			UnrealMcpRql::Warn(FString::Printf(TEXT("the water surface is far below the ground at %s: its modifier digs the ground down to it (a deep trench or pit)"),
				*FString::Join(Low, TEXT(", "))));
		}
	}

	/** Add the terrain @p Modifier shapes (when it still exists) to @p OutTerrains. */
	void AddAffected(const UWaterModifier* Modifier, TArray<AMeshPartition*>& OutTerrains)
	{
		if (AMeshPartition* Terrain = Modifier->GetAffectedMeshPartition(); IsValid(Terrain))
			OutTerrains.AddUnique(Terrain);
	}

	/**
	 * Remove @p Modifier from its actor (the component only; the body stays). Its terrain rebuilds only the area it shaped:
	 * the change is queued before the component goes, and the build wait's first UpdateModifierList drops it from the stack.
	 */
	void RemoveModifier(UWaterModifier* Modifier)
	{
		Modifier->OnChanged(Modifier->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
		AActor* Owner = Modifier->GetOwner();
		Owner->Modify();
		Modifier->Modify();
		Owner->RemoveInstanceComponent(Modifier);
		Modifier->DestroyComponent();
	}

	/**
	 * Give @p Body exactly one water modifier, of its kind, bound to @p Terrain and enabled, with Affects Landscape on (the
	 * modifier does nothing without it). @p OutLeft gains the terrains it was rebound away from: they rebuild whole (the
	 * modifier's change reaches only its new terrain; an extra modifier removed queues its own area). Null after Fail.
	 */
	UWaterModifier* BindModifier(AWaterBody* Body, AMeshPartition* Terrain, TArray<AMeshPartition*>& OutLeft)
	{
		UClass* Class = ModifierClassFor(Body);
		if (!Class)
		{
			const FString Type = WaterType(Body);
			return UnrealMcpRql::Failed<UWaterModifier*>(FString::Printf(TEXT("%s is %s %s: only lakes and rivers shape a Mesh Terrain (an ocean needs no modifier: its spline only crops the water under the terrain; raise land above it with meshTerrainModifier | call AddPatch)"),
				*Body->GetActorNameOrLabel(), Type == TEXT("Ocean") ? TEXT("an") : TEXT("a"), *Type));
		}
		UWaterModifier* Kept = MatchingModifier(Body);
		for (UWaterModifier* Modifier : WaterModifiers(Body))
		{
			if (Modifier != Kept)
				RemoveModifier(Modifier);
		}
		UWaterBodyComponent* Component = Body->GetWaterBodyComponent();
		if (!Component->bAffectsLandscape)
		{
			Component->Modify();
			Component->bAffectsLandscape = true;
		}
		if (!Kept)
		{
			// No name: a modifier removed earlier (Detach) may still hold its class name until garbage collection.
			Kept = Cast<UWaterModifier>(UnrealMcpRql::AddComponent(Body, Class));
			if (!Kept)
				return nullptr;
		}
		AMeshPartition* Previous = Kept->GetAffectedMeshPartition();
		Kept->Modify();
		if (Kept->GetIsDisabledFlag())
			Kept->SetIsDisabledFlag(false);
		if (Previous != Terrain)
		{
			Kept->BP_SetAffectedMegaMesh(Terrain);
			if (IsValid(Previous))
				OutLeft.AddUnique(Previous);
		}
		RebuildBody(Body); // its spline, then the modifier hears the change
		Kept->OnChanged(Kept->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
		return Kept;
	}

	/**
	 * Wait for @p Terrains within @p TimeoutSeconds in all, first rebuilding every section of those also in @p RebuildWhole
	 * (a terrain a body was rebound away from): the result's Seconds / State (Ready from the body's Problem: the caller
	 * fills it).
	 */
	void WaitTerrains(TConstArrayView<AMeshPartition*> Terrains, double TimeoutSeconds, FUnrealAIMeshTerrainWaterResult& Out,
		TConstArrayView<AMeshPartition*> RebuildWhole = {})
	{
		const double Start = FPlatformTime::Seconds();
		TArray<FString> Pending;
		for (AMeshPartition* Terrain : Terrains)
		{
			UMeshPartitionEditorComponent* Component = IsValid(Terrain) ? EditorComponent(Terrain) : nullptr;
			if (!Component)
				continue;
			if (RebuildWhole.Contains(Terrain))
				Component->ForceRebuildAllSections(UE::MeshPartition::EChangeType::StateChange); // WaitForBuild updates its modifier list first
			FString Still;
			if (!WaitForBuild(Terrain, FMath::Max(MinTimeoutSeconds, TimeoutSeconds - (FPlatformTime::Seconds() - Start)), Still))
				Pending.Add(Still);
		}
		Out.Seconds = SecondsSince(Start);
		Out.State = Pending.IsEmpty() ? FString(TEXT("built"))
			: FString::Printf(TEXT("applied (one undo step), %s: call meshTerrainWater | call WaitForBuild(body) before reading heights"), *FString::Join(Pending, TEXT("; ")));
	}

	/** The result fields of @p Body (Body, Label, Type, Terrain, Modifier, Location, Problem, Ready). */
	void Describe(AWaterBody* Body, FUnrealAIMeshTerrainWaterResult& Out)
	{
		Out.Body = Body;
		Out.Label = Body->GetActorLabel();
		Out.Type = WaterType(Body);
		UWaterModifier* Modifier = MatchingModifier(Body);
		const AMeshPartition* Terrain = Modifier ? Modifier->GetAffectedMeshPartition() : nullptr;
		Out.Terrain = IsValid(Terrain) ? Terrain->GetActorLabel() : FString();
		Out.Modifier = Modifier;
		Out.Location = Body->GetActorLocation();
		Out.Problem = Problem(Body);
		Out.Ready = Out.Problem.IsEmpty();
	}

	/** A body the operations can change (in the open level). False after Fail. */
	bool CheckBody(const AWaterBody* Body)
	{
		if (!CheckAvailable())
			return false;
		if (!Body)
			return UnrealMcpRql::Failed<bool>(TEXT("Body is null: pass unreal.find(\"<label>\") of a water body (meshTerrainWaterBodies | select Label)"));
		if (!IsValid(Body) || Body->GetWorld() != UnrealMcpRql::EditorWorld())
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not a water body of the open level"), *Body->GetActorNameOrLabel()));
		if (!Body->GetWaterBodyComponent())
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no water body component"), *Body->GetActorNameOrLabel()));
		return true;
	}

	/**
	 * Spawn a lake or river for @p Terrain at @p Location with its spline through @p Points (world) and bind its modifier;
	 * the shared part of CreateLake / CreateRiver. @p Depth > 0 is its ChannelDepth; @p Shape sets the rest of its profile
	 * (a river's width) before it is rebuilt.
	 */
	FUnrealAIMeshTerrainWaterResult CreateShaping(UClass* Class, AMeshPartition* Terrain, const FString& Label, const FVector& Location,
		const TArray<FVector>& Points, double Depth, TFunctionRef<void(AWaterBody*)> Shape, UMaterialInterface* Material, double TimeoutSeconds)
	{
		FUnrealAIMeshTerrainWaterResult Out;
		UWorld* World = UnrealMcpRql::EditorWorld();
		WarnGround(Terrain, Points);
		if (!EnsureZone(World, Location))
			return Out;
		const FScopedTransaction Transaction(FText::Format(LOCTEXT("CreateWater", "Create {0} on Mesh Terrain"), FText::FromString(Class->GetName())));
		AWaterBody* Body = Cast<AWaterBody>(SpawnWater(World, Class, Location, Label));
		if (!Body)
			return Out;
		SetSpline(Body, Points);
		if (Depth > 0.0)
		{
			UWaterBodyComponent* Component = Body->GetWaterBodyComponent();
			Component->Modify();
			Component->CurveSettings.ChannelDepth = static_cast<float>(Depth);
		}
		Shape(Body);
		SetBodyMaterial(Body, Material);
		TArray<AMeshPartition*> Left; // stays empty: a new body was bound to no terrain
		if (!BindModifier(Body, Terrain, Left))
		{
			World->EditorDestroyActor(Body, true); // no half-made body
			return Out;
		}
		WaitTerrains({ Terrain }, TimeoutSeconds, Out);
		Describe(Body, Out);
		return Out;
	}
}

FUnrealAIMeshTerrainWaterResult UUnrealAIMeshTerrainWaterRqlApi::CreateLake(UE::MeshPartition::AMeshPartition* Terrain, const FString& Label, FVector Center,
	double Radius, double Depth, UMaterialInterface* Material, double TimeoutSeconds)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckTerrain(Terrain) || !ValidBody(Label, Material) || !ValidDepth(Depth) || !CheckTimeout(TimeoutSeconds))
		return {};
	if (Center.ContainsNaN())
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(TEXT("Center must be finite numbers"));
	if (!(Radius >= 100.0 && Radius <= 200000.0))
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(FString::Printf(TEXT("Radius %g is outside 100 .. 200000 cm"), Radius));
	if (!CheckOnTerrain(Terrain, { Center }, TEXT("the lake's Center")))
		return {};
	TArray<FVector> Shore;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const double Angle = UE_DOUBLE_TWO_PI * Index / 8.0;
		Shore.Add(Center + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0));
	}
	// The lake's actor sits at its centre, its spline is the shore around it.
	return CreateShaping(AWaterBodyLake::StaticClass(), Terrain, Label, Center, Shore, Depth, [](AWaterBody*) {}, Material, TimeoutSeconds);
}

FUnrealAIMeshTerrainWaterResult UUnrealAIMeshTerrainWaterRqlApi::CreateRiver(UE::MeshPartition::AMeshPartition* Terrain, const FString& Label,
	const TArray<FVector>& Points, double Width, double Depth, UMaterialInterface* Material, double TimeoutSeconds)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckTerrain(Terrain) || !ValidBody(Label, Material) || !ValidDepth(Depth) || !CheckTimeout(TimeoutSeconds))
		return {};
	if (!(Width >= 100.0 && Width <= 20000.0))
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(FString::Printf(TEXT("Width %g is outside 100 .. 20000 cm"), Width));
	if (Points.Num() < 2 || Points.Num() > 200)
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(FString::Printf(TEXT("a river needs 2 .. 200 points (upstream first), got %d"), Points.Num()));
	if (Points.ContainsByPredicate([](const FVector& Point) { return Point.ContainsNaN(); }))
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(TEXT("river points must be finite numbers"));
	if (!CheckOnTerrain(Terrain, Points, TEXT("every river point")))
		return {};
	return CreateShaping(AWaterBodyRiver::StaticClass(), Terrain, Label, Points[0], Points, Depth, [Width, Depth](AWaterBody* River)
	{
		// The river's per-point width and depth (the Water metadata curves).
		if (UWaterSplineMetadata* Metadata = River->GetWaterBodyComponent()->GetWaterSplineMetadata())
		{
			Metadata->Modify();
			for (FInterpCurvePoint<float>& Point : Metadata->RiverWidth.Points)
				Point.OutVal = static_cast<float>(Width);
			if (Depth > 0.0)
			{
				for (FInterpCurvePoint<float>& Point : Metadata->Depth.Points)
					Point.OutVal = static_cast<float>(Depth);
			}
		}
	}, Material, TimeoutSeconds);
}

FUnrealAIMeshTerrainWaterResult UUnrealAIMeshTerrainWaterRqlApi::CreateOcean(const FString& Label, double Height, UMaterialInterface* Material)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckAvailable() || !ValidBody(Label, Material))
		return {};
	if (!FMath::IsFinite(Height) || FMath::Abs(Height) > 1000000.0)
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(TEXT("Height must be within -1000000 .. 1000000 cm"));
	UWorld* World = UnrealMcpRql::EditorWorld();
	if (const AActor* Existing = FindFirst(World, AWaterBodyOcean::StaticClass()))
	{
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(FString::Printf(
			TEXT("the level already has an ocean (%s; one per world): move it with set Location.Z, or meshTerrainWater | call Delete(unreal.find(\"%s\")) first"),
			*Existing->GetActorLabel(), *Existing->GetActorLabel()));
	}
	const FScopedTransaction Transaction(LOCTEXT("CreateOcean", "Create Ocean on Mesh Terrain"));
	const AWaterZone* Zone = EnsureZone(World, FVector::ZeroVector);
	if (!Zone)
		return {};
	// The ocean's actor factory fills the zone (collision and ocean extents), as the Place Actors panel does.
	AWaterBodyOcean* Ocean = Cast<AWaterBodyOcean>(SpawnWater(World, AWaterBodyOcean::StaticClass(),
		FVector(Zone->GetActorLocation().X, Zone->GetActorLocation().Y, Height), Label));
	if (!Ocean)
		return {};
	SetBodyMaterial(Ocean, Material);
	RebuildBody(Ocean);
	FUnrealAIMeshTerrainWaterResult Out;
	Out.State = TEXT("built");
	Describe(Ocean, Out);
	return Out;
}

FUnrealAIMeshTerrainWaterResult UUnrealAIMeshTerrainWaterRqlApi::Attach(AWaterBody* Body, UE::MeshPartition::AMeshPartition* Terrain, double TimeoutSeconds)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckBody(Body) || !CheckTerrain(Terrain) || !CheckTimeout(TimeoutSeconds))
		return {};
	TArray<FVector> Points;
	if (const UWaterSplineComponent* Spline = Body->GetWaterSpline())
	{
		for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
			Points.Add(Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World));
	}
	Points.Add(Body->GetActorLocation());
	const bool bShapes = ModifierClassFor(Body) != nullptr; // else BindModifier refuses it
	if (bShapes && !CheckOnTerrain(Terrain, Points, *Body->GetActorNameOrLabel()))
		return {};
	if (bShapes && Body->GetWaterBodyComponent()->CurveSettings.ChannelDepth <= 0.f)
	{
		UnrealMcpRql::Warn(FString::Printf(TEXT("%s's ChannelDepth is 0: its bed is not dug below the water surface (set WaterBodyComponent.CurveSettings.ChannelDepth, or SetMaterial(body) for the Water defaults, 500 cm for a lake)"),
			*Body->GetActorNameOrLabel()));
	}
	const FScopedTransaction Transaction(LOCTEXT("AttachWater", "Attach Water Body to Mesh Terrain"));
	TArray<AMeshPartition*> Left;
	if (!BindModifier(Body, Terrain, Left))
		return {};
	TArray<AMeshPartition*> Changed = Left;
	Changed.AddUnique(Terrain);
	FUnrealAIMeshTerrainWaterResult Out;
	WaitTerrains(Changed, TimeoutSeconds, Out, Left);
	Describe(Body, Out);
	return Out;
}

FUnrealAIMeshTerrainWaterResult UUnrealAIMeshTerrainWaterRqlApi::Detach(AWaterBody* Body, double TimeoutSeconds)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckBody(Body) || !CheckTimeout(TimeoutSeconds))
		return {};
	const TArray<UWaterModifier*> Modifiers = WaterModifiers(Body);
	if (Modifiers.IsEmpty())
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(FString::Printf(TEXT("%s has no Mesh Partition water modifier to remove"), *Body->GetActorNameOrLabel()));
	const FScopedTransaction Transaction(LOCTEXT("DetachWater", "Detach Water Body from Mesh Terrain"));
	TArray<AMeshPartition*> Changed;
	for (UWaterModifier* Modifier : Modifiers)
	{
		AddAffected(Modifier, Changed);
		RemoveModifier(Modifier);
	}
	FUnrealAIMeshTerrainWaterResult Out;
	WaitTerrains(Changed, TimeoutSeconds, Out);
	Describe(Body, Out);
	return Out;
}

FUnrealAIMeshTerrainWaterResult UUnrealAIMeshTerrainWaterRqlApi::Delete(AWaterBody* Body, double TimeoutSeconds)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckBody(Body) || !CheckTimeout(TimeoutSeconds))
		return {};
	FUnrealAIMeshTerrainWaterResult Out;
	Out.Label = Body->GetActorLabel();
	Out.Type = WaterType(Body);
	TArray<AMeshPartition*> Changed;
	{
		const FScopedTransaction Transaction(LOCTEXT("DeleteWater", "Delete Water Body"));
		// Its modifiers go first, each queuing a rebuild of the area it shaped (as Detach), then the body.
		for (UWaterModifier* Modifier : WaterModifiers(Body))
		{
			AddAffected(Modifier, Changed);
			RemoveModifier(Modifier);
		}
		if (!Body->GetWorld()->EditorDestroyActor(Body, /*bShouldModifyLevel*/ true))
			return UnrealMcpRql::Failed<FUnrealAIMeshTerrainWaterResult>(FString::Printf(TEXT("could not delete %s"), *Out.Label));
	}
	Out.Terrain = Changed.IsEmpty() ? FString() : Changed[0]->GetActorLabel();
	WaitTerrains(Changed, TimeoutSeconds, Out);
	Out.Ready = Out.State == TEXT("built");
	if (!Out.Ready)
		Out.Problem = Out.State;
	return Out;
}

FUnrealAIMeshTerrainWaterResult UUnrealAIMeshTerrainWaterRqlApi::WaitForBuild(AWaterBody* Body, double TimeoutSeconds)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckBody(Body) || !CheckTimeout(TimeoutSeconds))
		return {};
	FUnrealAIMeshTerrainWaterResult Out;
	if (AMeshPartition* Terrain = TerrainOf(Body))
	{
		LoadTerrainActors(Terrain);
		WaitTerrains({ Terrain }, TimeoutSeconds, Out);
	}
	else
	{
		Out.State = TEXT("built");
	}
	Describe(Body, Out);
	return Out;
}

AWaterZone* UUnrealAIMeshTerrainWaterRqlApi::SetZone(double SizeMeters, FVector Center)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckAvailable())
		return nullptr;
	const FScopedTransaction Transaction(LOCTEXT("SetZone", "Set Water Zone"));
	return FitZone(UnrealMcpRql::EditorWorld(), SizeMeters, Center);
}

AWaterBody* UUnrealAIMeshTerrainWaterRqlApi::SetMaterial(AWaterBody* Body, UMaterialInterface* Material)
{
	using namespace UnrealAIMeshTerrainWaterApiLocal;
	if (!CheckBody(Body) || (Material && !RequireWaterMaterial(Material)))
		return nullptr;
	const FScopedTransaction Transaction(LOCTEXT("SetMaterial", "Set Water Material"));
	if (!EnsureZone(Body->GetWorld(), Body->GetActorLocation()))
		return nullptr;
	UWaterBodyComponent* Component = Body->GetWaterBodyComponent();
	if (Material)
	{
		SetBodyMaterial(Body, Material);
		RebuildBody(Body);
	}
	else if (!ApplyWaterDefaults(Body)) // rebuilds it
	{
		return UnrealMcpRql::Failed<AWaterBody*>(FString::Printf(TEXT("the Water plugin has no Place Actors defaults for %s: pass a material"), *Body->GetClass()->GetName()));
	}
	return Component->GetWaterMaterial() ? Body
		: UnrealMcpRql::Failed<AWaterBody*>(TEXT("the Water plugin has no default water material (Project Settings > Water Editor): pass one, e.g. /Water/Materials/WaterSurface/Water_Material"));
}

FString UUnrealAIMeshTerrainWaterRqlApi::EnableMeshTerrainWater()
{
	// Every plugin is checked before the project file changes: nothing is half enabled on an older engine.
	TArray<FString> Unloaded;
	for (const TCHAR* Name : UnrealAIMeshTerrainWater::RequiredPlugins)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
		if (!Plugin.IsValid())
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("this engine has no %s plugin: water on Mesh Terrain needs Unreal Engine 5.8 or later"), Name));
		if (!Plugin->IsEnabled())
			Unloaded.Add(Name);
	}
	const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
	TArray<FString> Added;
	FText Reason;
	for (const TCHAR* Name : UnrealAIMeshTerrainWater::RequiredPlugins)
	{
		if (Project && Project->Plugins.ContainsByPredicate([Name](const FPluginReferenceDescriptor& Entry) { return Entry.Name == Name && Entry.bEnabled; }))
			continue;
		if (!IProjectManager::Get().SetPluginEnabled(Name, true, Reason))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not enable %s in the project file: %s"), Name, *Reason.ToString()));
		Added.Add(Name);
	}
	if (!Added.IsEmpty() && !IProjectManager::Get().SaveCurrentProjectToDisk(Reason))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not save the project file: %s"), *Reason.ToString()));
	const FString Listed = Added.IsEmpty() ? FString(TEXT("MeshPartition, MeshPartitionWater and Water are enabled in the project file"))
		: FString::Printf(TEXT("enabled %s in the project file (packaged games and cooks include them)"), *FString::Join(Added, TEXT(", ")));
	const FString Loaded = Unloaded.IsEmpty() ? FString(TEXT("loaded now, no restart needed"))
		: FString::Printf(TEXT("restart the editor to load %s"), *FString::Join(Unloaded, TEXT(", ")));
	return Listed + TEXT("; ") + Loaded;
}

#undef LOCTEXT_NAMESPACE
