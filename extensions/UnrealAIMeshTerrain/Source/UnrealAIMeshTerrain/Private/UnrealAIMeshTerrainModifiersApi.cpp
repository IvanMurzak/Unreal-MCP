// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainModifiersApi.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "IUnrealMcpRqlModule.h"

#include "ActorFactories/ActorFactory.h"
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
	/** The component tag a shape's mesh component carries ("MeshTerrainShape:Box"), so Edit can rebuild it at a new size. */
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

	/** The channel names of @p Terrain's definition, for messages. */
	FString ChannelList(const AMeshPartition* Terrain)
	{
		const UMeshPartitionDefinition* Definition = Terrain ? Terrain->GetMeshPartitionDefinition() : nullptr;
		TArray<FString> Names;
		if (Definition)
		{
			for (const FName& Name : Definition->GetChannelNames())
				Names.Add(Name.ToString());
		}
		return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
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

	/** The double property @p Name of @p Object (0 when missing). */
	double GetNumber(const UObject* Object, const FName Name)
	{
		const FNumericProperty* Property = Object ? CastField<FNumericProperty>(Object->GetClass()->FindPropertyByName(Name)) : nullptr;
		if (!Property)
			return 0.0;
		const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
		return Property->IsFloatingPoint() ? Property->GetFloatingPointPropertyValue(Value) : static_cast<double>(Property->GetSignedIntPropertyValue(Value));
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

	/** The guards every operation on an existing modifier runs: available, a live modifier of the editor world, not a base mesh. */
	bool CheckModifier(const UModifierComponent* Modifier)
	{
		if (!CheckAvailable())
			return false;
		if (!Modifier)
			return UnrealMcpRql::Failed<bool>(TEXT("Modifier is null: pass one from meshTerrainModifiers (meshTerrainModifiers | select $ref Modifier)"));
		if (!IsValid(Modifier) || Modifier->GetWorld() != UnrealMcpRql::EditorWorld())
			return UnrealMcpRql::Failed<bool>(TEXT("the modifier is not in the open level (deleted, or another world)"));
		if (Modifier->IsBase())
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is a base mesh (a section of the terrain itself), not a modifier: only the terrain's create / import operations make or replace those"),
				*Modifier->GetOwner()->GetActorNameOrLabel()));
		if (AMeshPartition* Terrain = TerrainOf(Modifier))
			LoadTerrainActors(Terrain);
		return true;
	}

	/**
	 * Place a modifier of @p Class the way the Modifiers palette does (UModifierActorFactory: an AModifierActor named after
	 * the class, with a billboard) at @p Location, then bind it to @p Terrain: the factory binds the nearest terrain by its
	 * own rule, not always the one under it. @p OutPrevious is the terrain it bound first (to rebuild it too).
	 */
	UModifierComponent* SpawnModifier(AMeshPartition* Terrain, UClass* Class, const FVector& Location, const FRotator& Rotation, const FString& Label, AMeshPartition*& OutPrevious)
	{
		OutPrevious = nullptr;
		UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryByClass(UE::MeshPartition::UModifierActorFactory::StaticClass()) : nullptr;
		UWorld* World = Terrain->GetWorld();
		ULevel* Level = World ? World->GetCurrentLevel() : nullptr;
		if (!Factory || !Level)
			return UnrealMcpRql::Failed<UModifierComponent*>(TEXT("the Mesh Partition modifier actor factory is not registered (is the MeshPartition plugin loaded?)"));
		AActor* Actor = Factory->CreateActor(Class, Level, FTransform(Rotation, Location));
		UModifierComponent* Modifier = Actor ? Cast<UModifierComponent>(Actor->FindComponentByClass(Class)) : nullptr;
		if (!Modifier)
		{
			if (Actor)
				World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
			return UnrealMcpRql::Failed<UModifierComponent*>(FString::Printf(TEXT("could not place a %s"), *Class->GetName()));
		}
		if (!Label.IsEmpty())
			Actor->SetActorLabel(Label);
		AMeshPartition* Bound = TerrainOf(Modifier);
		if (Bound != Terrain)
		{
			OutPrevious = Bound;
			Modifier->Modify();
			// The Blueprint setter also tells the terrains, so they gather the modifier (the plain setter does not).
			Modifier->BP_SetAffectedMegaMesh(Terrain);
		}
		return Modifier;
	}

	/** The modifier's summary plus where @p Terrain's build stands. */
	FModifierOpResult Describe(UModifierComponent* Modifier, const AMeshPartition* Terrain, double Seconds)
	{
		FModifierOpResult Out;
		Out.Modifier = Modifier;
		if (Modifier)
		{
			Out.Actor = Modifier->GetOwner() ? Modifier->GetOwner()->GetActorNameOrLabel() : FString();
			Out.Kind = ModifierKind(Modifier);
			Out.PriorityLayer = Modifier->GetType().ToString();
			Out.Priority = Modifier->GetPriority();
			Out.Disabled = Modifier->GetIsDisabledFlag();
		}
		Out.Terrain = Terrain ? Terrain->GetActorNameOrLabel() : FString();
		Out.Seconds = FMath::RoundToDouble(Seconds * 100.0) / 100.0;
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
			if (UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
				Component->UpdateModifierList();
			if (!WaitForBuild(Terrain))
				return FModifierOpResult();
		}
		return Describe(Modifier, Main, FPlatformTime::Seconds() - Start);
	}

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
	 * Replace @p Mesh with @p Shape (one of ShapeNames) of @p Size (full extents, cm) centred on the origin, the way Mesh
	 * Terrain Mode's Shapes tools generate them (GeometryScript primitives): closed solids, except the flat Rectangle and Disc
	 * (facing up, at Z 0).
	 */
	bool BuildShape(UDynamicMesh* Mesh, const FString& Shape, const FVector& Size)
	{
		Mesh->Reset();
		const FGeometryScriptPrimitiveOptions Options;
		using UPrimitives = UGeometryScriptLibrary_MeshPrimitiveFunctions;
		const EGeometryScriptPrimitiveOriginMode Center = EGeometryScriptPrimitiveOriginMode::Center;
		// Round shapes are made 100 cm across and scaled to Size (an ellipse when X != Y).
		const FTransform Unit(FQuat::Identity, FVector::ZeroVector, Size / 100.0);
		if (Shape == TEXT("Box"))
		{
			// Faces split into ~2.5 m quads (at most 16 per side): long two-triangle faces make the terrain boolean report cracks.
			const auto Steps = [](double Length) { return FMath::Clamp(FMath::RoundToInt32(Length / 250.0), 1, 16); };
			UPrimitives::AppendBox(Mesh, Options, FTransform::Identity, Size.X, Size.Y, Size.Z, Steps(Size.X), Steps(Size.Y), Steps(Size.Z), Center);
		}
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
			// Size.Z is the ring's thickness; X and Y its outer diameter.
			const double Minor = Size.Z / 2.0;
			const double Major = Size.X / 2.0 - Minor;
			if (Major <= 0.0)
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("a Torus needs Size X (outer diameter, %.0f) larger than Size Z (ring thickness, %.0f)"), Size.X, Size.Z));
			const FTransform Stretch(FQuat::Identity, FVector::ZeroVector, FVector(1.0, Size.Y / Size.X, 1.0));
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

	/** The shape's size (full extents, cm), kept in the component's bounds-free tag "MeshTerrainShapeSize:X,Y,Z". */
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
		Component->ComponentTags.RemoveAll([](const FName& Tag) { return Tag.ToString().StartsWith(TEXT("MeshTerrainShape")); });
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

	bool CheckPoints(const TArray<FVector>& Points)
	{
		if (Points.Num() < 2 || Points.Num() > 1000)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%d points: a spline needs 2 .. 1000 world points [[x, y, z], ...]"), Points.Num()));
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
			return Existing;
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
		if (USplineModifier* SplineModifier = Cast<USplineModifier>(Modifier))
			SplineModifier->UpdateSplineData();
		else if (USplineRemeshModifier* Remesh = Cast<USplineRemeshModifier>(Modifier))
			Remesh->UpdateSplineData();
		Modifier->OnChanged(Modifier->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
		return true;
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

using namespace UnrealAIMeshTerrainModifiersLocal;

TArray<FVector> UnrealAIMeshTerrain::LatticeWorldPoints(const UE::MeshPartition::UModifierComponent* Modifier)
{
	const ULatticeModifier* Lattice = Cast<ULatticeModifier>(Modifier);
	return Lattice ? LatticePoints(Lattice) : TArray<FVector>();
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddNoise(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector2D Size, double Strength, double Frequency, double Falloff, const FString& Label)
{
	if (!CheckAvailable() || !CheckTerrain(Terrain))
		return FModifierOpResult();
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
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
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Noise = SpawnModifier(Terrain, ModifierClassOf(TEXT("Noise")), Location, FRotator::ZeroRotator, Label, Previous);
	if (!Noise || !SetProperty(Noise, TEXT("UnscaledCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=50000)"), Size.X, Size.Y))
		|| !SetNumber(Noise, TEXT("Intensity"), Strength) || !SetProperty(Noise, TEXT("NoiseFrequency"), FString::Printf(TEXT("(X=%.17g,Y=%.17g)"), Frequency, Frequency))
		|| !SetNumber(Noise, TEXT("Falloff"), Falloff))
		return FModifierOpResult();
	return Finish(Noise, { Terrain, Previous });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddPatch(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, double Radius, double Falloff, const FString& Label)
{
	if (!CheckAvailable() || !CheckTerrain(Terrain))
		return FModifierOpResult();
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	if (!IsLength(Radius) || !IsLength(Falloff, /*bAllowZero*/ true))
		return Fail(FString::Printf(TEXT("Radius %g must be above 0 and Falloff %g at least 0, both at most 10000000 cm"), Radius, Falloff));
	if (!CheckFootprint(Terrain, Around(Location, Radius + Falloff, Radius + Falloff)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddPatch", "Add Mesh Terrain Patch"));
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Patch = SpawnModifier(Terrain, ModifierClassOf(TEXT("Patch")), Location, FRotator::ZeroRotator, Label, Previous);
	if (!Patch || !SetNumber(Patch, TEXT("Radius"), Radius) || !SetNumber(Patch, TEXT("Falloff"), Falloff))
		return FModifierOpResult();
	return Finish(Patch, { Terrain, Previous });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddWeight(UE::MeshPartition::AMeshPartition* Terrain, const FString& Channel, FVector Location, double Radius, double Falloff, double Value, const FString& Label)
{
	if (!CheckAvailable() || !CheckTerrain(Terrain) || !CheckChannel(Terrain, Channel, /*bAllowEmpty*/ false))
		return FModifierOpResult();
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	if (!IsLength(Radius) || !IsLength(Falloff, /*bAllowZero*/ true))
		return Fail(FString::Printf(TEXT("Radius %g must be above 0 and Falloff %g at least 0, both at most 10000000 cm"), Radius, Falloff));
	if (!FMath::IsFinite(Value) || Value < 0.0 || Value > 1.0)
		return Fail(FString::Printf(TEXT("Value %g is outside 0 .. 1"), Value));
	if (!CheckFootprint(Terrain, Around(Location, Radius + Falloff, Radius + Falloff)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddWeight", "Add Mesh Terrain Weight"));
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Weight = SpawnModifier(Terrain, ModifierClassOf(TEXT("WeightUtility")), Location, FRotator::ZeroRotator, Label, Previous);
	if (!Weight || !SetProperty(Weight, TEXT("WeightChannelName"), ChannelName(Terrain, Channel)) || !SetNumber(Weight, TEXT("Radius"), Radius)
		|| !SetNumber(Weight, TEXT("Falloff"), Falloff) || !SetNumber(Weight, TEXT("InnerValue"), Value) || !SetNumber(Weight, TEXT("OuterValue"), 0.0))
		return FModifierOpResult();
	return Finish(Weight, { Terrain, Previous });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddSpline(UE::MeshPartition::AMeshPartition* Terrain, const TArray<FVector>& Points, double Width, double Falloff, bool Closed, const FString& Channel, const FString& Label)
{
	if (!CheckAvailable() || !CheckTerrain(Terrain) || !CheckPoints(Points) || !CheckChannel(Terrain, Channel, /*bAllowEmpty*/ true))
		return FModifierOpResult();
	if (!IsLength(Width, /*bAllowZero*/ true) || !IsLength(Falloff))
		return Fail(FString::Printf(TEXT("Width %g must be at least 0 and Falloff %g above 0, both at most 10000000 cm"), Width, Falloff));
	if (!CheckFootprint(Terrain, PointsArea(Points, Width / 2.0 + Falloff)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddSpline", "Add Mesh Terrain Spline"));
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Spline = SpawnModifier(Terrain, ModifierClassOf(TEXT("Spline")), Points[0], FRotator::ZeroRotator, Label, Previous);
	if (!Spline || !SetNumber(Spline, TEXT("PlateauDistance"), Width / 2.0) || !SetNumber(Spline, TEXT("FalloffDistance"), Falloff)
		|| !SetProperty(Spline, TEXT("bMeshClosedInterior"), Closed ? TEXT("True") : TEXT("False"))
		|| !SetProperty(Spline, TEXT("WeightChannels"), Channel.IsEmpty() ? FString(TEXT("()"))
			: FString::Printf(TEXT("((WeightChannelName=(Name=\"%s\"),Value=1.000000))"), *ChannelName(Terrain, Channel)))
		|| !ApplySplinePoints(Spline, Points, Closed))
		return FModifierOpResult();
	return Finish(Spline, { Terrain, Previous });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddShape(UE::MeshPartition::AMeshPartition* Terrain, const FString& Shape, FVector Location, FVector Size, const FString& Operation, FRotator Rotation, const FString& Label)
{
	if (!CheckAvailable() || !CheckTerrain(Terrain))
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
	if (!IsLength(Size.X) || !IsLength(Size.Y) || (!bFlat && !IsLength(Size.Z)))
		return Fail(FString::Printf(TEXT("Size %s must be above 0 and at most 10000000 cm on each axis"), *Text(Size)));
	const double Reach = 0.5 * Size.Size();
	if (!CheckFootprint(Terrain, Around(Location, Reach, Reach)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddShape", "Add Mesh Terrain Shape"));
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Modifier = SpawnModifier(Terrain, ModifierClassOf(*OpName == TEXT("Project") ? TEXT("MeshProject") : TEXT("Boolean")), Location, Rotation,
		Label.IsEmpty() ? *ShapeName : Label, Previous);
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
	return Finish(Modifier, { Terrain, Previous });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddLattice(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector Size, int32 Resolution, const FString& Label)
{
	if (!CheckAvailable() || !CheckTerrain(Terrain))
		return FModifierOpResult();
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	if (!IsLength(Size.X) || !IsLength(Size.Y) || !IsLength(Size.Z))
		return Fail(FString::Printf(TEXT("Size %s must be above 0 and at most 10000000 cm on each axis"), *Text(Size)));
	if (Resolution < 2 || Resolution > 9)
		return Fail(FString::Printf(TEXT("Resolution %d is outside 2 .. 9 (control points per axis)"), Resolution));
	if (!CheckFootprint(Terrain, Around(Location, Size.X / 2.0, Size.Y / 2.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddLattice", "Add Mesh Terrain Lattice"));
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Lattice = SpawnModifier(Terrain, ModifierClassOf(TEXT("Lattice")), Location, FRotator::ZeroRotator, Label, Previous);
	if (!Lattice || !SetProperty(Lattice, TEXT("Extents"), FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=%.17g)"), Size.X, Size.Y, Size.Z))
		|| !SetProperty(Lattice, TEXT("LatticeResolution"), FString::Printf(TEXT("(X=%d,Y=%d,Z=%d)"), Resolution, Resolution, Resolution)))
		return FModifierOpResult();
	ResetLattice(Cast<ULatticeModifier>(Lattice), Size, FIntVector(Resolution));
	return Finish(Lattice, { Terrain, Previous });
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
	if (!CheckAvailable() || !CheckTerrain(Terrain))
		return FModifierOpResult();
	if (!Texture)
		return Fail(TEXT("Texture is null: pass a heightmap texture asset path (\"/Game/Heightmaps/T_Hill\")"));
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	if (!IsLength(Size.X) || !IsLength(Size.Y))
		return Fail(FString::Printf(TEXT("Size [%g, %g] must be above 0 and at most 10000000 cm on X and Y"), Size.X, Size.Y));
	if (!FMath::IsFinite(Height) || FMath::Abs(Height) > 1000000.0)
		return Fail(FString::Printf(TEXT("Height %g cm is outside -1000000 .. 1000000"), Height));
	if (Texture->VirtualTextureStreaming)
		return Fail(FString::Printf(TEXT("%s is a virtual texture: a texture patch reads plain textures only (turn Virtual Texture Streaming off)"), *Texture->GetPathName()));
	if (!CheckFootprint(Terrain, Around(Location, Size.X / 2.0, Size.Y / 2.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddTexturePatch", "Add Mesh Terrain Texture Patch"));
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Patch = SpawnModifier(Terrain, ModifierClassOf(TEXT("TexturePatch")), Location, FRotator::ZeroRotator, Label, Previous);
	if (!Patch || !SetProperty(Patch, TEXT("UnscaledPatchCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g)"), Size.X, Size.Y)))
		return FModifierOpResult();
	// The height channel is an instanced entry object (UTexturePatchHeightEntry): its texture and scale.
	const FObjectProperty* Entry = CastField<FObjectProperty>(Patch->GetClass()->FindPropertyByName(TEXT("HeightChannel")));
	UClass* EntryClass = FindObject<UClass>(nullptr, TEXT("/Script/MeshPartitionEditor.TexturePatchHeightEntry"));
	if (!Entry || !EntryClass)
		return Fail(TEXT("this engine's TexturePatchModifier has no HeightChannel entry"));
	UObject* Height_ = Entry->GetObjectPropertyValue_InContainer(Patch);
	if (!Height_)
	{
		Height_ = NewObject<UObject>(Patch, EntryClass, NAME_None, RF_Transactional);
		if (!SetProperty(Patch, TEXT("HeightChannel"), Height_->GetPathName()))
			return FModifierOpResult();
	}
	if (!SetProperty(Height_, TEXT("TextureAsset"), Texture->GetPathName()) || !SetNumber(Height_, TEXT("EncodingScale"), Height))
		return FModifierOpResult();
	Patch->OnChanged(Patch->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
	return Finish(Patch, { Terrain, Previous });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::AddRemesh(UE::MeshPartition::AMeshPartition* Terrain, FVector Location, FVector Size, double EdgeLength, const FString& Label)
{
	if (!CheckAvailable() || !CheckTerrain(Terrain))
		return FModifierOpResult();
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	if (!IsLength(Size.X) || !IsLength(Size.Y) || !IsLength(Size.Z))
		return Fail(FString::Printf(TEXT("Size %s must be above 0 and at most 10000000 cm on each axis"), *Text(Size)));
	if (!FMath::IsFinite(EdgeLength) || EdgeLength < 10.0 || EdgeLength > 100000.0)
		return Fail(FString::Printf(TEXT("EdgeLength %g is outside 10 .. 100000 cm"), EdgeLength));
	// A remesh far finer than the ground it covers makes millions of triangles: keep it within ~4M.
	const double Triangles = 2.0 * (Size.X / EdgeLength) * (Size.Y / EdgeLength);
	if (Triangles > 4000000.0)
		return Fail(FString::Printf(TEXT("a %.0f x %.0f cm remesh with %g cm edges makes about %.0f triangles: use longer edges or a smaller Size (at most 4000000)"), Size.X, Size.Y, EdgeLength, Triangles));
	if (!CheckFootprint(Terrain, Around(Location, Size.X / 2.0, Size.Y / 2.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddRemesh", "Add Mesh Terrain Remesh"));
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Remesh = SpawnModifier(Terrain, ModifierClassOf(TEXT("Remesh")), Location, FRotator::ZeroRotator, Label, Previous);
	if (!Remesh || !SetProperty(Remesh, TEXT("UnscaledCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=%.17g)"), Size.X, Size.Y, Size.Z))
		|| !SetNumber(Remesh, TEXT("TargetEdgeLength"), EdgeLength))
		return FModifierOpResult();
	return Finish(Remesh, { Terrain, Previous });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Add(UE::MeshPartition::AMeshPartition* Terrain, const FString& Kind, FVector Location, const FString& Label)
{
	if (!CheckAvailable() || !CheckTerrain(Terrain))
		return FModifierOpResult();
	UClass* Class = ModifierClassOf(Kind);
	if (!Class)
		return Fail(FString::Printf(TEXT("'%s' is not a modifier kind that can be added: use %s (base meshes come from the terrain's create / import, InstancedProjection / InstancedTexturePatch are filled by other tools)"),
			*Kind, *FString::Join(AddableKinds(), TEXT(", "))));
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	if (!CheckFootprint(Terrain, Around(Location, 1000.0, 1000.0)))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("AddModifier", "Add Mesh Terrain Modifier"));
	AMeshPartition* Previous = nullptr;
	UModifierComponent* Modifier = SpawnModifier(Terrain, Class, Location, FRotator::ZeroRotator, Label, Previous);
	if (!Modifier)
		return FModifierOpResult();
	// A spline remesh needs a spline to follow: give it the two-point one a spline modifier starts with.
	if (Cast<USplineRemeshModifier>(Modifier) && !ApplySplinePoints(Modifier, { Location, Location + FVector(2000.0, 0.0, 0.0) }, false))
		return FModifierOpResult();
	return Finish(Modifier, { Terrain, Previous });
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
	FTerrainTransaction Transaction(LOCTEXT("ModifyModifier", "Modify Mesh Terrain Modifier"), { Modifier });
	if (bSize)
	{
		FString Shape;
		if (UDynamicMeshComponent* Mesh = ShapeComponent(Modifier, &Shape))
		{
			// A shape keeps the mesh it was made with and is resized by its actor's scale, which undo restores with the
			// actor (a regenerated mesh would not reach the modifier on undo). A Capsule's caps and a Torus's ring stretch with it.
			AActor* Owner = Modifier->GetOwner();
			const FVector Made = ShapeSize(Mesh);
			const FVector Scale = Owner->GetActorScale3D();
			const bool bFlat = IsFlatShape(Shape);
			if (Made.X <= 0.0 || Made.Y <= 0.0 || (!bFlat && Made.Z <= 0.0))
				return Fail(FString::Printf(TEXT("%s's shape size is unknown (its mesh component lost its MeshTerrainShapeSize tag)"), *Owner->GetActorNameOrLabel()));
			const FVector New = Resized(FVector(Made.X * Scale.X, Made.Y * Scale.Y, bFlat ? 0.0 : Made.Z * Scale.Z));
			if (!IsLength(New.X) || !IsLength(New.Y) || (!bFlat && !IsLength(New.Z)))
				return Fail(FString::Printf(TEXT("Size %s must be above 0 on each axis"), *Text(New)));
			Owner->Modify();
			Owner->SetActorScale3D(FVector(New.X / Made.X, New.Y / Made.Y, bFlat ? Scale.Z : New.Z / Made.Z));
			Owner->PostEditMove(/*bFinished*/ true);
		}
		else if (Kind == TEXT("Noise") || Kind == TEXT("Remesh"))
		{
			const FVector New = Resized(VectorProperty(TEXT("UnscaledCoverage")));
			if (!SetProperty(Modifier, TEXT("UnscaledCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=%.17g)"), New.X, New.Y, New.Z)))
				return FModifierOpResult();
		}
		else if (ULatticeModifier* Lattice = Cast<ULatticeModifier>(Modifier))
		{
			// The points scale with the box around the lattice, so it keeps its deformation.
			const FVector Old = VectorProperty(TEXT("Extents"));
			const FVector New = Resized(Old);
			if (!IsLength(New.X) || !IsLength(New.Y) || !IsLength(New.Z))
				return Fail(FString::Printf(TEXT("Size %s must be above 0 on each axis"), *Text(New)));
			TArray<FVector> Points = LatticePoints(Lattice);
			if (!SetProperty(Modifier, TEXT("Extents"), FString::Printf(TEXT("(X=%.17g,Y=%.17g,Z=%.17g)"), New.X, New.Y, New.Z)))
				return FModifierOpResult();
			// A lattice whose LatticeResolution was set directly holds the wrong number of points: start it over as a grid.
			const FStructProperty* ResolutionProperty = CastField<FStructProperty>(Modifier->GetClass()->FindPropertyByName(TEXT("LatticeResolution")));
			const FIntVector Resolution = ResolutionProperty ? *ResolutionProperty->ContainerPtrToValuePtr<FIntVector>(Modifier) : FIntVector(0);
			if (Points.Num() != Resolution.X * Resolution.Y * Resolution.Z)
			{
				ResetLattice(Lattice, New, Resolution);
			}
			else
			{
				const FVector Center = Lattice->GetComponentLocation();
				const FVector Scale(Old.X > 0.0 ? New.X / Old.X : 1.0, Old.Y > 0.0 ? New.Y / Old.Y : 1.0, Old.Z > 0.0 ? New.Z / Old.Z : 1.0);
				for (FVector& Point : Points)
					Point = Center + (Point - Center) * Scale;
				StoreLatticeWorldPoints(Lattice, Points);
			}
		}
		else if (Kind == TEXT("TexturePatch"))
		{
			const FVector New = Resized(VectorProperty(TEXT("UnscaledPatchCoverage")));
			if (!SetProperty(Modifier, TEXT("UnscaledPatchCoverage"), FString::Printf(TEXT("(X=%.17g,Y=%.17g)"), New.X, New.Y)))
				return FModifierOpResult();
		}
		else
		{
			return Unsupported(TEXT("Size"));
		}
	}
	if (Radius >= 0.0)
	{
		if (Kind != TEXT("Patch") && Kind != TEXT("InstancedPatch") && Kind != TEXT("WeightUtility"))
			return Unsupported(TEXT("Radius"));
		if (!SetNumber(Modifier, TEXT("Radius"), Radius))
			return FModifierOpResult();
	}
	if (Width >= 0.0)
	{
		if (Kind != TEXT("Spline"))
			return Unsupported(TEXT("Width"));
		if (!SetNumber(Modifier, TEXT("PlateauDistance"), Width / 2.0))
			return FModifierOpResult();
	}
	if (Strength >= 0.0)
	{
		if (Kind == TEXT("Noise"))
		{
			if (!SetNumber(Modifier, TEXT("Intensity"), Strength))
				return FModifierOpResult();
		}
		else if (Kind == TEXT("WeightUtility"))
		{
			if (Strength > 1.0)
				return Fail(FString::Printf(TEXT("Strength %g: a weight's value is 0 .. 1"), Strength));
			if (!SetNumber(Modifier, TEXT("InnerValue"), Strength))
				return FModifierOpResult();
			RefreshWeightCacheKey(Modifier);
		}
		else if (Kind == TEXT("TexturePatch"))
		{
			const FObjectProperty* Entry = CastField<FObjectProperty>(Modifier->GetClass()->FindPropertyByName(TEXT("HeightChannel")));
			UObject* Height = Entry ? Entry->GetObjectPropertyValue_InContainer(Modifier) : nullptr;
			if (!Height || !SetNumber(Height, TEXT("EncodingScale"), Strength))
				return Height ? FModifierOpResult() : Fail(TEXT("the texture patch has no height channel"));
			Modifier->OnChanged(Modifier->ComputeBounds(), UE::MeshPartition::EChangeType::StateChange);
		}
		else
		{
			return Unsupported(TEXT("Strength"));
		}
	}
	if (Falloff >= 0.0)
	{
		if (Kind == TEXT("Noise"))
		{
			if (Falloff > 1.0)
				return Fail(FString::Printf(TEXT("Falloff %g: a noise's falloff is 0 .. 1"), Falloff));
			if (!SetNumber(Modifier, TEXT("Falloff"), Falloff))
				return FModifierOpResult();
		}
		else if (Kind == TEXT("Patch") || Kind == TEXT("InstancedPatch") || Kind == TEXT("WeightUtility"))
		{
			if (!SetNumber(Modifier, TEXT("Falloff"), Falloff))
				return FModifierOpResult();
		}
		else if (Kind == TEXT("Spline"))
		{
			if (Falloff < 0.1 || !SetNumber(Modifier, TEXT("FalloffDistance"), Falloff))
				return Falloff < 0.1 ? Fail(TEXT("a spline's Falloff is at least 0.1 cm")) : FModifierOpResult();
		}
		else
		{
			return Unsupported(TEXT("Falloff"));
		}
	}
	return Finish(Modifier, { TerrainOf(Modifier) });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::Move(UE::MeshPartition::UModifierComponent* Modifier, FVector Location)
{
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	if (!IsPoint(Location))
		return Fail(FString::Printf(TEXT("Location %s is not a world position"), *Text(Location)));
	AActor* Owner = Modifier->GetOwner();
	FTerrainTransaction Transaction(LOCTEXT("MoveModifier", "Move Mesh Terrain Modifier"), { Owner, Modifier });
	// As a viewport drag: the move notifies the modifier (PostEditMove), which rebuilds its old and new area (a lattice
	// carries its world-space points along itself).
	Owner->SetActorLocation(Location, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
	Owner->PostEditMove(/*bFinished*/ true);
	return Finish(Modifier, { TerrainOf(Modifier) });
}

FUnrealAIMeshTerrainModifierResult UUnrealAIMeshTerrainModifiersApi::SetSplinePoints(UE::MeshPartition::UModifierComponent* Modifier, const TArray<FVector>& Points, bool Closed)
{
	if (!CheckModifier(Modifier) || !CheckPoints(Points))
		return FModifierOpResult();
	FTerrainTransaction Transaction(LOCTEXT("SetSplinePoints", "Edit Mesh Terrain Spline"), { Modifier->GetOwner(), Modifier });
	if (!ApplySplinePoints(Modifier, Points, Closed))
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
	if (!CheckModifier(Modifier) || !CheckTerrain(Terrain))
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
	if (!CheckModifier(Modifier))
		return FModifierOpResult();
	AMeshPartition* Terrain = TerrainOf(Modifier);
	AActor* Owner = Modifier->GetOwner();
	UWorld* World = Owner->GetWorld();
	const FString Label = Owner->GetActorNameOrLabel();
	const FString Kind = ModifierKind(Modifier);
	FTerrainTransaction Transaction(LOCTEXT("RemoveModifier", "Remove Mesh Terrain Modifier"));
	// A modifier actor (what the palette and these operations place) goes as a whole; a modifier component someone added
	// to another actor goes alone.
	TArray<UModifierComponent*> Siblings;
	Owner->GetComponents(Siblings);
	if (Cast<UE::MeshPartition::AModifierActor>(Owner) || Siblings.Num() == 1)
	{
		if (!World->EditorDestroyActor(Owner, /*bShouldModifyLevel*/ true))
			return Fail(FString::Printf(TEXT("could not delete %s"), *Owner->GetActorNameOrLabel()));
	}
	else
	{
		Owner->Modify();
		Modifier->Modify();
		Modifier->DestroyComponent();
	}
	FModifierOpResult Out = Finish(nullptr, { Terrain });
	if (!Out.State.IsEmpty())
	{
		Out.Actor = Label;
		Out.Kind = Kind;
	}
	return Out;
}

#undef LOCTEXT_NAMESPACE
