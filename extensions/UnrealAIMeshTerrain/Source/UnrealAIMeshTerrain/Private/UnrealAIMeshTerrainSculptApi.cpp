// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainSculptApi.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "IUnrealMcpRqlModule.h"

#include "ActorFactories/ActorFactory.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"

#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionModifierActor.h"
#include "MeshPartitionModifierToolTarget.h"
#include "Modifiers/MeshPartitionMeshProvider.h"
#include "Modifiers/MeshPartitionProjectSculptLayersModifier.h"

/*
 * How a stroke is stored (verified in a UE 5.8 editor, see RESEARCH.md "sculpt"): Mesh Terrain Mode's Sculpt and Paint tools
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

	/** 1 inside the brush's core, fading smoothly to 0 at its Radius over the outer Falloff part. */
	double BrushWeight(double Distance, double Radius, double Falloff)
	{
		if (Distance >= Radius)
			return 0.0;
		const double Inner = Radius * (1.0 - Falloff);
		if (Distance <= Inner)
			return 1.0;
		const double T = (Distance - Inner) / (Radius - Inner);
		return 1.0 - T * T * (3.0 - 2.0 * T);
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
	 * an active sculpt layer with a weight (else the layer cannot be edited) and, to paint, a plain Set entry for the channel
	 * (or none yet).
	 */
	bool CanTakeStroke(UProjectMeshLayersModifier* Brush, const FStroke& Stroke)
	{
		const FTransform Transform = Brush->GetComponentTransform();
		const uint8* Method = PropertyValue<uint8>(Brush, TEXT("ProjectMethod"));
		if (Brush->GetIsDisabledFlag() || !Transform.GetRotation().Equals(FQuat::Identity, 1e-4) || !Transform.GetScale3D().Equals(FVector::OneVector, 1e-4)
			|| !Method || *Method != static_cast<uint8>(UE::MeshPartition::ESculptLayerProjectMethod::FixedDirection) || BoolProperty(Brush, TEXT("bSculptAbsolutePositions")))
			return false;
		const int32 Layer = Brush->GetActiveLayer() - 1; // LayerWeights leaves out the base layer
		if (Brush->LayerWeights.IsValidIndex(Layer) && FMath::IsNearlyZero(Brush->LayerWeights[Layer]))
			return false;
		if (Stroke.Kind != EStroke::Paint)
			return true;
		const FSculptLayerModifierWeightAttributeEntry* Entry = ChannelEntry(Brush->GetChannelEntries(), Stroke.Channel);
		return !Entry || (Entry->Method == UE::MeshPartition::ESculptLayerSetWeightChannelMethod::Set && !Entry->bInternalChannel
			&& FMath::IsNearlyEqual(Entry->BlendWeight, 1.0f) && !Entry->bScaleSculptLayers);
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

	/** A unique label for a new brush modifier of @p Terrain: "<terrain>_Brush", "<terrain>_Brush2", ... */
	FString BrushLabel(const AMeshPartition* Terrain)
	{
		TSet<FString> Taken;
		for (UModifierComponent* Modifier : Modifiers(Terrain))
		{
			if (Modifier->GetOwner())
				Taken.Add(Modifier->GetOwner()->GetActorLabel());
		}
		const FString Base = Terrain->GetActorLabel() + TEXT("_Brush");
		FString Label = Base;
		for (int32 Index = 2; Taken.Contains(Label); ++Index)
			Label = FString::Printf(TEXT("%s%d"), *Base, Index);
		return Label;
	}

	/**
	 * Move the vertices of @p Mesh (in the brush's local space, @p Transform to world) that the stroke covers (or their
	 * channel weights); returns how many changed, INDEX_NONE when no vertex is under the brush.
	 */
	int32 ApplyStroke(FDynamicMesh3& Mesh, const FTransform& Transform, const FStroke& Stroke, double& OutBefore, double& OutAfter)
	{
		const FVector Local = Transform.InverseTransformPosition(FVector(Stroke.Point, 0.0));
		const FVector2D Center(Local.X, Local.Y);
		TMap<int32, double> Weights;
		int32 Nearest = INDEX_NONE;
		double NearestDistance = TNumericLimits<double>::Max();
		for (const int32 Vertex : Mesh.VertexIndicesItr())
		{
			const double Distance = FVector2D::Distance(FVector2D(Mesh.GetVertex(Vertex)), Center);
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
			return INDEX_NONE;

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
				return 0;
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
		if (!CheckAvailable() || !CheckTerrain(Terrain))
			return FUnrealAIMeshTerrainStroke();
		if (Stroke.Kind == EStroke::Paint)
		{
			const UMeshPartitionDefinition* Definition = Terrain->GetMeshPartitionDefinition();
			const TArray<FName> Channels = Definition ? Definition->GetChannelNames() : TArray<FName>();
			if (!Channels.Contains(Stroke.Channel))
			{
				TArray<FString> Names;
				for (const FName& Name : Channels)
					Names.Add(Name.ToString());
				return Failed(FString::Printf(TEXT("'%s' is not a paint channel of %s: its definition has %s"), *Stroke.Channel.ToString(), *Terrain->GetActorNameOrLabel(),
					Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", "))));
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
		const double Density = VertexDensity(Terrain, Area);
		const double Spacing = Density > 0.0 ? FMath::Sqrt(1.0 / Density) : 100.0;
		const double Margin = 2.0 * Spacing + 100.0;
		const auto Estimate = [Density](const FVector2D& HalfExtents) { return Density * 4.0 * HalfExtents.X * HalfExtents.Y; };

		// The brush modifier the stroke goes into: one that covers the area already (the last in build order), else one that
		// overlaps it or is near (within MinBrushHalfExtent) and may grow to cover it (the smallest growth), else a new one.
		UProjectMeshLayersModifier* Brush = nullptr;
		FVector2D Extents = FVector2D::ZeroVector;
		double BestGrowth = TNumericLimits<double>::Max();
		for (UModifierComponent* Modifier : Modifiers(Terrain))
		{
			UProjectMeshLayersModifier* Candidate = Cast<UProjectMeshLayersModifier>(Modifier);
			if (!Candidate || !CanTakeStroke(Candidate, Stroke))
				continue;
			const FVector2D Have = EditExtents(Candidate);
			const FVector2D Offset = (Stroke.Point - FVector2D(Candidate->GetComponentLocation())).GetAbs();
			if (Offset.X + Stroke.Radius <= Have.X && Offset.Y + Stroke.Radius <= Have.Y)
			{
				Brush = Candidate;
				Extents = Have;
				BestGrowth = 0.0;
				continue;
			}
			if (BestGrowth == 0.0 || Offset.X - Stroke.Radius - Have.X > MinBrushHalfExtent || Offset.Y - Stroke.Radius - Have.Y > MinBrushHalfExtent)
				continue; // a covering brush was found, or this one is far from the stroke
			const FVector2D Need(FMath::Max(Have.X, Offset.X + Stroke.Radius + Margin), FMath::Max(Have.Y, Offset.Y + Stroke.Radius + Margin));
			const double Growth = Need.X * Need.Y - Have.X * Have.Y;
			if (Need.X <= MaxBrushHalfExtent && Need.Y <= MaxBrushHalfExtent && Estimate(Need) <= MaxVertices && Growth < BestGrowth)
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
			if (Estimate(FVector2D(MinBrushHalfExtent)) <= MaxVertices / 4.0)
				Half = FMath::Max(Half, MinBrushHalfExtent);
			Extents = FVector2D(Half);
		}
		if (Estimate(Extents) > MaxVertices)
		{
			return Failed(FString::Printf(TEXT("the stroke would edit about %.0f terrain vertices (at most %.0f, the terrain has a vertex every %.0f cm): use a smaller Radius"),
				Estimate(Extents), MaxVertices, Spacing));
		}

		FScopedTransaction Transaction(Description);
		UWorld* World = Terrain->GetWorld();
		if (bNew)
		{
			// Placed the way Mesh Terrain Mode's modifier palette places one, on the ground under the point.
			const FSurfaceSample Ground = SampleSurface(Terrain, Stroke.Point.X, Stroke.Point.Y, /*bChannels*/ false);
			const FVector Location(Stroke.Point, Ground.bInside ? Ground.Height : Terrain->GetActorLocation().Z);
			UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryByClass(UE::MeshPartition::UModifierActorFactory::StaticClass()) : nullptr;
			AActor* Actor = Factory ? Factory->CreateActor(UProjectMeshLayersModifier::StaticClass(), World->GetCurrentLevel(), FTransform(Location)) : nullptr;
			Brush = Actor ? Actor->FindComponentByClass<UProjectMeshLayersModifier>() : nullptr;
			if (!Brush)
			{
				Transaction.Cancel();
				return Failed(TEXT("could not place a brush modifier (UModifierActorFactory)"));
			}
			Actor->SetActorLabel(BrushLabel(Terrain));
			// The factory binds the nearest terrain by its own rule: bind this one. Applied after every modifier there is.
			Brush->SetAffectedMeshPartition(Terrain);
			double Last = 0.0;
			for (const UModifierComponent* Modifier : Modifiers(Terrain))
			{
				if (Modifier != Brush && Modifier->GetType() == Brush->GetType())
					Last = FMath::Max(Last, Modifier->GetPriority() + 1.0);
			}
			Brush->SetPriority(Last);
		}
		AActor* Owner = Brush->GetOwner();
		Brush->Modify();
		if (Owner)
			Owner->Modify();
		const FObjectProperty* MeshProperty = CastField<FObjectProperty>(Brush->GetClass()->FindPropertyByName(TEXT("MeshObject")));
		if (UObject* MeshObject = MeshProperty ? MeshProperty->GetObjectPropertyValue_InContainer(Brush) : nullptr)
			MeshObject->Modify(); // the stored sculpt mesh: undo restores it
		if (EditExtents(Brush) != Extents)
			SetProperty(Brush, TEXT("EditExtents"), Extents);
		// Deep enough to reach all of the terrain's ground under the brush (and what the stroke raises).
		const double Depth = FMath::Max(10000.0, Built.Max.Z - Built.Min.Z + 10000.0 + (Stroke.Kind == EStroke::Raise || Stroke.Kind == EStroke::Lower ? Stroke.Amount : 0.0));
		for (const TCHAR* Name : { TEXT("VerticalExtentUp"), TEXT("VerticalExtentDown") })
		{
			const float* Value = PropertyValue<float>(Brush, Name);
			if (Value && *Value < Depth)
				SetProperty(Brush, Name, static_cast<float>(Depth));
		}
		const TArray<FSculptLayerModifierWeightAttributeEntry> OldEntries = Brush->GetChannelEntries();
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
		const auto Abandon = [&](const FString& Message)
		{
			if (bNew && Owner)
				World->EditorDestroyActor(Owner, /*bShouldModifyLevel*/ true);
			else
				Brush->SetChannelEntries(OldEntries);
			return Failed(Message);
		};
		if (Mesh.VertexCount() > MaxVertices * 1.5)
			return Abandon(FString::Printf(TEXT("the brush area holds %d terrain vertices (at most %.0f): use a smaller Radius"), Mesh.VertexCount(), MaxVertices));
		UE::Geometry::FDynamicMeshSculptLayers* Layers = Mesh.HasAttributes() ? Mesh.Attributes()->GetSculptLayers() : nullptr;
		if (Stroke.Kind != EStroke::Paint && (!Layers || !Layers->IsEnabled()))
			return Abandon(FString::Printf(TEXT("the brush modifier %s has no sculpt layer to edit"), Owner ? *Owner->GetActorLabel() : TEXT("?")));

		FUnrealAIMeshTerrainStroke Out;
		Out.Vertices = ApplyStroke(Mesh, Target->GetWorldTransform(), Stroke, Out.Before, Out.After);
		if (Out.Vertices == INDEX_NONE)
		{
			return Abandon(FString::Printf(TEXT("no terrain vertex is under the brush at X %.0f, Y %.0f (Radius %.0f cm; the terrain has a vertex about every %.0f cm)"),
				Stroke.Point.X, Stroke.Point.Y, Stroke.Radius, Spacing));
		}
		if (Out.Vertices == 0)
		{
			// Already that shape / weight there (an Erase where nothing is painted): nothing to store, and no new channel
			// entry (a Set entry with nothing painted would pin the channel to its current values under the brush).
			if (bNew && Owner)
			{
				World->EditorDestroyActor(Owner, /*bShouldModifyLevel*/ true);
			}
			else
			{
				Brush->SetChannelEntries(OldEntries);
				Out.Brush = Owner ? Owner->GetActorLabel() : FString();
			}
			Out.Ready = true;
			Out.Seconds = FMath::RoundToDouble((FPlatformTime::Seconds() - Start) * 100.0) / 100.0;
			return Out;
		}
		// The moved vertices become offsets of the active sculpt layer (what the Sculpt tool does at the end of a stroke).
		if (Stroke.Kind != EStroke::Paint && !Layers->UpdateLayersFromMesh())
			return Abandon(TEXT("the brush modifier's active sculpt layer cannot be edited (its weight is 0)"));
		Target->CommitDynamicMesh(Mesh, IDynamicMeshCommitter::FDynamicMeshCommitInfo());

		Out.Brush = Owner ? Owner->GetActorLabel() : FString();
		Out.NewBrush = bNew;
		const double Round = Stroke.Kind == EStroke::Paint ? 1000.0 : 100.0;
		Out.Before = FMath::RoundToDouble(Out.Before * Round) / Round;
		Out.After = FMath::RoundToDouble(Out.After * Round) / Round;
		Out.Ready = WaitForBuild(Terrain);
		Out.Seconds = FMath::RoundToDouble((FPlatformTime::Seconds() - Start) * 100.0) / 100.0;
		return Out.Ready ? Out : FUnrealAIMeshTerrainStroke();
	}
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
