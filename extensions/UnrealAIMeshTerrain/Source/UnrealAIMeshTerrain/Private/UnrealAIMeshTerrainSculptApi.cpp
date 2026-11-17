// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainSculptApi.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "IUnrealMcpRqlModule.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"

#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionModifierToolTarget.h"
#include "Modifiers/MeshPartitionMeshProvider.h"
#include "Modifiers/MeshPartitionProjectSculptLayersModifier.h"

/*
 * How a stroke is stored (verified in a UE 5.8 editor): Mesh Terrain Mode's Sculpt and Paint tools
 * edit a brush modifier (UProjectMeshLayersModifier, "BrushModifier") through UEditableModifierToolTarget: GetDynamicMesh()
 * is the terrain inside the brush's edit box (EditExtents = HALF size around the component, in its local space) as the
 * modifiers before it left it, with the brush's sculpt layers enabled (layer 0 the base, the active layer the one edited)
 * and a weight attribute per channel entry of the brush; CommitDynamicMesh() stores it in the brush (ApplyEditWithMesh).
 * The tool and these operations then differ only in how the vertices move. Growing EditExtents keeps what was sculpted.
 */
namespace UnrealAIMeshTerrainSculptLocal
{
	using namespace UnrealAIMeshTerrain;
	using UE::MeshPartition::UProjectMeshLayersModifier;
	using UE::MeshPartition::FSculptLayerModifierWeightAttributeEntry;
	using UE::Geometry::FDynamicMesh3;

	constexpr double MaxRadius = 25000.0;            // cm: a 500 m stroke
	constexpr double MaxRaise = 100000.0;            // cm
	constexpr double MaxBrushHalfExtent = 30000.0;   // cm: a brush modifier covers at most 600 x 600 m
	constexpr double MinBrushHalfExtent = 2500.0;    // cm: a new brush covers at least 50 x 50 m, so nearby strokes reuse it
	constexpr double MaxVertices = 1000000.0;        // terrain vertices a brush modifier may hold (its edit mesh)
	constexpr int32 SmoothIterations = 4;

	enum class EStroke { Raise, Lower, Flatten, Smooth, Paint };

	struct FStroke
	{
		EStroke Kind = EStroke::Raise;
		FVector2D Point = FVector2D::ZeroVector;
		double Radius = 0.0;
		double Amount = 0.0;   // Raise / Lower: cm; Flatten: world Z; Paint: the target weight
		double Strength = 1.0;
		double Falloff = 0.5;
		FName Channel;
	};

	/** 1 at @p T = 0, a smoothstep fall to 0 at @p T = 1. */
	double SmoothFall(double T)
	{
		return 1.0 - T * T * (3.0 - 2.0 * T);
	}

	/** 1 inside the brush's core, fading smoothly to 0 at its Radius over the outer Falloff part. */
	double BrushWeight(double Distance, double Radius, double Falloff)
	{
		if (Distance >= Radius)
			return 0.0;
		const double Inner = Radius * (1.0 - Falloff);
		if (Distance <= Inner)
			return 1.0;
		return SmoothFall((Distance - Inner) / (Radius - Inner));
	}

	template <typename T>
	T* PropertyValue(UObject* Object, const TCHAR* Name)
	{
		const FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(Name) : nullptr;
		return Property ? Property->ContainerPtrToValuePtr<T>(Object) : nullptr;
	}

	bool BoolProperty(UObject* Object, const TCHAR* Name)
	{
		const FBoolProperty* Property = Object ? CastField<FBoolProperty>(Object->GetClass()->FindPropertyByName(Name)) : nullptr;
		return Property && Property->GetPropertyValue_InContainer(Object);
	}

	/** Set the (private) UPROPERTY @p Name of @p Object the way the details panel does, so the modifier updates and rebuilds. */
	template <typename T>
	void SetProperty(UObject* Object, const TCHAR* Name, const T& Value)
	{
		FProperty* Property = Object->GetClass()->FindPropertyByName(Name);
		if (!Property)
			return;
		Object->PreEditChange(Property);
		*Property->ContainerPtrToValuePtr<T>(Object) = Value;
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Object->PostEditChangeProperty(Event);
	}

	FVector2D EditExtents(UProjectMeshLayersModifier* Brush)
	{
		const FVector2D* Extents = PropertyValue<FVector2D>(Brush, TEXT("EditExtents"));
		return Extents ? *Extents : FVector2D(500.0, 500.0);
	}

	const FSculptLayerModifierWeightAttributeEntry* ChannelEntry(const TArray<FSculptLayerModifierWeightAttributeEntry>& Entries, FName Channel)
	{
		return Entries.FindByPredicate([Channel](const FSculptLayerModifierWeightAttributeEntry& Entry) { return Entry.ChannelName.GetName() == Channel; });
	}

	/**
	 * Whether a stroke can go into @p Brush the way these operations edit it: enabled, upright and unscaled (the stroke is a
	 * world-space circle moved along world Z), projecting along a fixed direction, storing offsets (not absolute positions),
	 * a sculpt layer active (not the locked base) with a weight (else the layer cannot be edited), no channel scaling its
	 * sculpt layers (a Raise would move less than asked) and, to paint, a plain Set entry for the channel (or none yet).
	 */
	bool CanTakeStroke(UProjectMeshLayersModifier* Brush, const FStroke& Stroke)
	{
		const FTransform Transform = Brush->GetComponentTransform();
		const uint8* Method = PropertyValue<uint8>(Brush, TEXT("ProjectMethod"));
		if (Brush->IsDisabled() || !Transform.GetRotation().Equals(FQuat::Identity, 1e-4) || !Transform.GetScale3D().Equals(FVector::OneVector, 1e-4)
			|| !Method || *Method != static_cast<uint8>(UE::MeshPartition::ESculptLayerProjectMethod::FixedDirection) || BoolProperty(Brush, TEXT("bSculptAbsolutePositions")))
			return false;
		const int32 Layer = Brush->GetActiveLayer() - 1; // LayerWeights leaves out the base layer (0)
		if (Layer < 0 || (Brush->LayerWeights.IsValidIndex(Layer) && FMath::IsNearlyZero(Brush->LayerWeights[Layer])))
			return false;
		const TArray<FSculptLayerModifierWeightAttributeEntry> Entries = Brush->GetChannelEntries(); // a copy: returned by value
		if (Entries.ContainsByPredicate([](const FSculptLayerModifierWeightAttributeEntry& Entry) { return Entry.bScaleSculptLayers; }))
			return false;
		const FSculptLayerModifierWeightAttributeEntry* Entry = Stroke.Kind == EStroke::Paint ? ChannelEntry(Entries, Stroke.Channel) : nullptr;
		return !Entry || (Entry->Method == UE::MeshPartition::ESculptLayerSetWeightChannelMethod::Set && !Entry->bInternalChannel
			&& FMath::IsNearlyEqual(Entry->BlendWeight, 1.0f));
	}

	/** The terrain's vertices per cm² (the densest base mesh near @p Area): to bound the size of a brush's edit mesh. */
	double VertexDensity(const AMeshPartition* Terrain, const FBox2D& Area)
	{
		double Density = 0.0;
		for (UModifierComponent* Modifier : Modifiers(Terrain, /*bBases*/ true))
		{
			const UE::MeshPartition::UMeshProviderModifier* Base = Cast<UE::MeshPartition::UMeshProviderModifier>(Modifier);
			const FDynamicMesh3* Mesh = Base ? Base->GetMesh() : nullptr;
			if (!Mesh || Mesh->VertexCount() == 0)
				continue;
			FBox Box(ForceInit);
			for (const FBox& Part : Base->ComputeBounds())
				Box += Part;
			const FBox2D Box2D(FVector2D(Box.Min), FVector2D(Box.Max));
			if (Box.IsValid && Box2D.Intersect(Area))
				Density = FMath::Max(Density, Mesh->VertexCount() / FMath::Max(Box2D.GetArea(), 1.0));
		}
		return Density;
	}

	/**
	 * Move the vertices of @p Mesh (in the brush's local space, @p Transform to world) that the stroke covers (or their
	 * channel weights); returns how many changed, NoVertex when no vertex is under the brush, NoWeightLayer when the mesh has
	 * no weight attribute for the painted channel.
	 */
	constexpr int32 NoVertex = -1;
	constexpr int32 NoWeightLayer = -2;

	int32 ApplyStroke(FDynamicMesh3& Mesh, const FTransform& Transform, const FStroke& Stroke, double& OutBefore, double& OutAfter, double& OutNearest)
	{
		const FVector Local = Transform.InverseTransformPosition(FVector(Stroke.Point, 0.0));
		const FVector2D Center(Local.X, Local.Y);
		TMap<int32, double> Weights;
		int32 Nearest = INDEX_NONE;
		double NearestDistance = TNumericLimits<double>::Max();
		// How far the nearest vertex is, under the brush or not (for the error when none is under it).
		OutNearest = TNumericLimits<double>::Max();
		for (const int32 Vertex : Mesh.VertexIndicesItr())
		{
			const double Distance = FVector2D::Distance(FVector2D(Mesh.GetVertex(Vertex)), Center);
			OutNearest = FMath::Min(OutNearest, Distance);
			const double Weight = BrushWeight(Distance, Stroke.Radius, Stroke.Falloff) * Stroke.Strength;
			if (Weight <= 0.0)
				continue;
			Weights.Add(Vertex, Weight);
			if (Distance < NearestDistance)
			{
				NearestDistance = Distance;
				Nearest = Vertex;
			}
		}
		if (Weights.IsEmpty())
			return NoVertex;

		if (Stroke.Kind == EStroke::Paint)
		{
			UE::Geometry::FDynamicMeshWeightAttribute* Channel = nullptr;
			UE::Geometry::FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
			for (int32 Index = 0; Attributes && Index < Attributes->NumWeightLayers(); ++Index)
			{
				if (Attributes->GetWeightLayer(Index)->GetName() == Stroke.Channel)
					Channel = Attributes->GetWeightLayer(Index);
			}
			if (!Channel)
				return NoWeightLayer;
			const auto Read = [Channel](int32 Vertex) { float Value = 0.0f; Channel->GetValue(Vertex, &Value); return Value; };
			OutBefore = Read(Nearest);
			int32 Changed = 0;
			for (const TPair<int32, double>& Item : Weights)
			{
				const float Old = Read(Item.Key);
				const float New = static_cast<float>(FMath::Lerp(static_cast<double>(Old), Stroke.Amount, Item.Value));
				if (New != Old)
				{
					Channel->SetValue(Item.Key, &New);
					++Changed;
				}
			}
			OutAfter = Read(Nearest);
			return Changed;
		}

		const double LocalZ = Transform.GetLocation().Z;
		OutBefore = Mesh.GetVertex(Nearest).Z + LocalZ;
		TMap<int32, double> Heights;
		for (const TPair<int32, double>& Item : Weights)
			Heights.Add(Item.Key, Mesh.GetVertex(Item.Key).Z);
		switch (Stroke.Kind)
		{
		case EStroke::Raise:
		case EStroke::Lower:
			for (TPair<int32, double>& Item : Heights)
				Item.Value += (Stroke.Kind == EStroke::Raise ? 1.0 : -1.0) * Stroke.Amount * Weights[Item.Key];
			break;
		case EStroke::Flatten:
			for (TPair<int32, double>& Item : Heights)
				Item.Value = FMath::Lerp(Item.Value, Stroke.Amount - LocalZ, Weights[Item.Key]);
			break;
		case EStroke::Smooth:
			for (int32 Iteration = 0; Iteration < SmoothIterations; ++Iteration)
			{
				TMap<int32, double> Next = Heights;
				for (TPair<int32, double>& Item : Next)
				{
					double Sum = 0.0;
					int32 Count = 0;
					for (const int32 Neighbour : Mesh.VtxVerticesItr(Item.Key))
					{
						const double* Height = Heights.Find(Neighbour);
						Sum += Height ? *Height : Mesh.GetVertex(Neighbour).Z;
						++Count;
					}
					if (Count > 0)
						Item.Value = FMath::Lerp(Heights[Item.Key], Sum / Count, Weights[Item.Key]);
				}
				Heights = MoveTemp(Next);
			}
			break;
		default:
			break;
		}
		int32 Changed = 0;
		for (const TPair<int32, double>& Item : Heights)
		{
			FVector3d Position = Mesh.GetVertex(Item.Key);
			if (FMath::IsNearlyEqual(Position.Z, Item.Value, 1e-3))
				continue;
			Position.Z = Item.Value;
			Mesh.SetVertex(Item.Key, Position);
			++Changed;
		}
		OutAfter = Mesh.GetVertex(Nearest).Z + LocalZ;
		return Changed;
	}

	FUnrealAIMeshTerrainStroke Failed(const FString& Message)
	{
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainStroke>(Message);
	}

	FUnrealAIMeshTerrainStroke Run(AMeshPartition* Terrain, const FStroke& Stroke, const FText& Description)
	{
		const double Start = FPlatformTime::Seconds();
		if (!FMath::IsFinite(Stroke.Point.X) || !FMath::IsFinite(Stroke.Point.Y) || !FMath::IsFinite(Stroke.Amount))
			return Failed(TEXT("Point and the amounts must be finite numbers"));
		if (!(Stroke.Radius >= 1.0 && Stroke.Radius <= MaxRadius))
			return Failed(FString::Printf(TEXT("Radius %g cm is outside 1 .. %g"), Stroke.Radius, MaxRadius));
		if (!(Stroke.Falloff >= 0.0 && Stroke.Falloff <= 1.0))
			return Failed(FString::Printf(TEXT("Falloff %g is outside 0 .. 1"), Stroke.Falloff));
		if (!(Stroke.Strength > 0.0 && Stroke.Strength <= 1.0))
			return Failed(FString::Printf(TEXT("Strength %g is outside 0 (exclusive) .. 1"), Stroke.Strength));
		if (!CheckTerrain(Terrain))
			return FUnrealAIMeshTerrainStroke();
		if (Stroke.Kind == EStroke::Paint)
		{
			const UMeshPartitionDefinition* Definition = Terrain->GetMeshPartitionDefinition();
			if (!Definition || !Definition->GetChannelNames().Contains(Stroke.Channel)) // FName: case-insensitive
			{
				return Failed(FString::Printf(TEXT("'%s' is not a paint channel of %s: its definition has %s"), *Stroke.Channel.ToString(), *Terrain->GetActorNameOrLabel(),
					*ChannelList(Terrain)));
			}
		}
		// Start from a built terrain: the surface height under the point places a new brush, the build bounds its depth.
		if (!WaitForBuild(Terrain))
			return FUnrealAIMeshTerrainStroke();
		const FBox Built = BuiltBounds(Terrain);
		const FBox2D Area(Stroke.Point - FVector2D(Stroke.Radius), Stroke.Point + FVector2D(Stroke.Radius));
		if (!Built.IsValid || !Area.Intersect(FBox2D(FVector2D(Built.Min), FVector2D(Built.Max))))
		{
			return Failed(Built.IsValid
				? FString::Printf(TEXT("the brush at X %.0f, Y %.0f (Radius %.0f cm) is off the terrain %s (it spans X %.0f .. %.0f, Y %.0f .. %.0f)"), Stroke.Point.X, Stroke.Point.Y,
					Stroke.Radius, *Terrain->GetActorNameOrLabel(), Built.Min.X, Built.Max.X, Built.Min.Y, Built.Max.Y)
				: FString::Printf(TEXT("%s has no built ground to sculpt (no base mesh?)"), *Terrain->GetActorNameOrLabel()));
		}
		const double StrokeDensity = VertexDensity(Terrain, Area);
		const double Spacing = StrokeDensity > 0.0 ? FMath::Sqrt(1.0 / StrokeDensity) : 100.0;
		const double Margin = 2.0 * Spacing + 100.0;
		const auto BoxAround = [](const FVector2D& Center, const FVector2D& HalfExtents) { return FBox2D(Center - HalfExtents, Center + HalfExtents); };
		// The vertices a brush box holds, from the densest base mesh it reaches.
		const auto Estimate = [Terrain](const FBox2D& Box) { return VertexDensity(Terrain, Box) * Box.GetArea(); };

		// The brush modifier the stroke goes into: one that covers the area already (the last in build order), else one that
		// overlaps it or is near (within MinBrushHalfExtent) and may grow to cover it (the smallest growth), else a new one.
		// Never one with an enabled modifier after it over its box: the stroke is made on the ground as the modifiers before
		// the brush leave it, so a later one would move it (a Flatten would not be flat).
		const TArray<UModifierComponent*> Stack = Modifiers(Terrain);
		const auto CoveredLater = [&Stack](int32 Index, const FBox2D& Box)
		{
			for (int32 Later = Index + 1; Later < Stack.Num(); ++Later)
			{
				const FBox Bounds = Stack[Later]->ComputeCombinedBounds();
				if (!Stack[Later]->GetIsDisabledFlag() && Bounds.IsValid && Box.Intersect(FBox2D(FVector2D(Bounds.Min), FVector2D(Bounds.Max))))
					return true;
			}
			return false;
		};
		UProjectMeshLayersModifier* Brush = nullptr;
		FVector2D Extents = FVector2D::ZeroVector;
		double BestGrowth = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Stack.Num(); ++Index)
		{
			UProjectMeshLayersModifier* Candidate = Cast<UProjectMeshLayersModifier>(Stack[Index]);
			if (!Candidate || !CanTakeStroke(Candidate, Stroke))
				continue;
			const FVector2D Center(Candidate->GetComponentLocation());
			const FVector2D Have = EditExtents(Candidate);
			const FVector2D Offset = (Stroke.Point - Center).GetAbs();
			if (Offset.X + Stroke.Radius <= Have.X && Offset.Y + Stroke.Radius <= Have.Y)
			{
				if (Estimate(BoxAround(Center, Have)) <= MaxVertices && !CoveredLater(Index, BoxAround(Center, Have)))
				{
					Brush = Candidate;
					Extents = Have;
					BestGrowth = 0.0;
				}
				continue;
			}
			if (BestGrowth == 0.0 || Offset.X - Stroke.Radius - Have.X > MinBrushHalfExtent || Offset.Y - Stroke.Radius - Have.Y > MinBrushHalfExtent)
				continue; // a covering brush was found, or this one is far from the stroke
			const FVector2D Need(FMath::Max(Have.X, Offset.X + Stroke.Radius + Margin), FMath::Max(Have.Y, Offset.Y + Stroke.Radius + Margin));
			const double Growth = Need.X * Need.Y - Have.X * Have.Y;
			if (Need.X <= MaxBrushHalfExtent && Need.Y <= MaxBrushHalfExtent && Growth < BestGrowth
				&& Estimate(BoxAround(Center, Need)) <= MaxVertices && !CoveredLater(Index, BoxAround(Center, Need)))
			{
				Brush = Candidate;
				Extents = Need;
				BestGrowth = Growth;
			}
		}
		const bool bNew = Brush == nullptr;
		if (bNew)
		{
			double Half = Stroke.Radius + Margin;
			if (Estimate(BoxAround(Stroke.Point, FVector2D(MinBrushHalfExtent))) <= MaxVertices / 4.0)
				Half = FMath::Max(Half, MinBrushHalfExtent);
			Extents = FVector2D(Half);
			const double Vertices = Estimate(BoxAround(Stroke.Point, Extents));
			if (Vertices > MaxVertices)
			{
				return Failed(FString::Printf(TEXT("the stroke would edit about %.0f terrain vertices (at most %.0f, the terrain has a vertex every %.0f cm): use a smaller Radius"),
					Vertices, MaxVertices, Spacing));
			}
		}

		FScopedTransaction Transaction(Description);
		UWorld* World = Terrain->GetWorld();
		AMeshPartition* Previous = nullptr; // the terrain the factory bound a new brush to first
		if (bNew)
		{
			// Placed the way Mesh Terrain Mode's modifier palette places one, on the ground under the point.
			const FSurfaceSample Ground = SampleSurface(Terrain, Stroke.Point.X, Stroke.Point.Y, /*bChannels*/ false);
			const FVector Location(Stroke.Point, Ground.bInside ? Ground.Height : Terrain->GetActorLocation().Z);
			Brush = Cast<UProjectMeshLayersModifier>(SpawnModifier(Terrain, UProjectMeshLayersModifier::StaticClass(), Location, FRotator::ZeroRotator,
				UniqueLabel(World, Terrain->GetActorLabel() + TEXT("_Brush")), Previous));
			if (!Brush)
			{
				Transaction.Cancel();
				return FUnrealAIMeshTerrainStroke(); // SpawnModifier failed with the reason
			}
			// Applied after every modifier there is.
			double Last = 0.0;
			for (const UModifierComponent* Modifier : Modifiers(Terrain))
			{
				if (Modifier != Brush && Modifier->GetType() == Brush->GetType())
					Last = FMath::Max(Last, Modifier->GetPriority() + 1.0);
			}
			Brush->SetPriority(Last);
		}
		AActor* Owner = Brush->GetOwner();
		const auto FloatProperty = [Brush](const TCHAR* Name) { const float* Value = PropertyValue<float>(Brush, Name); return Value ? *Value : 0.0f; };
		const FVector2D OldExtents = EditExtents(Brush);
		const float OldUp = FloatProperty(TEXT("VerticalExtentUp"));
		const float OldDown = FloatProperty(TEXT("VerticalExtentDown"));
		const TArray<FSculptLayerModifierWeightAttributeEntry> OldEntries = Brush->GetChannelEntries();
		Brush->Modify();
		if (Owner)
			Owner->Modify();
		const FObjectProperty* MeshProperty = CastField<FObjectProperty>(Brush->GetClass()->FindPropertyByName(TEXT("MeshObject")));
		if (UObject* MeshObject = MeshProperty ? MeshProperty->GetObjectPropertyValue_InContainer(Brush) : nullptr)
			MeshObject->Modify(); // the stored sculpt mesh: undo restores it
		if (EditExtents(Brush) != Extents)
			SetProperty(Brush, TEXT("EditExtents"), Extents);
		// Deep enough to reach all of the terrain's ground under the brush, and where the stroke moves it.
		const double BrushZ = Brush->GetComponentLocation().Z;
		const double Reach = Stroke.Kind == EStroke::Raise || Stroke.Kind == EStroke::Lower ? Stroke.Amount
			: Stroke.Kind == EStroke::Flatten ? FMath::Abs(Stroke.Amount - BrushZ) : 0.0;
		const float Depth = static_cast<float>(FMath::Max(Built.Max.Z - BrushZ, BrushZ - Built.Min.Z) + Reach + 10000.0);
		for (const TCHAR* Name : { TEXT("VerticalExtentUp"), TEXT("VerticalExtentDown") })
		{
			if (FloatProperty(Name) < Depth)
				SetProperty(Brush, Name, Depth);
		}
		if (Stroke.Kind == EStroke::Paint && !ChannelEntry(OldEntries, Stroke.Channel))
		{
			// What the Paint tool adds for a channel it paints: Set the brush's weights, unscaled. GetDynamicMesh then has a
			// weight attribute for it holding the channel as the modifiers before the brush leave it.
			TArray<FSculptLayerModifierWeightAttributeEntry> Entries = OldEntries;
			Entries.AddDefaulted_GetRef().ChannelName = Stroke.Channel;
			Brush->SetChannelEntries(Entries);
		}

		UE::MeshPartition::UEditableModifierToolTarget* Target = NewObject<UE::MeshPartition::UEditableModifierToolTarget>();
		Target->Initialize(Brush);
		FDynamicMesh3 Mesh = Target->GetDynamicMesh();
		// A stroke that stores nothing (it failed, or changes nothing) leaves no new brush behind and a reused one as it was.
		const auto Restore = [&]()
		{
			if (bNew)
			{
				if (Owner)
					World->EditorDestroyActor(Owner, /*bShouldModifyLevel*/ true);
				for (const AMeshPartition* Bound : { Terrain, Previous })
				{
					if (UMeshPartitionEditorComponent* Component = EditorComponent(Bound))
						Component->UpdateModifierList();
				}
				return;
			}
			Brush->SetChannelEntries(OldEntries);
			if (EditExtents(Brush) != OldExtents)
				SetProperty(Brush, TEXT("EditExtents"), OldExtents);
			if (FloatProperty(TEXT("VerticalExtentUp")) != OldUp)
				SetProperty(Brush, TEXT("VerticalExtentUp"), OldUp);
			if (FloatProperty(TEXT("VerticalExtentDown")) != OldDown)
				SetProperty(Brush, TEXT("VerticalExtentDown"), OldDown);
		};
		const auto Abandon = [&](const FString& Message)
		{
			Restore();
			return Failed(Message);
		};
		if (Mesh.VertexCount() > MaxVertices * 1.5)
			return Abandon(FString::Printf(TEXT("the brush area holds %d terrain vertices (at most %.0f): use a smaller Radius"), Mesh.VertexCount(), MaxVertices));
		UE::Geometry::FDynamicMeshSculptLayers* Layers = Mesh.HasAttributes() ? Mesh.Attributes()->GetSculptLayers() : nullptr;
		if (Stroke.Kind != EStroke::Paint && (!Layers || !Layers->IsEnabled()))
			return Abandon(FString::Printf(TEXT("the brush modifier %s has no sculpt layer to edit"), Owner ? *Owner->GetActorLabel() : TEXT("?")));

		FUnrealAIMeshTerrainStroke Out;
		double Nearest = 0.0;
		Out.Vertices = ApplyStroke(Mesh, Target->GetWorldTransform(), Stroke, Out.Before, Out.After, Nearest);
		if (Out.Vertices == NoWeightLayer)
			return Abandon(FString::Printf(TEXT("the brush modifier %s has no weight attribute for the channel %s"), Owner ? *Owner->GetActorLabel() : TEXT("?"), *Stroke.Channel.ToString()));
		if (Out.Vertices == NoVertex)
		{
			// The base meshes' spacing alone would have put a vertex under the brush: the ground there is a modifier's own
			// triangles (a Boolean shape keeps its mesh's, 100+ m across for a large sphere). (No vertex at all: Nearest is Max.)
			const bool bCoarseGround = Nearest < TNumericLimits<double>::Max() && Nearest > Stroke.Radius && Spacing < Stroke.Radius;
			return Abandon(bCoarseGround
				? FString::Printf(TEXT("no terrain vertex is under the brush at X %.0f, Y %.0f: the nearest is %.0f cm away (Radius %.0f cm). The ground there is made of large triangles, such as a Boolean shape's: %s"),
					Stroke.Point.X, Stroke.Point.Y, Nearest, Stroke.Radius, *(Nearest < MaxRadius
						? FString::Printf(TEXT("use a Radius above %.0f cm, or shape that ground with modifiers"), Nearest)
						: FString(TEXT("a stroke cannot reach them (Radius at most 25000 cm): shape that ground with modifiers"))))
				: FString::Printf(TEXT("no terrain vertex is under the brush at X %.0f, Y %.0f (Radius %.0f cm; the terrain has a vertex about every %.0f cm)"),
					Stroke.Point.X, Stroke.Point.Y, Stroke.Radius, Spacing));
		}

		if (Out.Vertices == 0)
		{
			// Already that shape / weight there (an Erase where nothing is painted): nothing to store, and no new channel
			// entry (a Set entry with nothing painted would pin the channel to its current values under the brush).
			Restore();
			Out.Brush = bNew || !Owner ? FString() : Owner->GetActorLabel();
			if (!WaitForBuild(Terrain))
				return FUnrealAIMeshTerrainStroke();
			Out.Ready = true;
			Out.State = TEXT("built");
			Out.Seconds = SecondsSince(Start);
			return Out;
		}
		// The moved vertices become offsets of the active sculpt layer (what the Sculpt tool does at the end of a stroke).
		if (Stroke.Kind != EStroke::Paint && !Layers->UpdateLayersFromMesh())
			return Abandon(TEXT("the brush modifier's active sculpt layer cannot be edited (its weight is 0)"));
		Target->CommitDynamicMesh(Mesh, IDynamicMeshCommitter::FDynamicMeshCommitInfo());

		Out.Brush = Owner ? Owner->GetActorLabel() : FString();
		Out.NewBrush = bNew;
		const double Digits = Stroke.Kind == EStroke::Paint ? 1000.0 : 100.0;
		Out.Before = FMath::RoundToDouble(Out.Before * Digits) / Digits;
		Out.After = FMath::RoundToDouble(Out.After * Digits) / Digits;
		// The stroke is stored: a build that takes longer is reported (Ready false, State), not failed.
		FString StillBuilding;
		Out.Ready = WaitForBuild(Terrain, DefaultBuildTimeoutSeconds, &StillBuilding);
		Out.State = Out.Ready ? FString(TEXT("built")) : StillBuildingState(Terrain, StillBuilding);
		Out.Seconds = SecondsSince(Start);
		return Out;
	}
}

namespace UnrealAIMeshTerrainSculptLocal
{
	constexpr int32 MaxPaintTiles = 64;
	constexpr double SlopeEdgeDegrees = 2.0;
	constexpr double HeightEdge = 50.0; // cm

	/** 1 inside [Lo, Hi], fading smoothly to 0 over Edge past each bound. */
	double RangeWeight(double Value, double Lo, double Hi, double Edge)
	{
		const double Outside = Value < Lo ? Lo - Value : Value > Hi ? Value - Hi : 0.0;
		if (Outside <= 0.0)
			return 1.0;
		if (Outside >= Edge)
			return 0.0;
		return SmoothFall(Outside / Edge);
	}

	FUnrealAIMeshTerrainPaintWhere PaintWhereFailed(const FString& Message)
	{
		return UnrealMcpRql::Failed<FUnrealAIMeshTerrainPaintWhere>(Message);
	}

	/**
	 * PaintWhere: the area split into tiles a brush modifier can hold (at most 600 x 600 m and MaxVertices); in each tile a
	 * brush (the one an earlier PaintWhere left for exactly that tile, when nothing is applied after it there; else a new
	 * one after every modifier) whose weight attribute for the channel moves to Strength at every vertex in the ranges.
	 */
	FUnrealAIMeshTerrainPaintWhere RunPaintWhere(AMeshPartition* Terrain, const FString& ChannelText, double MinSlope, double MaxSlope, double MinHeight, double MaxHeight,
		double Strength, const FString& Label, const TArray<double>& Within)
	{
		const double Start = FPlatformTime::Seconds();
		for (const double Value : { MinSlope, MaxSlope, MinHeight, MaxHeight, Strength })
		{
			if (!FMath::IsFinite(Value))
				return PaintWhereFailed(TEXT("the slopes, heights and Strength must be finite numbers"));
		}
		if (!(MinSlope >= 0.0 && MinSlope <= MaxSlope && MaxSlope <= 90.0))
			return PaintWhereFailed(FString::Printf(TEXT("MinSlope %g / MaxSlope %g: need 0 <= MinSlope <= MaxSlope <= 90 degrees"), MinSlope, MaxSlope));
		if (MinHeight > MaxHeight)
			return PaintWhereFailed(FString::Printf(TEXT("MinHeight %g is above MaxHeight %g (world Z, cm)"), MinHeight, MaxHeight));
		if (!(Strength >= 0.0 && Strength <= 1.0))
			return PaintWhereFailed(FString::Printf(TEXT("Strength %g is outside 0 .. 1 (the weight to paint)"), Strength));
		if (Within.Num() != 0 && Within.Num() != 4)
			return PaintWhereFailed(FString::Printf(TEXT("Within has %d numbers: pass [MinX, MinY, MaxX, MaxY] (world cm) or [] for the whole terrain"), Within.Num()));
		if (Within.Num() == 4 && !(Within[0] < Within[2] && Within[1] < Within[3]))
			return PaintWhereFailed(TEXT("Within must be [MinX, MinY, MaxX, MaxY] with MinX < MaxX and MinY < MaxY (world cm)"));
		const FName Channel(*ChannelText.TrimStartAndEnd());
		if (Channel.IsNone())
			return PaintWhereFailed(TEXT("Channel is empty: name one of the terrain's Channels (meshTerrains | select Label Channels)"));
		if (!CheckTerrain(Terrain))
			return FUnrealAIMeshTerrainPaintWhere();
		const UMeshPartitionDefinition* Definition = Terrain->GetMeshPartitionDefinition();
		if (!Definition || !Definition->GetChannelNames().Contains(Channel))
			return PaintWhereFailed(FString::Printf(TEXT("'%s' is not a paint channel of %s: its definition has %s"), *ChannelText, *Terrain->GetActorNameOrLabel(), *ChannelList(Terrain)));
		if (!WaitForBuild(Terrain))
			return FUnrealAIMeshTerrainPaintWhere();
		const FBox Built = BuiltBounds(Terrain);
		if (!Built.IsValid)
			return PaintWhereFailed(FString::Printf(TEXT("%s has no built ground to paint"), *Terrain->GetActorNameOrLabel()));
		const FBox2D Ground(FVector2D(Built.Min), FVector2D(Built.Max));
		FBox2D Area = Within.Num() == 4 ? FBox2D(FVector2D(Within[0], Within[1]), FVector2D(Within[2], Within[3])) : Ground;
		if (!Area.Intersect(Ground))
		{
			return PaintWhereFailed(FString::Printf(TEXT("Within is off the terrain %s (it spans X %.0f .. %.0f, Y %.0f .. %.0f)"), *Terrain->GetActorNameOrLabel(),
				Ground.Min.X, Ground.Max.X, Ground.Min.Y, Ground.Max.Y));
		}
		Area = Area.Overlap(Ground);

		// Tiles: at most 600 m and MaxVertices of the terrain's base grid each.
		const double Density = VertexDensity(Terrain, Area);
		const double Tile = FMath::Min(2.0 * MaxBrushHalfExtent, Density > 0.0 ? FMath::Sqrt(MaxVertices / 2.0 / Density) : 2.0 * MaxBrushHalfExtent);
		const FVector2D Size = Area.GetSize();
		const int32 TilesX = FMath::Max(1, FMath::CeilToInt(Size.X / Tile)), TilesY = FMath::Max(1, FMath::CeilToInt(Size.Y / Tile));
		if (TilesX * TilesY > MaxPaintTiles)
			return PaintWhereFailed(FString::Printf(TEXT("the area needs %d brush tiles (at most %d): pass a smaller Within"), TilesX * TilesY, MaxPaintTiles));
		const FVector2D Half(Size.X / TilesX / 2.0, Size.Y / TilesY / 2.0);
		// A brush fades out at the edge of its edit box: each one reaches a few vertices past its tile, so the vertices along a
		// tile's edge lie well inside a brush (the overlapping brushes write the same weights there).
		const double Margin = 2.0 * (Density > 0.0 ? FMath::Sqrt(1.0 / Density) : 100.0) + 100.0;

		FScopedTransaction Transaction(FText::Format(NSLOCTEXT("UnrealAIMeshTerrain", "PaintWhere", "Paint Mesh Terrain where ({0})"), FText::FromName(Channel)));
		UWorld* World = Terrain->GetWorld();
		const TArray<UModifierComponent*> Stack = Modifiers(Terrain);
		// New brushes go after every modifier of their priority layer there is (as Run places a stroke's brush): set from the first.
		TOptional<double> Last;
		FStroke Paint;
		Paint.Kind = EStroke::Paint;
		Paint.Channel = Channel;
		FUnrealAIMeshTerrainPaintWhere Out;
		const float Depth = static_cast<float>(Built.Max.Z - Built.Min.Z + 20000.0);
		struct FTile { FVector2D Center; FVector2D Half; int32 Splits = 0; };
		TArray<FTile> Tiles;
		for (int32 TileY = 0; TileY < TilesY; ++TileY)
		{
			for (int32 TileX = 0; TileX < TilesX; ++TileX)
				Tiles.Add({ Area.Min + FVector2D((2 * TileX + 1) * Half.X, (2 * TileY + 1) * Half.Y), Half });
		}
		TArray<FString> Skipped;
		// A prepared tile: its brush (new or reused), what to restore when it stores nothing, and its edit mesh.
		struct FPrepared
		{
			UProjectMeshLayersModifier* Brush = nullptr;
			bool bNew = false;
			AActor* Owner = nullptr;
			AMeshPartition* Previous = nullptr;
			TArray<FSculptLayerModifierWeightAttributeEntry> OldEntries;
			float OldUp = 0.0f, OldDown = 0.0f;
			FBox2D Box;
			UE::MeshPartition::UEditableModifierToolTarget* Target = nullptr;
			FDynamicMesh3 Mesh;
			int32 Layer = INDEX_NONE;
		};
		TArray<FPrepared> Prepared;
		TMap<FIntPoint, double> TargetWeights; // by world X, Y (cm): the weight each vertex moves by
		const auto Key = [](const FVector& Point) { return FIntPoint(FMath::RoundToInt32(Point.X), FMath::RoundToInt32(Point.Y)); };
		// A tile that stores nothing leaves no new brush behind and a reused one as it was.
		const auto DropTile = [World, Terrain](FPrepared& Prep)
		{
			if (Prep.bNew)
			{
				if (Prep.Owner)
					World->EditorDestroyActor(Prep.Owner, /*bShouldModifyLevel*/ true);
				for (const AMeshPartition* Bound : { static_cast<const AMeshPartition*>(Terrain), static_cast<const AMeshPartition*>(Prep.Previous) })
				{
					if (UMeshPartitionEditorComponent* Component = EditorComponent(Bound))
						Component->UpdateModifierList();
				}
				return;
			}
			Prep.Brush->SetChannelEntries(Prep.OldEntries);
			const auto Restore = [&Prep](const TCHAR* Name, float Old)
			{
				const float* Value = PropertyValue<float>(Prep.Brush, Name);
				if (Value && *Value != Old)
					SetProperty(Prep.Brush, Name, Old);
			};
			Restore(TEXT("VerticalExtentUp"), Prep.OldUp);
			Restore(TEXT("VerticalExtentDown"), Prep.OldDown);
		};
		// A vertex belongs to exactly one tile (half-open boxes; the area's own far edges included), so seams are painted once.
		const auto Owns = [&Area](const FBox2D& Box, const FVector2D& Point)
		{
			// (Tile edges are sums of halves: within 1 cm of the area's far edge counts as on it.)
			return Point.X >= Box.Min.X && (Point.X < Box.Max.X || Box.Max.X >= Area.Max.X - 1.0) && Point.Y >= Box.Min.Y && (Point.Y < Box.Max.Y || Box.Max.Y >= Area.Max.Y - 1.0);
		};
		for (int32 TileIndex = 0; TileIndex < Tiles.Num(); ++TileIndex)
		{
			const FTile Piece = Tiles[TileIndex];
			const FVector2D Center = Piece.Center;
			const FBox2D Box(Center - Piece.Half, Center + Piece.Half);
			// An earlier PaintWhere's brush for exactly this tile, with nothing applied after it there (the brushes of the
			// neighbouring tiles overlap it only by their margins, with the same weights: they do not count).
			const FBox2D Inner = Box.ExpandBy(-10.0);
			const FVector2D Extents = Piece.Half + FVector2D(Margin);
			const auto IsTileBrush = [&Extents, &Center](const UModifierComponent* Modifier)
			{
				return Cast<UProjectMeshLayersModifier>(Modifier) && EditExtents(const_cast<UProjectMeshLayersModifier*>(Cast<UProjectMeshLayersModifier>(Modifier))).Equals(Extents, 1.0)
					&& !FVector2D(Modifier->GetComponentLocation()).Equals(Center, 1.0);
			};
			UProjectMeshLayersModifier* Brush = nullptr;
			for (int32 Index = 0; Index < Stack.Num() && !Brush; ++Index)
			{
				UProjectMeshLayersModifier* Candidate = Cast<UProjectMeshLayersModifier>(Stack[Index]);
				if (!Candidate || !CanTakeStroke(Candidate, Paint) || !FVector2D(Candidate->GetComponentLocation()).Equals(Center, 1.0) || !EditExtents(Candidate).Equals(Extents, 1.0))
					continue;
				bool bCovered = false;
				for (int32 Later = Index + 1; Later < Stack.Num() && !bCovered; ++Later)
				{
					const FBox Bounds = Stack[Later]->ComputeCombinedBounds();
					bCovered = !Stack[Later]->GetIsDisabledFlag() && Bounds.IsValid && !IsTileBrush(Stack[Later]) && Inner.Intersect(FBox2D(FVector2D(Bounds.Min), FVector2D(Bounds.Max)));
				}
				Brush = bCovered ? nullptr : Candidate;
			}
			const bool bNew = Brush == nullptr;
			AMeshPartition* Previous = nullptr;
			if (bNew)
			{
				const FVector Location(Center, (Built.Min.Z + Built.Max.Z) / 2.0);
				Brush = Cast<UProjectMeshLayersModifier>(SpawnModifier(Terrain, UProjectMeshLayersModifier::StaticClass(), Location, FRotator::ZeroRotator,
					UniqueLabel(World, Label.IsEmpty() ? Terrain->GetActorLabel() + TEXT("_Paint") : Label), Previous));
				if (!Brush)
				{
					if (Out.Brushes.IsEmpty())
					{
						Transaction.Cancel();
						return FUnrealAIMeshTerrainPaintWhere(); // SpawnModifier failed with the reason; nothing was painted
					}
					Skipped.Add(FString::Printf(TEXT("X %.0f, Y %.0f (no brush could be placed)"), Center.X, Center.Y));
					continue;
				}
				if (!Last)
				{
					Last = 0.0;
					for (const UModifierComponent* Modifier : Stack)
					{
						if (Modifier->GetType() == Brush->GetType())
							Last = FMath::Max(*Last, Modifier->GetPriority() + 1.0);
					}
				}
				Brush->SetPriority(*Last);
				SetProperty(Brush, TEXT("EditExtents"), Extents);
			}
			AActor* Owner = Brush->GetOwner();
			Brush->Modify();
			if (Owner)
				Owner->Modify();
			const FObjectProperty* MeshProperty = CastField<FObjectProperty>(Brush->GetClass()->FindPropertyByName(TEXT("MeshObject")));
			if (UObject* MeshObject = MeshProperty ? MeshProperty->GetObjectPropertyValue_InContainer(Brush) : nullptr)
				MeshObject->Modify();
			const auto FloatProperty = [Brush](const TCHAR* Name) { const float* Value = PropertyValue<float>(Brush, Name); return Value ? *Value : 0.0f; };
			const float OldUp = FloatProperty(TEXT("VerticalExtentUp")), OldDown = FloatProperty(TEXT("VerticalExtentDown"));
			for (const TCHAR* Name : { TEXT("VerticalExtentUp"), TEXT("VerticalExtentDown") })
			{
				if (FloatProperty(Name) < Depth)
					SetProperty(Brush, Name, Depth);
			}
			const TArray<FSculptLayerModifierWeightAttributeEntry> OldEntries = Brush->GetChannelEntries();
			if (!ChannelEntry(OldEntries, Channel))
			{
				TArray<FSculptLayerModifierWeightAttributeEntry> Entries = OldEntries;
				Entries.AddDefaulted_GetRef().ChannelName = Channel;
				Brush->SetChannelEntries(Entries);
			}
			FPrepared& Prep = Prepared.AddDefaulted_GetRef();
			Prep.Brush = Brush;
			Prep.bNew = bNew;
			Prep.Owner = Owner;
			Prep.Previous = Previous;
			Prep.OldEntries = OldEntries;
			Prep.OldUp = OldUp;
			Prep.OldDown = OldDown;
			Prep.Box = Box;
			Prep.Target = NewObject<UE::MeshPartition::UEditableModifierToolTarget>();
			Prep.Target->Initialize(Brush);
			Prep.Mesh = Prep.Target->GetDynamicMesh();
			FDynamicMesh3& Mesh = Prep.Mesh;
			if (Mesh.VertexCount() > MaxVertices * 1.5)
			{
				// Denser ground than the base grid (a remeshed shape): split the tile in four (twice at most).
				DropTile(Prep);
				if (bNew && Piece.Splits < 2)
				{
					const FVector2D Quarter = Piece.Half / 2.0;
					for (const FVector2D Sign : { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(-1, 1), FVector2D(1, 1) })
						Tiles.Add({ Center + Sign * Quarter, Quarter, Piece.Splits + 1 });
				}
				else
				{
					Skipped.Add(FString::Printf(TEXT("X %.0f, Y %.0f (%d vertices in its %.0f m tile)"), Center.X, Center.Y, Mesh.VertexCount(), Piece.Half.X / 50.0));
				}
				Prepared.Pop();
				continue;
			}
			UE::Geometry::FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
			for (int32 Index = 0; Attributes && Index < Attributes->NumWeightLayers(); ++Index)
			{
				if (Attributes->GetWeightLayer(Index)->GetName() == Channel)
					Prep.Layer = Index;
			}
			if (Prep.Layer == INDEX_NONE)
			{
				DropTile(Prep);
				Skipped.Add(FString::Printf(TEXT("X %.0f, Y %.0f (its brush %s has no weight attribute for %s)"), Center.X, Center.Y, Owner ? *Owner->GetActorLabel() : TEXT("?"), *Channel.ToString()));
				Prepared.Pop();
				continue;
			}
			// The weights of the vertices this tile owns, from its full neighbourhood (an edge vertex of a tile's mesh has
			// triangles on one side only; its owner sees both).
			const FTransform Transform = Prep.Target->GetWorldTransform();
			for (const int32 Vertex : Mesh.VertexIndicesItr())
			{
				const FVector World3 = Transform.TransformPosition(Mesh.GetVertex(Vertex));
				if (!Area.IsInsideOrOn(FVector2D(World3)) || !Owns(Box, FVector2D(World3)))
					continue;
				const FVector Normal = Transform.TransformVectorNoScale(UE::Geometry::FMeshNormals::ComputeVertexNormal(Mesh, Vertex));
				const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FMath::Abs(Normal.Z), 0.0, 1.0)));
				const double Weight = RangeWeight(Slope, MinSlope, MaxSlope, SlopeEdgeDegrees) * RangeWeight(World3.Z, MinHeight, MaxHeight, HeightEdge);
				TargetWeights.Add(Key(World3), Weight);
				Out.Matched += Weight >= 1.0 ? 1 : 0;
			}
		}
		// Then every tile writes all of its mesh's vertices, those past its edge too: a brush Sets the channel at every vertex
		// it holds, so overlapping brushes must agree (a neighbour's vertex left at its old value would undo the paint there).
		TSet<FIntPoint> Painted;
		for (FPrepared& Prep : Prepared)
		{
			FDynamicMesh3& Mesh = Prep.Mesh;
			UE::Geometry::FDynamicMeshWeightAttribute* Weights = Mesh.Attributes()->GetWeightLayer(Prep.Layer);
			const FTransform Transform = Prep.Target->GetWorldTransform();
			int32 Changed = 0;
			for (const int32 Vertex : Mesh.VertexIndicesItr())
			{
				const FIntPoint Where = Key(Transform.TransformPosition(Mesh.GetVertex(Vertex)));
				const double* Weight = TargetWeights.Find(Where);
				if (!Weight || *Weight <= 0.0)
					continue;
				float Old = 0.0f;
				Weights->GetValue(Vertex, &Old);
				const float New = static_cast<float>(FMath::Lerp(static_cast<double>(Old), Strength, *Weight));
				if (New != Old)
				{
					Weights->SetValue(Vertex, &New);
					++Changed;
					Painted.Add(Where);
				}
			}
			if (Changed == 0)
			{
				DropTile(Prep); // nothing to store: no new brush and no new channel entry (it would pin the channel there)
				continue;
			}
			Prep.Target->CommitDynamicMesh(Mesh, IDynamicMeshCommitter::FDynamicMeshCommitInfo());
			Out.NewBrushes += Prep.bNew ? 1 : 0;
			Out.Brushes.Add(Prep.Owner ? Prep.Owner->GetActorLabel() : FString());
		}
		Out.Vertices = Painted.Num();
		if (!Skipped.IsEmpty())
			Out.Note = FString::Printf(TEXT("not painted: %s; pass a smaller Within there"), *FString::Join(Skipped, TEXT("; ")));
		FString StillBuilding;
		Out.Ready = WaitForBuild(Terrain, DefaultBuildTimeoutSeconds, &StillBuilding);
		Out.State = Out.Ready ? FString(TEXT("built")) : StillBuildingState(Terrain, StillBuilding);
		Out.Seconds = SecondsSince(Start);
		return Out;
	}
}

FUnrealAIMeshTerrainPaintWhere UUnrealAIMeshTerrainSculptApi::PaintWhere(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, double MinSlope, double MaxSlope,
	double MinHeight, double MaxHeight, double Strength, const FString& Label, const TArray<double>& Within)
{
	return UnrealAIMeshTerrainSculptLocal::RunPaintWhere(Terrain, Channel, MinSlope, MaxSlope, MinHeight, MaxHeight, Strength, Label, Within);
}

FUnrealAIMeshTerrainStroke UUnrealAIMeshTerrainSculptApi::Sculpt(UE::MeshPartition::AMeshPartition* Terrain, const FString& Mode, FVector Point, double Radius, double Height, double Strength, double Falloff)
{
	using namespace UnrealAIMeshTerrainSculptLocal;
	FStroke Stroke;
	static const TMap<FString, EStroke> Modes = { { TEXT("raise"), EStroke::Raise }, { TEXT("lower"), EStroke::Lower }, { TEXT("flatten"), EStroke::Flatten }, { TEXT("smooth"), EStroke::Smooth } };
	const EStroke* Kind = Modes.Find(Mode.TrimStartAndEnd().ToLower());
	if (!Kind)
		return Failed(FString::Printf(TEXT("Mode '%s' is not one of Raise, Lower, Flatten, Smooth"), *Mode));
	Stroke.Kind = *Kind;
	if ((Stroke.Kind == EStroke::Raise || Stroke.Kind == EStroke::Lower) && !(Height > 0.0 && Height <= MaxRaise))
		return Failed(FString::Printf(TEXT("Height %g cm is outside 0 (exclusive) .. %g: how far %s moves the ground at the centre"), Height, MaxRaise, *Mode));
	if (Stroke.Kind == EStroke::Flatten && !(FMath::Abs(Height) <= 10000000.0))
		return Failed(FString::Printf(TEXT("Height %g: Flatten needs the world Z (cm) to flatten to"), Height));
	Stroke.Point = FVector2D(Point.X, Point.Y);
	Stroke.Radius = Radius;
	Stroke.Amount = Stroke.Kind == EStroke::Smooth ? 0.0 : Height;
	Stroke.Strength = Strength;
	Stroke.Falloff = Falloff;
	return Run(Terrain, Stroke, FText::Format(NSLOCTEXT("UnrealAIMeshTerrain", "Sculpt", "Sculpt Mesh Terrain ({0})"), FText::FromString(Mode)));
}

FUnrealAIMeshTerrainStroke UUnrealAIMeshTerrainSculptApi::Paint(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, FVector Point, double Radius, double Weight, double Falloff)
{
	using namespace UnrealAIMeshTerrainSculptLocal;
	if (!(Weight >= 0.0 && Weight <= 1.0))
		return Failed(FString::Printf(TEXT("Weight %g is outside 0 .. 1"), Weight));
	if (Channel.TrimStartAndEnd().IsEmpty())
		return Failed(TEXT("Channel is empty: name one of the terrain's Channels (meshTerrains | select Label Channels)"));
	FStroke Stroke;
	Stroke.Kind = EStroke::Paint;
	Stroke.Channel = FName(*Channel.TrimStartAndEnd());
	Stroke.Point = FVector2D(Point.X, Point.Y);
	Stroke.Radius = Radius;
	Stroke.Amount = Weight;
	Stroke.Falloff = Falloff;
	return Run(Terrain, Stroke, FText::Format(NSLOCTEXT("UnrealAIMeshTerrain", "Paint", "Paint Mesh Terrain ({0})"), FText::FromName(Stroke.Channel)));
}

FUnrealAIMeshTerrainStroke UUnrealAIMeshTerrainSculptApi::Erase(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, FVector Point, double Radius, double Falloff)
{
	return Paint(Terrain, Channel, Point, Radius, 0.0, Falloff);
}
