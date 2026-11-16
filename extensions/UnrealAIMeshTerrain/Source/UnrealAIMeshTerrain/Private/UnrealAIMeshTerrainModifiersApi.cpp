// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainModifiersApi.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "IUnrealMcpRqlModule.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "UDynamicMesh.h"

#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionModifierActor.h"
#include "MeshPartitionModifierComponent.h"
#include "Modifiers/MeshPartitionBooleanModifier.h"
#include "Modifiers/MeshPartitionLatticeModifier.h"
#include "Modifiers/MeshPartitionMeshProjectModifier.h"
// Their spline delegate helper uses USplineComponent's experimental change notifications (C4996 in an engine header).
PRAGMA_DISABLE_DEPRECATION_WARNINGS
#include "Modifiers/MeshPartitionSplineModifier.h"
#include "Modifiers/MeshPartitionSplineRemeshModifier.h"
PRAGMA_ENABLE_DEPRECATION_WARNINGS

#define LOCTEXT_NAMESPACE "UnrealAIMeshTerrainModifiers"

namespace UnrealAIMeshTerrainModifiersLocal
{
	using namespace UnrealAIMeshTerrain;
	using FModifierOpResult = FUnrealAIMeshTerrainModifierResult;
	using UBooleanModifier = UE::MeshPartition::UBooleanModifier;
	using UMeshBasedModifierBase = UE::MeshPartition::UMeshBasedModifierBase;
	using UMeshProjectModifier = UE::MeshPartition::UMeshProjectModifier;
	using ULatticeModifier = UE::MeshPartition::ULatticeModifier;
	using USplineModifier = UE::MeshPartition::USplineModifier;
	using USplineRemeshModifier = UE::MeshPartition::USplineRemeshModifier;

	/** Sizes, radii and widths beyond 100 km are typos, not terrain features. */
	constexpr double MaxLength = 10000000.0;
	/** A remesh far finer than the ground it covers makes millions of triangles: at most this many. */
	constexpr double MaxRemeshTriangles = 4000000.0;
	/** The component tag a shape's mesh component carries ("MeshTerrainShape:Box"), so Edit can resize it. */
	const TCHAR* ShapeTagPrefix = TEXT("MeshTerrainShape:");

	FModifierOpResult Fail(const FString& Message)
	{
		return UnrealMcpRql::Failed<FModifierOpResult>(Message);
	}

	FString Text(const FVector& Value)
	{
		return FString::Printf(TEXT("[%g, %g, %g]"), Value.X, Value.Y, Value.Z);
	}

	bool IsLength(double Value, bool bAllowZero = false)
	{
		return FMath::IsFinite(Value) && (bAllowZero ? Value >= 0.0 : Value > 0.0) && Value <= MaxLength;
	}

	bool IsPoint(const FVector& Value)
	{
		return !Value.ContainsNaN() && Value.GetAbsMax() <= 100.0 * MaxLength;
	}

	/** The modifier classes agents can add by Kind (the Modifiers palette's, Mesh Terrain Mode's EModifierClassType). */
	const TArray<FString>& AddableKinds()
	{
		static const TArray<FString> Kinds = { TEXT("Noise"), TEXT("Patch"), TEXT("InstancedPatch"), TEXT("WeightUtility"), TEXT("Spline"),
			TEXT("SplineRemesh"), TEXT("Remesh"), TEXT("Boolean"), TEXT("MeshProject"), TEXT("TexturePatch"), TEXT("Lattice"), TEXT("ProjectMeshLayers") };
		return Kinds;
	}

	/** The modifier class of @p Kind ("Noise", "NoiseModifier", "Project" for MeshProject, "Weight" for WeightUtility); null when not addable. */
	UClass* ModifierClassOf(FString Kind)
	{
		Kind.TrimStartAndEndInline();
		Kind.RemoveFromEnd(TEXT("Modifier"), ESearchCase::IgnoreCase);
		if (Kind.Equals(TEXT("Project"), ESearchCase::IgnoreCase))
			Kind = TEXT("MeshProject");
		else if (Kind.Equals(TEXT("Weight"), ESearchCase::IgnoreCase))
			Kind = TEXT("WeightUtility");
		else if (Kind.Equals(TEXT("MeshLayer"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("Brush"), ESearchCase::IgnoreCase))
			Kind = TEXT("ProjectMeshLayers");
		const FString* Found = AddableKinds().FindByPredicate([&Kind](const FString& Candidate) { return Candidate.Equals(Kind, ESearchCase::IgnoreCase); });
		if (!Found)
			return nullptr;
		UClass* Class = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/MeshPartitionEditor.%sModifier"), **Found));
		return Class && Class->IsChildOf(UModifierComponent::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract) ? Class : nullptr;
	}

	/** Fail unless @p Channel is one of @p Terrain's paint channels (empty passes when @p bAllowEmpty). */
	bool CheckChannel(const AMeshPartition* Terrain, const FString& Channel, bool bAllowEmpty)
	{
		if (Channel.IsEmpty() && bAllowEmpty)
			return true;
		const UMeshPartitionDefinition* Definition = Terrain ? Terrain->GetMeshPartitionDefinition() : nullptr;
		if (Definition && Definition->GetChannelNames().ContainsByPredicate([&Channel](const FName& Name) { return Name.ToString().Equals(Channel, ESearchCase::IgnoreCase); }))
			return true;
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("'%s' is not a paint channel of %s; its channels: %s"), *Channel, *Terrain->GetActorNameOrLabel(), *ChannelList(Terrain)));
	}

	/** The definition's spelling of @p Channel (CheckChannel passed). */
	FString ChannelName(const AMeshPartition* Terrain, const FString& Channel)
	{
		for (const FName& Name : Terrain->GetMeshPartitionDefinition()->GetChannelNames())
		{
			if (Name.ToString().Equals(Channel, ESearchCase::IgnoreCase))
				return Name.ToString();
		}
		return Channel;
	}

	/**
	 * Write @p Object's property @p Name from UE's text form (as the Details panel's paste does) through the editor's
	 * property-change path (PreEditChange / PostEditChangeChainProperty), so the modifier queues its rebuild exactly as an
	 * edit in the Details panel. Fails (and returns false) when the property is missing or the text does not import.
	 */
	bool SetProperty(UObject* Object, const FName Name, const FString& Value)
	{
		FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(Name) : nullptr;
		if (!Property)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no property %s"), Object ? *Object->GetClass()->GetName() : TEXT("null"), *Name.ToString()));
		FEditPropertyChain Chain;
		Chain.AddHead(Property);
		Chain.SetActivePropertyNode(Property);
		Object->SetFlags(RF_Transactional);
		Object->Modify();
		Object->PreEditChange(Chain);
		const TCHAR* End = Property->ImportText_InContainer(*Value, Object, Object, PPF_None);
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		FPropertyChangedChainEvent ChainEvent(Chain, Event);
		Object->PostEditChangeChainProperty(ChainEvent);
		if (!End)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("could not write %s = %s on %s"), *Name.ToString(), *Value, *Object->GetClass()->GetName()));
		return true;
	}

	/** Write a double / float / int property from a number. */
	bool SetNumber(UObject* Object, const FName Name, double Value)
	{
		return SetProperty(Object, Name, FString::Printf(TEXT("%.17g"), Value));
	}

	/** The XY box of @p Terrain's built ground (WaitForBuild first), invalid when nothing is built. */
	FBox2D TerrainArea(const AMeshPartition* Terrain)
	{
		const FBox Built = BuiltBounds(Terrain);
		return Built.IsValid ? FBox2D(FVector2D(Built.Min), FVector2D(Built.Max)) : FBox2D(ForceInit);
	}

	/**
	 * Wait for @p Terrain's pending build, then fail unless the XY box @p Footprint overlaps its ground: a modifier placed off
	 * the terrain changes nothing, which is always a mistake (a wrong terrain or a wrong unit).
	 */
	bool CheckFootprint(AMeshPartition* Terrain, const FBox2D& Footprint)
	{
		if (!WaitForBuild(Terrain))
			return false;
		const FBox2D Area = TerrainArea(Terrain);
		if (!Area.bIsValid || Area.Intersect(Footprint))
			return true;
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("the modifier would cover X %.0f .. %.0f, Y %.0f .. %.0f, outside %s (X %.0f .. %.0f, Y %.0f .. %.0f cm): it would not change the terrain"),
			Footprint.Min.X, Footprint.Max.X, Footprint.Min.Y, Footprint.Max.Y, *Terrain->GetActorNameOrLabel(), Area.Min.X, Area.Max.X, Area.Min.Y, Area.Max.Y));
	}

	FBox2D Around(const FVector& Center, double HalfX, double HalfY)
	{
		return FBox2D(FVector2D(Center.X - HalfX, Center.Y - HalfY), FVector2D(Center.X + HalfX, Center.Y + HalfY));
	}

	/**
	 * The guards every operation on an existing modifier runs: available, a live modifier of the editor world, not a base mesh
	 * (with @p bOrphanBase, Remove's: a base mesh left behind by a deleted terrain, which shapes nothing, may go).
	 */
	bool CheckModifier(const UModifierComponent* Modifier, bool bNeedsTerrain = true, bool bOrphanBase = false)
	{
		if (!CheckAvailable())
			return false;
		if (!Modifier)
			return UnrealMcpRql::Failed<bool>(TEXT("Modifier is null: pass one from meshTerrainModifiers (meshTerrainModifiers | select $ref Modifier)"));
		if (!IsValid(Modifier) || Modifier->GetWorld() != UnrealMcpRql::EditorWorld())
			return UnrealMcpRql::Failed<bool>(TEXT("the modifier is not in the open level (deleted, or another world)"));
		if (Modifier->IsBase() && !(bOrphanBase && !IsValid(TerrainOf(Modifier))))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is a base mesh (a section of the terrain itself), not a modifier: only the terrain's create / import operations make or replace those"),
				*Modifier->GetOwner()->GetActorNameOrLabel()));
		AMeshPartition* Terrain = TerrainOf(Modifier);
		if (!Terrain && bNeedsTerrain)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not bound to a Mesh Terrain: call meshTerrainModifier | call Bind(modifier, terrain) first"),
				*Modifier->GetOwner()->GetActorNameOrLabel()));
		if (Terrain)
			LoadTerrainActors(Terrain);
		return true;
	}

	/** Whether @p Modifier is a @p Kind modifier or a subclass of one (a Blueprint of it, InstancedPatch for Patch). */
	bool IsKind(const UModifierComponent* Modifier, const TCHAR* Kind);

	/** The modifier's summary plus where @p Terrain's build stands. */
	FModifierOpResult Describe(UModifierComponent* Modifier, const AMeshPartition* Terrain, double Start)
	{
		FModifierOpResult Out;
		Out.Modifier = Modifier;
		if (Modifier)
		{
			Out.Actor = Modifier->GetOwner() ? Modifier->GetOwner()->GetActorNameOrLabel() : FString();
			Out.Location = Modifier->GetOwner() ? Modifier->GetOwner()->GetActorLocation() : Modifier->GetComponentLocation();
			Out.Kind = ModifierKind(Modifier);
			Out.PriorityLayer = Modifier->GetType().ToString();
			Out.Priority = Modifier->GetPriority();
			Out.Disabled = Modifier->IsDisabled();
		}
		Out.Terrain = Terrain ? Terrain->GetActorNameOrLabel() : FString();
		Out.Seconds = SecondsSince(Start);
		if (Terrain)
		{
			const FBuildState State = BuildState(Terrain);
			Out.Ready = State.IsReady();
			Out.State = State.Describe();
		}
		else
		{
			Out.State = TEXT("not bound to a Mesh Terrain");
		}
		return Out;
	}

	/**
	 * Let the edited terrains pick the change up (their modifier lists) and wait until each is rebuilt, then describe
	 * @p Modifier. @p Terrains may hold nulls and duplicates; the first non-null one is the result's terrain.
	 */
	FModifierOpResult Finish(UModifierComponent* Modifier, std::initializer_list<AMeshPartition*> Terrains)
	{
		const double Start = FPlatformTime::Seconds();
		AMeshPartition* Main = nullptr;
		TArray<AMeshPartition*> Done;
		for (AMeshPartition* Terrain : Terrains)
		{
			if (!IsValid(Terrain) || Done.Contains(Terrain))
				continue;
			Main = Main ? Main : Terrain;
			Done.Add(Terrain);
			if (!WaitForBuild(Terrain)) // its first round updates the terrain's modifier list
				return FModifierOpResult();
		}
		return Describe(Modifier, Main, Start);
	}

	/**
	 * One modifier an Add operation places: if a step after the spawn fails, the half-made modifier is taken out again
	 * (in the same undo step) when this goes out of scope; Keep() hands it to the terrain and waits for the build.
	 */
	struct FPlacement
	{
		AMeshPartition* Terrain = nullptr;
		AMeshPartition* Previous = nullptr;
		UModifierComponent* Modifier = nullptr;
		bool bKept = false;

		explicit FPlacement(AMeshPartition* InTerrain) : Terrain(InTerrain) {}
		~FPlacement()
		{
			AActor* Owner = !bKept && IsValid(Modifier) ? Modifier->GetOwner() : nullptr;
			if (!Owner)
				return;
			Owner->GetWorld()->EditorDestroyActor(Owner, /*bShouldModifyLevel*/ true);
			for (const AMeshPartition* Bound : { Terrain, Previous })
			{
				if (UMeshPartitionEditorComponent* Component = EditorComponent(Bound))
					Component->UpdateModifierList();
			}
		}
		UModifierComponent* Spawn(UClass* Class, const FVector& Location, const FRotator& Rotation, const FString& Label)
		{
			Modifier = SpawnModifier(Terrain, Class, Location, Rotation, Label, Previous);
			return Modifier;
		}
		FModifierOpResult Keep()
		{
			bKept = true;
			return Finish(Modifier, { Terrain, Previous });
		}
	};

	/**
	 * A weight utility modifier's build cache key leaves out its InnerValue / OuterValue (UE 5.8): after changing them the
	 * terrain rebuilds from the cached old weights. Its position is part of the key, so lift the actor by a hundredth of a
	 * millimetre (the weight is a vertical projection: its height within MaxZDistance does not matter) to get a new key.
	 */
	void RefreshWeightCacheKey(UModifierComponent* Modifier)
	{
		AActor* Owner = Modifier->GetOwner();
		Owner->Modify();
		Owner->SetActorLocation(Owner->GetActorLocation() + FVector(0.0, 0.0, 0.001), /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		Owner->PostEditMove(/*bFinished*/ true);
	}

	/** Room (cm) left above and below the heights a modifier must reach. */
	constexpr double ReachSpare = 10000.0;

	/** Raise @p Modifier's numeric property @p Name to @p Needed when it is lower (through SetNumber: one undo step with the edit). */
	bool WidenTo(UModifierComponent* Modifier, const TCHAR* Name, double Needed)
	{
		const FNumericProperty* Property = CastField<FNumericProperty>(Modifier->GetClass()->FindPropertyByName(Name));
		return !Property || Needed <= Property->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Modifier)) || SetNumber(Modifier, Name, Needed);
	}

	/**
	 * A Patch / WeightUtility modifier reaches the ground only within MaxZDistance / 2 above and below its location (default
	 * 200 m in all): a pad or paint on a 300 m hill would miss the ground above. Widen it to every height the terrain's built
	 * ground has around @p Location, with ReachSpare to spare.
	 */
	bool CoverTerrainHeights(UModifierComponent* Modifier, const AMeshPartition* Terrain, const FVector& Location)
	{
		const FBox Built = BuiltBounds(Terrain); // valid: the Add operations wait for the build (CheckFootprint) first
		// MaxZDistance is the whole span, centred on the location.
		return !Built.IsValid || WidenTo(Modifier, TEXT("MaxZDistance"), 2.0 * (FMath::Max(Built.Max.Z - Location.Z, Location.Z - Built.Min.Z) + ReachSpare));
	}

	/**
	 * A TexturePatch moves only the ground within VerticalPatchExtentUp / Down of its location (default 100 m each) and clamps
	 * what it writes to them: a 300 m stamp would be cut at 100 m. Widen them to the terrain's built heights plus the stamp's
	 * @p Height (its EncodingScale), with ReachSpare to spare.
	 */
	bool CoverTexturePatch(UModifierComponent* Patch, const AMeshPartition* Terrain, const FVector& Location, double Height)
	{
		const FBox Built = BuiltBounds(Terrain);
		const double Above = Built.IsValid ? FMath::Max(Built.Max.Z - Location.Z, 0.0) : 0.0; // ground above the patch
		const double Below = Built.IsValid ? FMath::Max(Location.Z - Built.Min.Z, 0.0) : 0.0; // and below it
		const double Reach = FMath::Abs(Height) + ReachSpare;
		// The extents are in the patch's own space: a patch scaled down on Z needs larger ones for the same world heights.
		const double ScaleZ = FMath::Max(FMath::Abs(Patch->GetComponentScale().Z), UE_KINDA_SMALL_NUMBER);
		return WidenTo(Patch, TEXT("VerticalPatchExtentUp"), (Above + Reach) / ScaleZ) && WidenTo(Patch, TEXT("VerticalPatchExtentDown"), (Below + Reach) / ScaleZ);
	}

	/** The guards every Add operation runs first: available, a terrain of the open level (its actors loaded), a world Location. */
	bool CheckAdd(const AMeshPartition* Terrain, const FVector& Location)
	{
		if (!CheckTerrain(Terrain))
			return false;
		return IsPoint(Location) || UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	}

	// ---- Shapes ------------------------------------------------------------------------------------------------------

	const TArray<FString>& ShapeNames()
	{
		static const TArray<FString> Names = { TEXT("Box"), TEXT("Sphere"), TEXT("Cylinder"), TEXT("Capsule"), TEXT("Cone"), TEXT("Torus"), TEXT("Rectangle"), TEXT("Disc") };
		return Names;
	}

	bool IsFlatShape(const FString& Shape)
	{
		return Shape == TEXT("Rectangle") || Shape == TEXT("Disc");
	}

	/**
	 * Fill @p Mesh with @p Shape (one of ShapeNames) of @p Size (full extents, cm) centred on the origin, the way Mesh
	 * Terrain Mode's Shapes tools generate them (GeometryScript primitives): closed solids, except the flat Rectangle and Disc
	 * (facing up, at Z 0).
	 */
	bool BuildShape(UDynamicMesh* Mesh, const FString& Shape, const FVector& Size)
	{
		const FGeometryScriptPrimitiveOptions Options;
		using UPrimitives = UGeometryScriptLibrary_MeshPrimitiveFunctions;
		const EGeometryScriptPrimitiveOriginMode Center = EGeometryScriptPrimitiveOriginMode::Center;
		// Round shapes are made 100 cm across and scaled to Size (an ellipse when X != Y).
		const FTransform Unit(FQuat::Identity, FVector::ZeroVector, Size / 100.0);
		if (Shape == TEXT("Box"))
			UPrimitives::AppendBox(Mesh, Options, FTransform::Identity, Size.X, Size.Y, Size.Z, 0, 0, 0, Center);
		else if (Shape == TEXT("Sphere"))
			UPrimitives::AppendSphereLatLong(Mesh, Options, Unit, 50.0f, 16, 24, Center);
		else if (Shape == TEXT("Cylinder"))
			UPrimitives::AppendCylinder(Mesh, Options, Unit, 50.0f, 100.0f, 32, 0, /*bCapped*/ true, Center);
		else if (Shape == TEXT("Cone"))
			UPrimitives::AppendCone(Mesh, Options, Unit, 50.0f, 0.5f, 100.0f, 32, 4, /*bCapped*/ true, Center);
		else if (Shape == TEXT("Capsule"))
		{
			// Round across its smaller side; the cylinder between the caps takes the rest of the height.
			const double Radius = FMath::Min(Size.X, Size.Y) / 2.0;
			const double Line = FMath::Max(Size.Z - 2.0 * Radius, 0.0);
			const FTransform Stretch(FQuat::Identity, FVector::ZeroVector, FVector(Size.X / (2.0 * Radius), Size.Y / (2.0 * Radius), Size.Z / (2.0 * Radius + Line)));
			UPrimitives::AppendCapsule(Mesh, Options, Stretch, static_cast<float>(Radius), static_cast<float>(Line), 8, 24, 0, Center);
		}
		else if (Shape == TEXT("Torus"))
		{
			// X and Y are the outer diameter, Z the ring's height. The ring is at most a third of the radius thick (so a third
			// of the diameter stays open as the hole) and stretched up or down to Size.Z.
			const double Minor = FMath::Min(Size.Z / 2.0, Size.X / 6.0);
			const double Major = Size.X / 2.0 - Minor;
			const FTransform Stretch(FQuat::Identity, FVector::ZeroVector, FVector(1.0, Size.Y / Size.X, Size.Z / (2.0 * Minor)));
			UPrimitives::AppendTorus(Mesh, Options, Stretch, FGeometryScriptRevolveOptions(), static_cast<float>(Major), static_cast<float>(Minor), 32, 12, Center);
		}
		else if (Shape == TEXT("Rectangle"))
			UPrimitives::AppendRectangleXY(Mesh, Options, FTransform::Identity, Size.X, Size.Y, 8, 8);
		else if (Shape == TEXT("Disc"))
			UPrimitives::AppendDisc(Mesh, Options, FTransform(FQuat::Identity, FVector::ZeroVector, FVector(Size.X / 100.0, Size.Y / 100.0, 1.0)), 50.0f, 32, 4);
		else
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("unknown shape '%s'"), *Shape));
		return Mesh->GetTriangleCount() > 0 || UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("could not generate a %s of size %s"), *Shape, *Text(Size)));
	}

	/** The shape mesh component of a modifier AddShape made (the tagged UDynamicMeshComponent on its actor) and its shape. */
	UDynamicMeshComponent* ShapeComponent(const UModifierComponent* Modifier, FString* OutShape = nullptr)
	{
		const AActor* Owner = Modifier ? Modifier->GetOwner() : nullptr;
		TArray<UDynamicMeshComponent*> Components;
		if (Owner)
			Owner->GetComponents(Components);
		for (UDynamicMeshComponent* Component : Components)
		{
			for (const FName& Tag : Component->ComponentTags)
			{
				FString Shape = Tag.ToString();
				if (Shape.RemoveFromStart(ShapeTagPrefix))
				{
					if (OutShape)
						*OutShape = Shape;
					return Component;
				}
			}
		}
		return nullptr;
	}

	/** The size (full extents, cm) the shape's mesh was made with, kept in the component tag "MeshTerrainShapeSize:X=.. Y=.. Z=..". */
	FVector ShapeSize(const UDynamicMeshComponent* Component)
	{
		for (const FName& Tag : Component->ComponentTags)
		{
			FString Text = Tag.ToString();
			FVector Size;
			if (Text.RemoveFromStart(TEXT("MeshTerrainShapeSize:")) && Size.InitFromString(Text))
				return Size;
		}
		return FVector(-1.0);
	}

	void TagShape(UDynamicMeshComponent* Component, const FString& Shape, const FVector& Size)
	{
		Component->ComponentTags.Add(FName(ShapeTagPrefix + Shape));
		Component->ComponentTags.Add(FName(TEXT("MeshTerrainShapeSize:") + Size.ToString()));
	}

	/** Point a Boolean / MeshProject modifier at @p Component's mesh and rebuild its mesh instance from it. */
	void UseShapeMesh(UModifierComponent* Modifier, UDynamicMeshComponent* Component)
	{
		UMeshBasedModifierBase* MeshBased = Cast<UMeshBasedModifierBase>(Modifier);
		MeshBased->Modify();
		MeshBased->SetMeshSourceMode(UE::MeshPartition::EModifierMeshSourceMode::DynamicMeshComponent);
		MeshBased->SetMeshComponent(Component);
		if (UBooleanModifier* Boolean = Cast<UBooleanModifier>(Modifier))
			Boolean->UpdateFromMesh();
		else if (UMeshProjectModifier* Project = Cast<UMeshProjectModifier>(Modifier))
			Project->UpdateFromMesh();
		Modifier->OnChanged(Modifier->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
	}

	// ---- Splines -----------------------------------------------------------------------------------------------------

	bool CheckPoints(const TArray<FVector>& Points, bool bClosed = false)
	{
		if (Points.Num() < 2 || Points.Num() > 1000)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%d points: a spline needs 2 .. 1000 world points [[x, y, z], ...]"), Points.Num()));
		if (bClosed && Points.Num() < 3)
			return UnrealMcpRql::Failed<bool>(TEXT("a closed spline needs at least 3 points"));
		if (bClosed && FVector::DistSquared(Points[0], Points.Last()) < 1.0)
			return UnrealMcpRql::Failed<bool>(TEXT("a closed spline joins its last point to the first itself: leave the repeated first point out"));
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			if (!IsPoint(Points[Index]))
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("point %d %s is not a world position"), Index, *Text(Points[Index])));
			if (Index > 0 && FVector::DistSquared(Points[Index], Points[Index - 1]) < 1.0)
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("points %d and %d are the same place: remove the duplicate"), Index - 1, Index));
		}
		return true;
	}

	FBox2D PointsArea(const TArray<FVector>& Points, double Pad)
	{
		FBox2D Area(ForceInit);
		for (const FVector& Point : Points)
			Area += FVector2D(Point);
		return Area.ExpandBy(Pad);
	}

	/** The spline component a Spline / SplineRemesh modifier follows (made for a SplineRemesh modifier that has none). */
	USplineComponent* SplineOf(UModifierComponent* Modifier, bool bCreate)
	{
		if (USplineModifier* Spline = Cast<USplineModifier>(Modifier))
			return Spline->GetSplineComponent();
		AActor* Owner = Modifier ? Modifier->GetOwner() : nullptr;
		if (!Cast<USplineRemeshModifier>(Modifier) || !Owner)
			return nullptr;
		if (USplineComponent* Existing = Owner->FindComponentByClass<USplineComponent>())
		{
			// Make sure the modifier follows the spline the points go to.
			if (bCreate)
				Cast<USplineRemeshModifier>(Modifier)->SetSplineComponent(Existing, /*bUpdate*/ false);
			return Existing;
		}
		if (!bCreate)
			return nullptr;
		Owner->Modify();
		USplineComponent* Spline = NewObject<USplineComponent>(Owner, TEXT("Spline"), RF_Transactional);
		Spline->SetupAttachment(Owner->GetRootComponent());
		Owner->AddInstanceComponent(Spline);
		Spline->RegisterComponent();
		Cast<USplineRemeshModifier>(Modifier)->SetSplineComponent(Spline, /*bUpdate*/ false);
		return Spline;
	}

	/** Make @p Modifier's spline pass through @p Points (world), then let the modifier re-read it. */
	bool ApplySplinePoints(UModifierComponent* Modifier, const TArray<FVector>& Points, bool bClosed)
	{
		USplineComponent* Spline = SplineOf(Modifier, /*bCreate*/ true);
		if (!Spline)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no spline: only Spline and SplineRemesh modifiers follow points"), *ModifierKind(Modifier)));
		Spline->Modify();
		Spline->ClearSplinePoints(/*bUpdateSpline*/ false);
		for (const FVector& Point : Points)
			Spline->AddSplinePoint(Point, ESplineCoordinateSpace::World, /*bUpdateSpline*/ false);
		Spline->SetClosedLoop(bClosed, /*bUpdateSpline*/ false);
		Spline->UpdateSpline();
		// A closed spline modifier fills its inside (a plateau outline), an open one is a path.
		if (USplineModifier* SplineModifier = Cast<USplineModifier>(Modifier))
		{
			if (!SetProperty(SplineModifier, TEXT("bMeshClosedInterior"), bClosed ? TEXT("True") : TEXT("False")))
				return false;
			SplineModifier->UpdateSplineData();
		}
		else if (USplineRemeshModifier* Remesh = Cast<USplineRemeshModifier>(Modifier))
			Remesh->UpdateSplineData();
		Modifier->OnChanged(Modifier->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
		return true;
	}

	/** A Remesh box's coverage (Size, cm) and edge length, as AddRemesh and AddRoad set them. */
	bool SetRemesh(UModifierComponent* Remesh, const FVector& Size, double EdgeLength)
	{
		return SetProperty(Remesh, TEXT("UnscaledCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=%.17g)"), Size.X, Size.Y, Size.Z))
			&& SetNumber(Remesh, TEXT("TargetEdgeLength"), EdgeLength);
	}

	/** A spline modifier's flat half-width, falloff, paint channel ("" none) and path, as AddSpline and AddRoad set them. */
	bool SetSpline(AMeshPartition* Terrain, UModifierComponent* Spline, const TArray<FVector>& Points, double Plateau, double Falloff, const FString& Channel, bool bClosed)
	{
		return SetNumber(Spline, TEXT("PlateauDistance"), Plateau) && SetNumber(Spline, TEXT("FalloffDistance"), Falloff)
			&& SetProperty(Spline, TEXT("WeightChannels"), Channel.IsEmpty() ? FString(TEXT("()"))
				: FString::Printf(TEXT("((WeightChannelName=(Name=\"%s\"),Value=1.000000))"), *ChannelName(Terrain, Channel)))
			&& ApplySplinePoints(Spline, Points, bClosed);
	}

	// ---- Lattices ----------------------------------------------------------------------------------------------------

	/**
	 * The lattice's control points, in its own order (X slowest, Z fastest, as FFFDLattice::GenerateInitialLatticePositions).
	 * UE 5.8's lattice modifier keeps and applies them in world space (verified: a grid stored around the lattice's own
	 * location deforms nothing until it is stored at the world positions); it moves them itself when its actor moves.
	 */
	TArray<FVector> LatticePoints(const ULatticeModifier* Lattice)
	{
		TArray<FVector3d> Stored;
		static_cast<const ILatticeStateStorage*>(Lattice)->ReadLatticePoints(Stored);
		return TArray<FVector>(Stored);
	}

	void StoreLatticeWorldPoints(ULatticeModifier* Lattice, const TArray<FVector>& Points)
	{
		Lattice->Modify();
		static_cast<ILatticeStateStorage*>(Lattice)->StoreLatticePoints(TArray<FVector3d>(Points));
		// A lattice's build cache key is not derived from its points: change it, or the cached old shape is built again.
		Lattice->UpdateCacheKey(/*bInChangeCacheKey*/ true);
		static_cast<UModifierComponent*>(Lattice)->OnChanged(static_cast<UModifierComponent*>(Lattice)->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
	}

	/**
	 * Give @p Lattice an undeformed grid of @p Resolution points per axis spanning @p Extents (full size, cm) around it. A
	 * lattice does not remake its points when Extents or LatticeResolution change outside its interactive tool: a point
	 * count that no longer matches the resolution fails an ensure in its build.
	 */
	void ResetLattice(ULatticeModifier* Lattice, const FVector& Extents, const FIntVector& Resolution)
	{
		const FVector Center = Lattice->GetComponentLocation();
		const FVector Steps(FMath::Max(Resolution.X - 1, 1), FMath::Max(Resolution.Y - 1, 1), FMath::Max(Resolution.Z - 1, 1));
		TArray<FVector> Points;
		for (int32 X = 0; X < Resolution.X; ++X)
		{
			for (int32 Y = 0; Y < Resolution.Y; ++Y)
			{
				for (int32 Z = 0; Z < Resolution.Z; ++Z)
					Points.Add(Center + Extents * (FVector(X, Y, Z) / Steps - 0.5));
			}
		}
		StoreLatticeWorldPoints(Lattice, Points);
	}
}

bool UnrealAIMeshTerrainModifiersLocal::IsKind(const UModifierComponent* Modifier, const TCHAR* Kind)
{
	const UClass* Class = ModifierClassOf(Kind);
	return Class && Modifier && Modifier->IsA(Class);
}

using namespace UnrealAIMeshTerrainModifiersLocal;

TArray<FVector> UnrealAIMeshTerrain::LatticeWorldPoints(const UE::MeshPartition::UModifierComponent* Modifier)
{
	const ULatticeModifier* Lattice = Cast<ULatticeModifier>(Modifier);
	return Lattice ? LatticePoints(Lattice) : TArray<FVector>();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddNoise(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector2D Size, double Strength, double Frequency, double Falloff, const FString& Label)
{
	if (!CheckAdd(Terrain, Location))
		return FModifierOpResult();
	if (!IsLength(Size.X) || !IsLength(Size.Y))
		return Fail(FString::Printf(TEXT("Size [%g, %g] must be above 0 and at most 10000000 cm on X and Y"), Size.X, Size.Y));
	if (!FMath::IsFinite(Strength) || FMath::Abs(Strength) > 1000000.0)
		return Fail(FString::Printf(TEXT("Strength %g cm is outside -1000000 .. 1000000"), Strength));
	if (!FMath::IsFinite(Frequency) || Frequency <= 0.0 || Frequency > 1.0)
		return Fail(FString::Printf(TEXT("Frequency %g is outside 0 .. 1 (1/cm: 0.001 = features about 10 m apart)"), Frequency));
	if (!FMath::IsFinite(Falloff) || Falloff < 0.0 || Falloff > 1.0)
		return Fail(FString::Printf(TEXT("Falloff %g is outside 0 .. 1 (the share of the area that fades out)"), Falloff));
	if (!CheckFootprint(Terrain, Around(Location, Size.X / 2.0, Size.Y / 2.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddNoise", "Add Mesh Terrain Noise"));
	FPlacement Placed(Terrain);
	UModifierComponent* Noise = Placed.Spawn(ModifierClassOf(TEXT("Noise")), Location, FRotator::ZeroRotator, Label);
	if (!Noise || !SetProperty(Noise, TEXT("UnscaledCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=50000)"), Size.X, Size.Y))
		|| !SetNumber(Noise, TEXT("Intensity"), Strength) || !SetProperty(Noise, TEXT("NoiseFrequency"), FString::Printf(TEXT("(X=%.17g,Y=%.17g)"), Frequency, Frequency))
		|| !SetNumber(Noise, TEXT("Falloff"), Falloff))
		return FModifierOpResult();
	return Placed.Keep();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddPatch(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, double Radius, double Falloff, const FString& Label)
{
	if (!CheckAdd(Terrain, Location))
		return FModifierOpResult();
	if (!IsLength(Radius) || !IsLength(Falloff, /*bAllowZero*/ true))
		return Fail(FString::Printf(TEXT("Radius %g must be above 0 and Falloff %g at least 0, both at most 10000000 cm"), Radius, Falloff));
	if (!CheckFootprint(Terrain, Around(Location, Radius + Falloff, Radius + Falloff)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddPatch", "Add Mesh Terrain Patch"));
	FPlacement Placed(Terrain);
	UModifierComponent* Patch = Placed.Spawn(ModifierClassOf(TEXT("Patch")), Location, FRotator::ZeroRotator, Label);
	if (!Patch || !SetNumber(Patch, TEXT("Radius"), Radius) || !SetNumber(Patch, TEXT("Falloff"), Falloff) || !CoverTerrainHeights(Patch, Terrain, Location))
		return FModifierOpResult();
	return Placed.Keep();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddWeight(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, FVector Location, double Radius, double Falloff, double Value, const FString& Label)
{
	if (!CheckAdd(Terrain, Location) || !CheckChannel(Terrain, Channel, /*bAllowEmpty*/ false))
		return FModifierOpResult();
	if (!IsLength(Radius) || !IsLength(Falloff, /*bAllowZero*/ true))
		return Fail(FString::Printf(TEXT("Radius %g must be above 0 and Falloff %g at least 0, both at most 10000000 cm"), Radius, Falloff));
	if (!FMath::IsFinite(Value) || Value < 0.0 || Value > 1.0)
		return Fail(FString::Printf(TEXT("Value %g is outside 0 .. 1"), Value));
	if (!CheckFootprint(Terrain, Around(Location, Radius + Falloff, Radius + Falloff)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddWeight", "Add Mesh Terrain Weight"));
	FPlacement Placed(Terrain);
	UModifierComponent* Weight = Placed.Spawn(ModifierClassOf(TEXT("WeightUtility")), Location, FRotator::ZeroRotator, Label);
	if (!Weight || !SetProperty(Weight, TEXT("WeightChannelName"), ChannelName(Terrain, Channel)) || !SetNumber(Weight, TEXT("Radius"), Radius)
		|| !SetNumber(Weight, TEXT("Falloff"), Falloff) || !SetNumber(Weight, TEXT("InnerValue"), Value) || !SetNumber(Weight, TEXT("OuterValue"), 0.0)
		|| !CoverTerrainHeights(Weight, Terrain, Location))
		return FModifierOpResult();
	return Placed.Keep();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddSpline(UE::MeshPartition::AMeshPartition* Terrain, const TArray<FVector>& Points, double Width, double Falloff, bool Closed, const FString& Channel, const FString& Label)
{
	if (!CheckTerrain(Terrain) || !CheckPoints(Points, Closed) || !CheckChannel(Terrain, Channel, /*bAllowEmpty*/ true))
		return FModifierOpResult();
	if (!IsLength(Width, /*bAllowZero*/ true) || !IsLength(Falloff) || Falloff < 0.1)
		return Fail(FString::Printf(TEXT("Width %g must be at least 0 and Falloff %g at least 0.1, both at most 10000000 cm"), Width, Falloff));
	if (!CheckFootprint(Terrain, PointsArea(Points, Width / 2.0 + Falloff)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddSpline", "Add Mesh Terrain Spline"));
	FPlacement Placed(Terrain);
	UModifierComponent* Spline = Placed.Spawn(ModifierClassOf(TEXT("Spline")), Points[0], FRotator::ZeroRotator, Label);
	if (!Spline || !SetSpline(Terrain, Spline, Points, Width / 2.0, Falloff, Channel, Closed))
		return FModifierOpResult();
	return Placed.Keep();
}

FUnrealAIMeshTerrainRoadResult UUnrealAIMeshTerrainModifiersApi::AddRoad(UE::MeshPartition::AMeshPartition* Terrain, const TArray<FVector>& Points, double Width, double MaxSlope, double Falloff, const FString& Label)
{
	using FRoadResult = FUnrealAIMeshTerrainRoadResult;
	const double Start = FPlatformTime::Seconds();
	if (!CheckTerrain(Terrain) || !CheckPoints(Points))
		return FRoadResult();
	if (!FMath::IsFinite(Width) || Width < 100.0 || Width > 10000.0)
		return UnrealMcpRql::Failed<FRoadResult>(FString::Printf(TEXT("Width %g is outside 100 .. 10000 cm"), Width));
	if (!FMath::IsFinite(MaxSlope) || MaxSlope < 0.5 || MaxSlope > 45.0)
		return UnrealMcpRql::Failed<FRoadResult>(FString::Printf(TEXT("MaxSlope %g is outside 0.5 .. 45 degrees"), MaxSlope));
	if (Falloff < 0.0)
		Falloff = Width;
	if (!FMath::IsFinite(Falloff) || Falloff < 10.0 || Falloff > 100000.0)
		return UnrealMcpRql::Failed<FRoadResult>(FString::Printf(TEXT("Falloff %g is outside 10 .. 100000 cm (-1: Width)"), Falloff));
	if (!CheckFootprint(Terrain, PointsArea(Points, Width / 2.0 + Falloff)))
		return FRoadResult();

	const double RoadEdge = FMath::Clamp(Width / 6.0, 50.0, 300.0); // remesh edges under a road
	// The path every few metres (at most 1000 points: the spline's limit).
	double Length = 0.0;
	for (int32 Index = 1; Index < Points.Num(); ++Index)
	{
		const double Segment = FVector2D::Distance(FVector2D(Points[Index - 1]), FVector2D(Points[Index]));
		if (Segment < 1.0) // the path is X, Y: points apart only in Z are the same place
			return UnrealMcpRql::Failed<FRoadResult>(FString::Printf(TEXT("points %d and %d are the same X, Y: remove the duplicate"), Index - 1, Index));
		Length += Segment;
	}
	auto Samples = [&Points](double Step)
	{
		int32 Count = 1;
		for (int32 Index = 1; Index < Points.Num(); ++Index)
			Count += FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(FVector2D(Points[Index - 1]), FVector2D(Points[Index])) / Step));
		return Count;
	};
	double Step = FMath::Max(FMath::Clamp(Width, 200.0, 1000.0), Length / 999.0);
	while (Samples(Step) > 1000)
		Step *= 1.1;
	TArray<FVector2D> Path = { FVector2D(Points[0]) };
	TArray<double> Along = { 0.0 };
	for (int32 Index = 1; Index < Points.Num(); ++Index)
	{
		const FVector2D From(Points[Index - 1]), To(Points[Index]);
		const double Segment = FVector2D::Distance(From, To);
		const int32 Pieces = FMath::Max(1, FMath::CeilToInt(Segment / Step));
		for (int32 Piece = 1; Piece <= Pieces; ++Piece)
		{
			Path.Add(FMath::Lerp(From, To, double(Piece) / Pieces));
			Along.Add(Along.Last() + Segment / Pieces);
		}
	}
	TArray<double> Ground;
	for (const FVector2D& Point : Path)
	{
		const FSurfaceSample Sample = SampleSurface(Terrain, Point.X, Point.Y, /*bChannels*/ false);
		if (!Sample.bInside)
			return UnrealMcpRql::Failed<FRoadResult>(FString::Printf(TEXT("the road leaves %s at X %.0f, Y %.0f: keep its points on the terrain"), *Terrain->GetActorNameOrLabel(), Point.X, Point.Y));
		Ground.Add(Sample.Height);
	}

	// The grade-limited profile (MaxSlope with a margin for the spline's curve and the mesh). When the climb fits the path,
	// both ends sit on the ground: every point stays within what the start and the end can reach (so the road starts
	// climbing early enough), as close to the ground as that allows. Otherwise it follows the ground from the start as far up
	// as it can. Then at most the grade per step, forwards and backwards.
	const double Tan = FMath::Tan(FMath::DegreesToRadians(MaxSlope * 0.9));
	const bool bFits = FMath::Abs(Ground.Last() - Ground[0]) <= Length * Tan;
	TArray<double> Z = Ground;
	if (bFits)
	{
		for (int32 Index = 0; Index < Z.Num(); ++Index)
		{
			const double FromStart = Along[Index] * Tan, FromEnd = (Length - Along[Index]) * Tan;
			Z[Index] = FMath::Clamp(Ground[Index], FMath::Max(Ground[0] - FromStart, Ground.Last() - FromEnd), FMath::Min(Ground[0] + FromStart, Ground.Last() + FromEnd));
		}
	}
	for (int32 Index = 1; Index < Z.Num(); ++Index)
	{
		const double Rise = (Along[Index] - Along[Index - 1]) * Tan;
		Z[Index] = FMath::Clamp(Z[Index], Z[Index - 1] - Rise, Z[Index - 1] + Rise);
	}
	for (int32 Index = Z.Num() - 2; Index >= 0; --Index)
	{
		const double Rise = (Along[Index + 1] - Along[Index]) * Tan;
		Z[Index] = FMath::Clamp(Z[Index], Z[Index + 1] - Rise, Z[Index + 1] + Rise);
	}
	TArray<FVector> Road;
	for (int32 Index = 0; Index < Path.Num(); ++Index)
	{
		Road.Add(FVector(Path[Index], Z[Index]));
	}

	// A Boolean shape's surface keeps its own coarse triangles (100+ m across), which no spline can bend: remesh it finer than
	// the road first.
	// The remesh covers each crossed shape whole: a coarse triangle straddling the remesh's edge stays as it is.
	TArray<UModifierComponent*> Existing = Modifiers(Terrain);
	FBox Coarse(ForceInit);
	for (const UModifierComponent* Modifier : Existing)
	{
		const FBox Box = Modifier->ComputeCombinedBounds();
		if (!IsKind(Modifier, TEXT("Boolean")) || Modifier->GetIsDisabledFlag() || !Box.IsValid)
			continue;
		const FBox2D Area(FVector2D(Box.Min), FVector2D(Box.Max));
		bool bCrossed = false;
		for (int32 Index = 1; Index < Path.Num() && !bCrossed; ++Index)
		{
			FBox2D Segment(ForceInit);
			Segment += Path[Index - 1];
			Segment += Path[Index];
			bCrossed = Area.Intersect(Segment);
		}
		if (bCrossed)
			Coarse += Box.ExpandBy(FVector(0.05 * Box.GetSize().X + 1000.0, 0.05 * Box.GetSize().Y + 1000.0, 0.0));
	}
	double Order = 0.0;
	for (const UModifierComponent* Modifier : Existing)
		Order = FMath::Max(Order, double(Modifier->GetPriority()));

	FTerrainTransaction Transaction(LOCTEXT("AddRoad", "Add Mesh Terrain Road"));
	FPlacement Remeshed(Terrain);
	FString RemeshLabel;
	if (Coarse.IsValid)
	{
		// A Remesh box (a SplineRemesh along the path leaves a Boolean shape's triangles as they are).
		FBox Box = Coarse;
		const FBox Built = BuiltBounds(Terrain);
		Box.Min.Z = FMath::Min(Built.Min.Z, Box.Min.Z) - 1000.0;
		Box.Max.Z = FMath::Max(Built.Max.Z, Box.Max.Z) + 1000.0;
		const FVector Size = Box.GetSize();
		// Edges a sixth of the road's width, longer when that would pass AddRemesh's triangle limit.
		const double Edge = FMath::Max(RoadEdge, FMath::Sqrt(2.0 * Size.X * Size.Y / MaxRemeshTriangles));
		UModifierComponent* Remesh = Remeshed.Spawn(ModifierClassOf(TEXT("Remesh")), Box.GetCenter(), FRotator::ZeroRotator, Label.IsEmpty() ? FString() : Label + TEXT("_Remesh"));
		if (!Remesh || !SetRemesh(Remesh, Size, Edge) || !SetNumber(Remesh, TEXT("Priority"), Order + 1.0))
			return FRoadResult();
		RemeshLabel = Remesh->GetOwner()->GetActorNameOrLabel();
	}
	FPlacement Placed(Terrain);
	UModifierComponent* Spline = Placed.Spawn(ModifierClassOf(TEXT("Spline")), Road[0], FRotator::ZeroRotator, Label);
	// Flat 1 m past each edge, so the whole Width is flat (the banks start outside it).
	// The spline reaches the ground only within MaxZDistance of its points (engine default 20 m): cover the deepest cut or fill.
	double Deepest = 0.0;
	for (int32 Index = 0; Index < Z.Num(); ++Index)
		Deepest = FMath::Max(Deepest, FMath::Abs(Ground[Index] - Z[Index]));
	if (!Spline || !SetNumber(Spline, TEXT("Priority"), Order + 2.0) || !SetNumber(Spline, TEXT("MaxZDistance"), FMath::Max(2000.0, Deepest + 1000.0))
		|| !SetSpline(Terrain, Spline, Road, Width / 2.0 + 100.0, Falloff, FString(), /*bClosed*/ false))
		return FRoadResult();
	Remeshed.bKept = true;
	const FModifierOpResult Built = Placed.Keep();
	if (!Built.Modifier)
		return FRoadResult();

	FRoadResult Out;
	Out.Modifier = Spline;
	Out.Actor = Built.Actor;
	Out.Remesh = RemeshLabel;
	Out.Terrain = Built.Terrain;
	Out.Ready = Built.Ready;
	Out.State = Built.State;
	Out.LengthMeters = Length / 100.0;
	Out.StartZ = Z[0];
	Out.EndZ = Z.Last();
	Out.EndGroundZ = Ground.Last();
	Out.Reached = FMath::Abs(Z.Last() - Ground.Last()) <= 100.0;
	Out.MinLengthMeters = FMath::Abs(Ground.Last() - Ground[0]) / Tan / 100.0; // with the profile's margin: what it needs
	// The built road: its grade along the centre line, from the collision.
	double Previous = 0.0;
	for (int32 Index = 0; Index < Path.Num(); ++Index)
	{
		const double Height = SampleSurface(Terrain, Path[Index].X, Path[Index].Y, /*bChannels*/ false).Height;
		if (Index > 0 && Along[Index] > Along[Index - 1])
			Out.MaxSlope = FMath::Max(Out.MaxSlope, FMath::RadiansToDegrees(FMath::Atan(FMath::Abs(Height - Previous) / (Along[Index] - Along[Index - 1]))));
		Previous = Height;
	}
	Out.MaxSlope = FMath::RoundToDouble(Out.MaxSlope * 100.0) / 100.0;
	if (!Out.Reached)
	{
		Out.Note = FString::Printf(TEXT("the path is %.0f m long: at %g degrees the road reaches Z %.0f, %.0f m %s the last point's ground (Z %.0f); a path of at least %.0f m reaches it: wind it (switchbacks or a spiral), Remove this road%s and call AddRoad again"),
			Out.LengthMeters, MaxSlope, Out.EndZ, FMath::Abs(Out.EndGroundZ - Out.EndZ) / 100.0, Out.EndZ < Out.EndGroundZ ? TEXT("below") : TEXT("above"), Out.EndGroundZ,
			FMath::CeilToDouble(Out.MinLengthMeters), RemeshLabel.IsEmpty() ? TEXT("") : TEXT(" and its Remesh"));
	}
	else if (Out.MaxSlope > MaxSlope + 0.5)
	{
		Out.Note = FString::Printf(TEXT("the built road is %.1f degrees steep somewhere: the ground under it is too coarse; AddRemesh over it (EdgeLength %.0f) and Rebuild"), Out.MaxSlope, RoadEdge);
	}
	Out.Seconds = SecondsSince(Start);
	return Out;
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddShape(UE::MeshPartition::AMeshPartition* Terrain, const FString& Shape, FVector Location, FVector Size, const FString& Operation, FRotator Rotation, const FString& Label)
{
	if (!CheckTerrain(Terrain))
		return FModifierOpResult();
	const FString* ShapeName = ShapeNames().FindByPredicate([&Shape](const FString& Name) { return Name.Equals(Shape.TrimStartAndEnd(), ESearchCase::IgnoreCase); });
	if (!ShapeName)
		return Fail(FString::Printf(TEXT("unknown Shape '%s': use %s"), *Shape, *FString::Join(ShapeNames(), TEXT(", "))));
	const bool bFlat = IsFlatShape(*ShapeName);
	FString Op = Operation.TrimStartAndEnd();
	if (Op.IsEmpty())
		Op = bFlat ? TEXT("Project") : TEXT("Union");
	static const TArray<FString> Operations = { TEXT("Union"), TEXT("Subtract"), TEXT("Trim"), TEXT("Project") };
	const FString* OpName = Operations.FindByPredicate([&Op](const FString& Name) { return Name.Equals(Op, ESearchCase::IgnoreCase); });
	if (!OpName)
		return Fail(FString::Printf(TEXT("unknown Operation '%s': use Union, Subtract, Trim or Project"), *Operation));
	if (bFlat && *OpName != TEXT("Project"))
		return Fail(FString::Printf(TEXT("a %s is flat (no volume to %s): use Operation Project, or a Box / Cylinder for a solid"), **ShapeName, **OpName));
	if (!IsPoint(Location) || Rotation.ContainsNaN())
		return Fail(FString::Printf(TEXT("Location %s / Rotation %s is not a world placement"), *Text(Location), *Rotation.ToString()));
	if (bFlat)
		Size.Z = 0.0; // a flat shape has no height
	if (!IsLength(Size.X) || !IsLength(Size.Y) || (!bFlat && !IsLength(Size.Z)))
		return Fail(FString::Printf(TEXT("Size %s must be above 0 and at most 10000000 cm on each axis"), *Text(Size)));
	const double Reach = 0.5 * Size.Size();
	if (!CheckFootprint(Terrain, Around(Location, Reach, Reach)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddShape", "Add Mesh Terrain Shape"));
	FPlacement Placed(Terrain);
	UModifierComponent* Modifier = Placed.Spawn(ModifierClassOf(*OpName == TEXT("Project") ? TEXT("MeshProject") : TEXT("Boolean")), Location, Rotation,
		Label.IsEmpty() ? UniqueLabel(Terrain->GetWorld(), *ShapeName) : Label);
	if (!Modifier)
		return FModifierOpResult();
	AActor* Owner = Modifier->GetOwner();
	// The shape lives on the modifier's actor as a dynamic mesh component, as Mesh Terrain Mode's Shapes tools make it;
	// hidden and without collision: the terrain shows it, and traces must hit the terrain.
	UDynamicMeshComponent* Mesh = NewObject<UDynamicMeshComponent>(Owner, TEXT("ShapeMesh"), RF_Transactional);
	Mesh->SetupAttachment(Owner->GetRootComponent());
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetVisibility(false);
	Mesh->SetHiddenInGame(true);
	Owner->AddInstanceComponent(Mesh);
	Mesh->RegisterComponent();
	TagShape(Mesh, *ShapeName, Size);
	if (!BuildShape(Mesh->GetDynamicMesh(), *ShapeName, Size))
		return FModifierOpResult();
	Mesh->NotifyMeshUpdated();
	if (*OpName != TEXT("Project") && !SetProperty(Modifier, TEXT("BooleanOp"), *OpName))
		return FModifierOpResult();
	UseShapeMesh(Modifier, Mesh);
	return Placed.Keep();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddLattice(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector Size, int32 Resolution, const FString& Label)
{
	if (!CheckAdd(Terrain, Location))
		return FModifierOpResult();
	if (!IsLength(Size.X) || !IsLength(Size.Y) || !IsLength(Size.Z))
		return Fail(FString::Printf(TEXT("Size %s must be above 0 and at most 10000000 cm on each axis"), *Text(Size)));
	if (Resolution < 2 || Resolution > 9)
		return Fail(FString::Printf(TEXT("Resolution %d is outside 2 .. 9 (control points per axis)"), Resolution));
	if (!CheckFootprint(Terrain, Around(Location, Size.X / 2.0, Size.Y / 2.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddLattice", "Add Mesh Terrain Lattice"));
	FPlacement Placed(Terrain);
	UModifierComponent* Lattice = Placed.Spawn(ModifierClassOf(TEXT("Lattice")), Location, FRotator::ZeroRotator, Label);
	if (!Lattice || !SetProperty(Lattice, TEXT("Extents"), FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=%.17g)"), Size.X, Size.Y, Size.Z))
		|| !SetProperty(Lattice, TEXT("LatticeResolution"), FString::Printf(TEXT("(X=%d,Y=%d,Z=%d)"), Resolution, Resolution, Resolution)))
		return FModifierOpResult();
	ResetLattice(Cast<ULatticeModifier>(Lattice), Size, FIntVector(Resolution));
	return Placed.Keep();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::SetLatticePoints(UE::MeshPartition::UModifierComponent* Modifier, const TArray<FVector>& Points)
{
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	ULatticeModifier* Lattice = Cast<ULatticeModifier>(Modifier);
	if (!Lattice)
		return Fail(FString::Printf(TEXT("%s is a %s modifier, not a Lattice"), *Modifier->GetOwner()->GetActorNameOrLabel(), *ModifierKind(Modifier)));
	const TArray<FVector> Current = LatticePoints(Lattice);
	if (Points.Num() != Current.Num())
		return Fail(FString::Printf(TEXT("%d points given, the lattice has %d (its LatticePoints member lists them in order)"), Points.Num(), Current.Num()));
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		if (!IsPoint(Points[Index]))
			return Fail(FString::Printf(TEXT("point %d %s is not a world position"), Index, *Text(Points[Index])));
	}
	FTerrainTransaction Transaction(LOCTEXT("SetLatticePoints", "Move Mesh Terrain Lattice Points"), { Modifier });
	StoreLatticeWorldPoints(Lattice, Points);
	return Finish(Modifier, { TerrainOf(Modifier) });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddTexturePatch(UE::MeshPartition::AMeshPartition* Terrain, UTexture2D* Texture, FVector Location, FVector2D Size, double Height, const FString& Label)
{
	if (!CheckAdd(Terrain, Location))
		return FModifierOpResult();
	if (!Texture)
		return Fail(TEXT("Texture is null: pass a heightmap texture asset path (\"/Game/Heightmaps/T_Hill\")"));
	if (!IsLength(Size.X) || !IsLength(Size.Y))
		return Fail(FString::Printf(TEXT("Size [%g, %g] must be above 0 and at most 10000000 cm on X and Y"), Size.X, Size.Y));
	if (!FMath::IsFinite(Height) || FMath::Abs(Height) > 1000000.0)
		return Fail(FString::Printf(TEXT("Height %g cm is outside -1000000 .. 1000000"), Height));
	UClass* EntryClass = FindObject<UClass>(nullptr, TEXT("/Script/MeshPartitionEditor.TexturePatchHeightEntry"));
	const UClass* PatchClass = ModifierClassOf(TEXT("TexturePatch"));
	const FObjectProperty* Entry = PatchClass ? CastField<FObjectProperty>(PatchClass->FindPropertyByName(TEXT("HeightChannel"))) : nullptr;
	if (!Entry || !EntryClass)
		return Fail(TEXT("this engine's TexturePatchModifier has no HeightChannel entry"));
	if (Texture->VirtualTextureStreaming)
		return Fail(FString::Printf(TEXT("%s is a virtual texture: a texture patch reads plain textures only (turn Virtual Texture Streaming off)"), *Texture->GetPathName()));
	if (!CheckFootprint(Terrain, Around(Location, Size.X / 2.0, Size.Y / 2.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddTexturePatch", "Add Mesh Terrain Texture Patch"));
	FPlacement Placed(Terrain);
	UModifierComponent* Patch = Placed.Spawn(ModifierClassOf(TEXT("TexturePatch")), Location, FRotator::ZeroRotator, Label);
	if (!Patch || !SetProperty(Patch, TEXT("UnscaledPatchCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g)"), Size.X, Size.Y)))
		return FModifierOpResult();
	// The height channel is an instanced entry object (UTexturePatchHeightEntry): its texture and scale.
	UObject* Height_ = Entry->GetObjectPropertyValue_InContainer(Patch);
	if (!Height_)
	{
		Height_ = NewObject<UObject>(Patch, EntryClass, NAME_None, RF_Transactional);
		if (!SetProperty(Patch, TEXT("HeightChannel"), Height_->GetPathName()))
			return FModifierOpResult();
	}
	if (!SetProperty(Height_, TEXT("TextureAsset"), Texture->GetPathName()) || !SetNumber(Height_, TEXT("EncodingScale"), Height)
		|| !CoverTexturePatch(Patch, Terrain, Location, Height))
		return FModifierOpResult();
	Patch->OnChanged(Patch->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
	return Placed.Keep();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddRemesh(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector Size, double EdgeLength, const FString& Label)
{
	if (!CheckAdd(Terrain, Location))
		return FModifierOpResult();
	if (!IsLength(Size.X) || !IsLength(Size.Y) || !IsLength(Size.Z))
		return Fail(FString::Printf(TEXT("Size %s must be above 0 and at most 10000000 cm on each axis"), *Text(Size)));
	if (!FMath::IsFinite(EdgeLength) || EdgeLength < 10.0 || EdgeLength > 100000.0)
		return Fail(FString::Printf(TEXT("EdgeLength %g is outside 10 .. 100000 cm"), EdgeLength));
	const double Triangles = 2.0 * (Size.X / EdgeLength) * (Size.Y / EdgeLength);
	if (Triangles > MaxRemeshTriangles)
		return Fail(FString::Printf(TEXT("a %.0f x %.0f cm remesh with %g cm edges makes about %.0f triangles: use longer edges or a smaller Size (at most %.0f)"), Size.X, Size.Y, EdgeLength, Triangles, MaxRemeshTriangles));
	if (!CheckFootprint(Terrain, Around(Location, Size.X / 2.0, Size.Y / 2.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddRemesh", "Add Mesh Terrain Remesh"));
	FPlacement Placed(Terrain);
	UModifierComponent* Remesh = Placed.Spawn(ModifierClassOf(TEXT("Remesh")), Location, FRotator::ZeroRotator, Label);
	if (!Remesh || !SetRemesh(Remesh, Size, EdgeLength))
		return FModifierOpResult();
	return Placed.Keep();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Add(UE::MeshPartition::AMeshPartition* Terrain, const FString& Kind, FVector Location, const FString& Label)
{
	if (!CheckAdd(Terrain, Location))
		return FModifierOpResult();
	UClass* Class = ModifierClassOf(Kind);
	if (!Class)
		return Fail(FString::Printf(TEXT("'%s' is not a modifier kind that can be added: use %s (base meshes come from the terrain's create / import, InstancedProjection / InstancedTexturePatch are filled by other tools)"),
			*Kind, *FString::Join(AddableKinds(), TEXT(", "))));
	if (!CheckFootprint(Terrain, Around(Location, 1000.0, 1000.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddModifier", "Add Mesh Terrain Modifier"));
	FPlacement Placed(Terrain);
	UModifierComponent* Modifier = Placed.Spawn(Class, Location, FRotator::ZeroRotator, Label);
	if (!Modifier)
		return FModifierOpResult();
	// A spline remesh needs a spline to follow: give it the two-point one a spline modifier starts with.
	if (Cast<USplineRemeshModifier>(Modifier) && !ApplySplinePoints(Modifier, { Location, Location + FVector(2000.0, 0.0, 0.0) }, false))
		return FModifierOpResult();
	// A lattice's own points are made around the world origin: give it its undeformed grid where it is.
	if (ULatticeModifier* Lattice = Cast<ULatticeModifier>(Modifier))
	{
		const FStructProperty* Extents = CastField<FStructProperty>(Lattice->GetClass()->FindPropertyByName(TEXT("Extents")));
		const FStructProperty* Resolution = CastField<FStructProperty>(Lattice->GetClass()->FindPropertyByName(TEXT("LatticeResolution")));
		if (!Extents || !Resolution)
			return Fail(TEXT("this engine's LatticeModifier has no Extents / LatticeResolution"));
		ResetLattice(Lattice, *Extents->ContainerPtrToValuePtr<FVector>(Lattice), *Resolution->ContainerPtrToValuePtr<FIntVector>(Lattice));
	}
	return Placed.Keep();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Edit(UE::MeshPartition::UModifierComponent* Modifier, FVector Size, double Radius, double Width, double Strength, double Falloff)
{
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	const FString Kind = ModifierKind(Modifier);
	const bool bSize = Size.X >= 0.0 || Size.Y >= 0.0 || Size.Z >= 0.0;
	if (!bSize && Radius < 0.0 && Width < 0.0 && Strength < 0.0 && Falloff < 0.0)
		return Fail(TEXT("nothing to change: give Size, Radius, Width, Strength or Falloff (a negative value keeps the current one)"));
	if (Size.ContainsNaN() || Size.GetMax() > MaxLength || !FMath::IsFinite(Radius) || Radius > MaxLength || !FMath::IsFinite(Width) || Width > MaxLength
		|| !FMath::IsFinite(Strength) || Strength > 1000000.0 || !FMath::IsFinite(Falloff) || Falloff > MaxLength)
		return Fail(TEXT("Size, Radius, Width and Falloff must be at most 10000000 cm and Strength at most 1000000"));
	const auto Unsupported = [&Kind](const TCHAR* Parameter)
	{
		return Fail(FString::Printf(TEXT("a %s modifier has no %s: set its properties instead (rql-schema %sModifier)"), *Kind, Parameter, *Kind));
	};
	// The current value of a vector property, with the given (non-negative) components of Size.
	const auto Resized = [&Size](const FVector& Current)
	{
		return FVector(Size.X >= 0.0 ? Size.X : Current.X, Size.Y >= 0.0 ? Size.Y : Current.Y, Size.Z >= 0.0 ? Size.Z : Current.Z);
	};
	const auto VectorProperty = [Modifier](const TCHAR* Name) -> FVector
	{
		const FStructProperty* Property = CastField<FStructProperty>(Modifier->GetClass()->FindPropertyByName(Name));
		if (!Property)
			return FVector::ZeroVector;
		if (Property->Struct == TBaseStructure<FVector2D>::Get())
		{
			const FVector2D Value = *Property->ContainerPtrToValuePtr<FVector2D>(Modifier);
			return FVector(Value.X, Value.Y, 0.0);
		}
		return *Property->ContainerPtrToValuePtr<FVector>(Modifier);
	};
	const auto SizeText = [](const FVector& Value) { return FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=%.17g)"), Value.X, Value.Y, Value.Z); };

	// Check every parameter against the kind first: a refused Edit changes nothing.
	FString Shape;
	UDynamicMeshComponent* ShapeMesh = ShapeComponent(Modifier, &Shape);
	ULatticeModifier* Lattice = Cast<ULatticeModifier>(Modifier);
	const bool bNoise = IsKind(Modifier, TEXT("Noise"));
	const bool bRemesh = IsKind(Modifier, TEXT("Remesh"));
	const bool bTexture = IsKind(Modifier, TEXT("TexturePatch"));
	const bool bPatch = IsKind(Modifier, TEXT("Patch"));
	const bool bWeight = IsKind(Modifier, TEXT("WeightUtility"));
	const bool bSpline = IsKind(Modifier, TEXT("Spline"));
	FVector NewSize = FVector::ZeroVector;
	FVector ShapeMade = FVector::ZeroVector;
	if (bSize)
	{
		if (ShapeMesh)
		{
			// A shape keeps the mesh it was made with and is resized by its actor's scale, which undo restores with the
			// actor (a regenerated mesh would not reach the modifier on undo). A Capsule's caps and a Torus's ring stretch with it.
			ShapeMade = ShapeSize(ShapeMesh);
			const FVector Scale = Modifier->GetOwner()->GetActorScale3D();
			const bool bFlat = IsFlatShape(Shape);
			if (ShapeMade.X <= 0.0 || ShapeMade.Y <= 0.0 || (!bFlat && ShapeMade.Z <= 0.0))
				return Fail(FString::Printf(TEXT("%s's shape size is unknown (its mesh component lost its MeshTerrainShapeSize tag)"), *Modifier->GetOwner()->GetActorNameOrLabel()));
			NewSize = Resized(FVector(ShapeMade.X * Scale.X, ShapeMade.Y * Scale.Y, bFlat ? 0.0 : ShapeMade.Z * Scale.Z));
			if (!IsLength(NewSize.X) || !IsLength(NewSize.Y) || (!bFlat && !IsLength(NewSize.Z)))
				return Fail(FString::Printf(TEXT("Size %s must be above 0 on each axis"), *Text(NewSize)));
		}
		else if (bNoise)
		{
			// A noise is a 2D area: Size Z is ignored (its vertical reach stays).
			const FVector Current = VectorProperty(TEXT("UnscaledCoverage"));
			NewSize = FVector(Size.X >= 0.0 ? Size.X : Current.X, Size.Y >= 0.0 ? Size.Y : Current.Y, Current.Z);
			if (!IsLength(NewSize.X) || !IsLength(NewSize.Y))
				return Fail(FString::Printf(TEXT("Size %s must be above 0 on X and Y"), *Text(NewSize)));
		}
		else if (bRemesh)
		{
			NewSize = Resized(VectorProperty(TEXT("UnscaledCoverage")));
			if (!IsLength(NewSize.X) || !IsLength(NewSize.Y) || !IsLength(NewSize.Z))
				return Fail(FString::Printf(TEXT("Size %s must be above 0 on each axis"), *Text(NewSize)));
			const FNumericProperty* Edge = CastField<FNumericProperty>(Modifier->GetClass()->FindPropertyByName(TEXT("TargetEdgeLength")));
			const double EdgeLength = Edge ? Edge->GetFloatingPointPropertyValue(Edge->ContainerPtrToValuePtr<void>(Modifier)) : 0.0;
			const double Triangles = EdgeLength > 0.0 ? 2.0 * (NewSize.X / EdgeLength) * (NewSize.Y / EdgeLength) : 0.0;
			if (Triangles > MaxRemeshTriangles)
				return Fail(FString::Printf(TEXT("a %.0f x %.0f cm remesh with %g cm edges makes about %.0f triangles: at most %.0f"), NewSize.X, NewSize.Y, EdgeLength, Triangles, MaxRemeshTriangles));
		}
		else if (Lattice)
		{
			NewSize = Resized(VectorProperty(TEXT("Extents")));
			if (!IsLength(NewSize.X) || !IsLength(NewSize.Y) || !IsLength(NewSize.Z))
				return Fail(FString::Printf(TEXT("Size %s must be above 0 on each axis"), *Text(NewSize)));
			const FStructProperty* ResolutionProperty = CastField<FStructProperty>(Modifier->GetClass()->FindPropertyByName(TEXT("LatticeResolution")));
			const FIntVector Resolution = ResolutionProperty ? *ResolutionProperty->ContainerPtrToValuePtr<FIntVector>(Modifier) : FIntVector(0);
			if (Resolution.GetMin() < 2 || Resolution.GetMax() > 9)
				return Fail(FString::Printf(TEXT("the lattice's LatticeResolution %s is outside 2 .. 9 per axis: remove it and AddLattice a new one"), *Resolution.ToString()));
		}
		else if (bTexture)
		{
			NewSize = Resized(VectorProperty(TEXT("UnscaledPatchCoverage")));
			if (!IsLength(NewSize.X) || !IsLength(NewSize.Y))
				return Fail(FString::Printf(TEXT("Size %s must be above 0 on X and Y"), *Text(NewSize)));
		}
		else
		{
			return Unsupported(TEXT("Size"));
		}
	}
	if (Radius >= 0.0 && !(bPatch || bWeight))
		return Unsupported(TEXT("Radius"));
	if (Radius >= 0.0 && !IsLength(Radius))
		return Fail(FString::Printf(TEXT("Radius %g must be above 0"), Radius));
	if (Width >= 0.0 && !bSpline)
		return Unsupported(TEXT("Width"));
	UObject* HeightEntry = nullptr;
	if (Strength >= 0.0)
	{
		if (bWeight && Strength > 1.0)
			return Fail(FString::Printf(TEXT("Strength %g: a weight's value is 0 .. 1"), Strength));
		if (bTexture)
		{
			const FObjectProperty* Entry = CastField<FObjectProperty>(Modifier->GetClass()->FindPropertyByName(TEXT("HeightChannel")));
			HeightEntry = Entry ? Entry->GetObjectPropertyValue_InContainer(Modifier) : nullptr;
			if (!HeightEntry)
				return Fail(TEXT("the texture patch has no height channel: set HeightChannel first"));
		}
		else if (!bNoise && !bWeight)
		{
			return Unsupported(TEXT("Strength"));
		}
	}
	if (Falloff >= 0.0)
	{
		if (bNoise && Falloff > 1.0)
			return Fail(FString::Printf(TEXT("Falloff %g: a noise's falloff is 0 .. 1"), Falloff));
		if (bSpline && Falloff < 0.1)
			return Fail(TEXT("a spline's Falloff is at least 0.1 cm"));
		if (!bNoise && !bPatch && !bWeight && !bSpline)
			return Unsupported(TEXT("Falloff"));
	}

	FTerrainTransaction Transaction(LOCTEXT("ModifyModifier", "Modify Mesh Terrain Modifier"), { Modifier });
	if (bSize)
	{
		if (ShapeMesh)
		{
			AActor* Owner = Modifier->GetOwner();
			const bool bFlat = IsFlatShape(Shape);
			Owner->Modify();
			Owner->SetActorScale3D(FVector(NewSize.X / ShapeMade.X, NewSize.Y / ShapeMade.Y, bFlat ? Owner->GetActorScale3D().Z : NewSize.Z / ShapeMade.Z));
			Owner->PostEditMove(/*bFinished*/ true);
		}
		else if (bNoise || bRemesh)
		{
			if (!SetProperty(Modifier, TEXT("UnscaledCoverage"), SizeText(NewSize)))
				return FModifierOpResult();
		}
		else if (Lattice)
		{
			// The points scale with the box around the lattice, so it keeps its deformation.
			const FVector Old = VectorProperty(TEXT("Extents"));
			TArray<FVector> Points = LatticePoints(Lattice);
			if (!SetProperty(Modifier, TEXT("Extents"), SizeText(NewSize)))
				return FModifierOpResult();
			// A lattice whose LatticeResolution was set directly holds the wrong number of points: start it over as a grid.
			const FStructProperty* ResolutionProperty = CastField<FStructProperty>(Modifier->GetClass()->FindPropertyByName(TEXT("LatticeResolution")));
			const FIntVector Resolution = ResolutionProperty ? *ResolutionProperty->ContainerPtrToValuePtr<FIntVector>(Modifier) : FIntVector(0);
			if (Points.Num() != Resolution.X * Resolution.Y * Resolution.Z)
			{
				ResetLattice(Lattice, NewSize, Resolution);
			}
			else
			{
				const FVector Center = Lattice->GetComponentLocation();
				const FVector Scale(Old.X > 0.0 ? NewSize.X / Old.X : 1.0, Old.Y > 0.0 ? NewSize.Y / Old.Y : 1.0, Old.Z > 0.0 ? NewSize.Z / Old.Z : 1.0);
				for (FVector& Point : Points)
					Point = Center + (Point - Center) * Scale;
				StoreLatticeWorldPoints(Lattice, Points);
			}
		}
		else if (!SetProperty(Modifier, TEXT("UnscaledPatchCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g)"), NewSize.X, NewSize.Y)))
		{
			return FModifierOpResult();
		}
	}
	if (Radius >= 0.0 && !SetNumber(Modifier, TEXT("Radius"), Radius))
		return FModifierOpResult();
	if (Width >= 0.0 && !SetNumber(Modifier, TEXT("PlateauDistance"), Width / 2.0))
		return FModifierOpResult();
	if (Strength >= 0.0)
	{
		if (bNoise && !SetNumber(Modifier, TEXT("Intensity"), Strength))
			return FModifierOpResult();
		if (bWeight)
		{
			if (!SetNumber(Modifier, TEXT("InnerValue"), Strength))
				return FModifierOpResult();
			RefreshWeightCacheKey(Modifier);
		}
		if (HeightEntry)
		{
			if (!SetNumber(HeightEntry, TEXT("EncodingScale"), Strength) || !CoverTexturePatch(Modifier, TerrainOf(Modifier), Modifier->GetComponentLocation(), Strength))
				return FModifierOpResult();
			Modifier->OnChanged(Modifier->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
		}
	}
	if (Falloff >= 0.0 && !SetNumber(Modifier, bSpline ? TEXT("FalloffDistance") : TEXT("Falloff"), Falloff))
		return FModifierOpResult();
	return Finish(Modifier, { TerrainOf(Modifier) });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Move(UE::MeshPartition::UModifierComponent* Modifier, FVector Location)
{
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	AActor* Owner = Modifier->GetOwner();
	if (!Cast<UE::MeshPartition::AModifierActor>(Owner))
		return Fail(FString::Printf(TEXT("%s's modifier lives on %s, not on a modifier actor of its own: move that actor itself"), *ModifierKind(Modifier), *Owner->GetActorNameOrLabel()));
	FTerrainTransaction Transaction(LOCTEXT("MoveModifier", "Move Mesh Terrain Modifier"), { Owner, Modifier });
	// As a viewport drag: the move notifies the modifier (PostEditMove), which rebuilds its old and new area (a lattice
	// carries its world-space points along itself).
	Owner->SetActorLocation(Location, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
	Owner->PostEditMove(/*bFinished*/ true);
	// The vertical reach the Add operations gave is measured from the location: keep it covering the ground at the new one.
	AMeshPartition* Terrain = TerrainOf(Modifier);
	if (IsKind(Modifier, TEXT("Patch")) || IsKind(Modifier, TEXT("WeightUtility")))
	{
		if (!CoverTerrainHeights(Modifier, Terrain, Modifier->GetComponentLocation()))
			return FModifierOpResult();
	}
	else if (IsKind(Modifier, TEXT("TexturePatch")))
	{
		const FObjectProperty* Entry = CastField<FObjectProperty>(Modifier->GetClass()->FindPropertyByName(TEXT("HeightChannel")));
		UObject* HeightEntry = Entry ? Entry->GetObjectPropertyValue_InContainer(Modifier) : nullptr;
		const FNumericProperty* Scale = HeightEntry ? CastField<FNumericProperty>(HeightEntry->GetClass()->FindPropertyByName(TEXT("EncodingScale"))) : nullptr;
		const double Height = Scale ? Scale->GetFloatingPointPropertyValue(Scale->ContainerPtrToValuePtr<void>(HeightEntry)) : 0.0;
		if (!CoverTexturePatch(Modifier, Terrain, Modifier->GetComponentLocation(), Height))
			return FModifierOpResult();
	}
	return Finish(Modifier, { Terrain });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::SetSplinePoints(UE::MeshPartition::UModifierComponent* Modifier, const TArray<FVector>& Points, int32 Closed)
{
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	if (Closed < -1 || Closed > 1)
		return Fail(FString::Printf(TEXT("Closed %d: use 1 (closed), 0 (open) or -1 (keep)"), Closed));
	const USplineComponent* Current = SplineOf(Modifier, /*bCreate*/ false);
	const bool bClosed = Closed < 0 ? Current && Current->IsClosedLoop() : Closed == 1;
	if (!CheckPoints(Points, bClosed))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("SetSplinePoints", "Edit Mesh Terrain Spline"), { Modifier->GetOwner(), Modifier });
	if (!ApplySplinePoints(Modifier, Points, bClosed))
		return FModifierOpResult();
	return Finish(Modifier, { TerrainOf(Modifier) });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::SetOrder(UE::MeshPartition::UModifierComponent* Modifier, double Priority, const FString& PriorityLayer)
{
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	if (!FMath::IsFinite(Priority) || FMath::Abs(Priority) > 1.0e9)
		return Fail(FString::Printf(TEXT("Priority %g is outside -1e9 .. 1e9"), Priority));
	AMeshPartition* Terrain = TerrainOf(Modifier);
	const FString Layer = PriorityLayer.TrimStartAndEnd();
	FName LayerName = Modifier->GetType();
	if (!Layer.IsEmpty())
	{
		const UMeshPartitionDefinition* Definition = Terrain ? Terrain->GetMeshPartitionDefinition() : nullptr;
		const TArray<FName> Layers = Definition ? Definition->GetModifierTypePriorities() : TArray<FName>();
		const FName* Found = Layers.FindByPredicate([&Layer](const FName& Name) { return Name.ToString().Equals(Layer, ESearchCase::IgnoreCase); });
		if (!Found && !Layer.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			TArray<FString> Names;
			for (const FName& Name : Layers)
				Names.Add(Name.ToString());
			return Fail(FString::Printf(TEXT("'%s' is not a priority layer of %s's definition; its layers: %s (\"None\": after all of them)"), *Layer,
				Terrain ? *Terrain->GetActorNameOrLabel() : TEXT("the terrain"), Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", "))));
		}
		LayerName = Found ? *Found : NAME_None;
	}
	FTerrainTransaction Transaction(LOCTEXT("SetModifierOrder", "Reorder Mesh Terrain Modifier"), { Modifier });
	if (LayerName != Modifier->GetType() && !SetProperty(Modifier, TEXT("Type"), FString::Printf(TEXT("(Name=\"%s\")"), *LayerName.ToString())))
		return FModifierOpResult();
	if (!SetNumber(Modifier, TEXT("Priority"), Priority))
		return FModifierOpResult();
	return Finish(Modifier, { Terrain });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Enable(UE::MeshPartition::UModifierComponent* Modifier)
{
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("EnableModifier", "Enable Mesh Terrain Modifier"), { Modifier });
	if (Modifier->GetIsDisabledFlag() && !SetProperty(Modifier, TEXT("bIsDisabled"), TEXT("False")))
		return FModifierOpResult();
	// "Disabled only in Editor" (transient) keeps it out of the editor build too.
	if (Modifier->IsTemporarilyDisabledInEditor() && !SetProperty(Modifier, TEXT("bIsTemporarilyDisabledInEditor"), TEXT("False")))
		return FModifierOpResult();
	return Finish(Modifier, { TerrainOf(Modifier) });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Disable(UE::MeshPartition::UModifierComponent* Modifier)
{
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("DisableModifier", "Disable Mesh Terrain Modifier"), { Modifier });
	if (!Modifier->GetIsDisabledFlag() && !SetProperty(Modifier, TEXT("bIsDisabled"), TEXT("True")))
		return FModifierOpResult();
	return Finish(Modifier, { TerrainOf(Modifier) });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Bind(UE::MeshPartition::UModifierComponent* Modifier, UE::MeshPartition::AMeshPartition* Terrain)
{
	if (!CheckModifier(Modifier, /*bNeedsTerrain*/ false) || !CheckTerrain(Terrain))
		return FModifierOpResult();
	const FBox Bounds = Modifier->ComputeCombinedBounds();
	if (Bounds.IsValid && !CheckFootprint(Terrain, FBox2D(FVector2D(Bounds.Min), FVector2D(Bounds.Max))))
		return FModifierOpResult();
	AMeshPartition* Previous = TerrainOf(Modifier);
	FTerrainTransaction Transaction(LOCTEXT("BindModifier", "Bind Mesh Terrain Modifier"), { Modifier });
	if (Previous != Terrain)
	{
		Modifier->Modify();
		Modifier->BP_SetAffectedMegaMesh(Terrain);
		Modifier->OnChanged(Modifier->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
		// The terrain it left rebuilds the area without it.
		if (UMeshPartitionEditorComponent* Component = EditorComponent(Previous))
			Component->ForceRebuildAllSections(UE::MeshPartition::EChangeType::StateChange);
	}
	return Finish(Modifier, { Terrain, Previous });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Remove(UE::MeshPartition::UModifierComponent* Modifier)
{
	if (!CheckModifier(Modifier, /*bNeedsTerrain*/ false, /*bOrphanBase*/ true))
		return FModifierOpResult();
	// A deleted terrain the modifier still points at counts as none (it may not be garbage collected yet).
	AMeshPartition* Terrain = TerrainOf(Modifier);
	if (!IsValid(Terrain))
		Terrain = nullptr;
	AActor* Owner = Modifier->GetOwner();
	const FString Label = Owner->GetActorNameOrLabel();
	const FString Kind = ModifierKind(Modifier);
	FTerrainTransaction Transaction(LOCTEXT("RemoveModifier", "Remove Mesh Terrain Modifier"));
	// A modifier actor holding only this modifier (what the palette and these operations place), or a base mesh's own actor
	// (the base mesh is its root: what Create made) left behind by a deleted terrain, goes as a whole; a modifier component on
	// any other actor (the terrain itself, a Blueprint, a second modifier) goes alone, with the shape mesh AddShape gave it.
	TArray<UModifierComponent*> Siblings;
	Owner->GetComponents(Siblings);
	if ((Cast<UE::MeshPartition::AModifierActor>(Owner) || (Modifier->IsBase() && Owner->GetRootComponent() == Modifier)) && Siblings.Num() == 1)
	{
		if (!Owner->GetWorld()->EditorDestroyActor(Owner, /*bShouldModifyLevel*/ true))
			return Fail(FString::Printf(TEXT("could not delete %s"), *Label));
	}
	else
	{
		Owner->Modify();
		if (UDynamicMeshComponent* Mesh = ShapeComponent(Modifier))
		{
			Mesh->Modify();
			Mesh->DestroyComponent();
		}
		Modifier->Modify();
		Modifier->DestroyComponent();
	}
	if (Terrain && !IsValid(Terrain))
		return Fail(FString::Printf(TEXT("removing %s deleted its terrain"), *Label));
	FModifierOpResult Out = Finish(nullptr, { Terrain });
	if (!Out.State.IsEmpty())
	{
		Out.Actor = Label;
		Out.Kind = Kind;
	}
	return Out;
}

#undef LOCTEXT_NAMESPACE
