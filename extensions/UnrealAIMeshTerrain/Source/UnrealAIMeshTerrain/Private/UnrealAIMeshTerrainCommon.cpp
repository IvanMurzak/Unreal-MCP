// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainCommon.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlPlay.h"

#include "ActorFactories/ActorFactory.h"
#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"
#include "AssetCompilingManager.h"
#include "Components/StaticMeshComponent.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "PhysicsEngine/BodySetup.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Interfaces/IPluginManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionBlueprintLibrary.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "Async/TaskGraphInterfaces.h"

#include "MeshPartition.h"
#include "MeshPartitionMeshBuilder.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionEditorSubsystem.h"
#include "MeshPartitionMeshData.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionModifierActor.h"
#include "MeshPartitionModifierComponentDesc.h"
#include "MeshPartitionPreviewComponents.h"
#include "MeshPartitionPreviewSection.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionTransformerPipeline.h"
#include "MeshPartitionRectangleGenerator.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIMeshTerrain, Log, All);

namespace UnrealAIMeshTerrain
{
	namespace
	{
		FString TerrainName(const AActor* Actor)
		{
			return Actor ? Actor->GetActorNameOrLabel() : FString(TEXT("null"));
		}

	}

	FString UnavailableReason()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MeshPartition"));
		if (!Plugin.IsValid() || !Plugin->IsEnabled())
			return TEXT("Mesh Terrain is not enabled in this project: enable the MeshTerrainMode plugin (Edit > Plugins, it enables MeshPartition) and restart the editor, or call meshTerrain | call EnableMeshTerrain()");
		if (!GEditor || !UE::MeshPartition::UMeshPartitionEditorSubsystem::Get())
			return TEXT("the Mesh Partition editor subsystem is not running (Mesh Terrain needs the Unreal Editor)");
		if (UnrealMcpRql::PlayWorld())
			return TEXT("a Play In Editor session is running: stop it first (Mesh Terrain edits the editor world)");
		if (!UnrealMcpRql::EditorWorld())
			return TEXT("no editor world is open");
		return FString();
	}

	bool CheckAvailable()
	{
		const FString Reason = UnavailableReason();
		if (Reason.IsEmpty())
			return true;
		UnrealMcpRql::Fail(Reason);
		return false;
	}

	bool CheckTimeout(double TimeoutSeconds)
	{
		return (TimeoutSeconds >= MinTimeoutSeconds && TimeoutSeconds <= MaxTimeoutSeconds)
			|| UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("TimeoutSeconds %g is outside %g .. %g"), TimeoutSeconds, MinTimeoutSeconds, MaxTimeoutSeconds));
	}

	double SecondsSince(double Start)
	{
		return FMath::RoundToDouble((FPlatformTime::Seconds() - Start) * 100.0) / 100.0;
	}

	TArray<AMeshPartition*> Terrains()
	{
		TArray<AMeshPartition*> Out;
		if (UWorld* World = UnrealMcpRql::EditorWorld())
		{
			for (TActorIterator<AMeshPartition> It(World); It; ++It)
			{
				if (IsValid(*It))
					Out.Add(*It);
			}
		}
		return Out;
	}

	AMeshPartition* FindTerrain(const FString& LabelOrName)
	{
		TArray<FString> Names;
		for (AMeshPartition* Terrain : Terrains())
		{
			if (Terrain->GetActorLabel().Equals(LabelOrName, ESearchCase::IgnoreCase) || Terrain->GetName().Equals(LabelOrName, ESearchCase::IgnoreCase))
				return Terrain;
			Names.Add(Terrain->GetActorLabel());
		}
		UnrealMcpRql::Fail(Names.IsEmpty()
			? FString::Printf(TEXT("no Mesh Terrain '%s': the level has no Mesh Terrain yet"), *LabelOrName)
			: FString::Printf(TEXT("no Mesh Terrain '%s'; the level has %s"), *LabelOrName, *FString::Join(Names, TEXT(", "))));
		return nullptr;
	}

	bool CheckTerrain(const AMeshPartition* Terrain)
	{
		if (!CheckAvailable())
			return false;
		if (!Terrain)
			return UnrealMcpRql::Failed<bool>(TEXT("Terrain is null: pass unreal.find(\"<label>\") of a Mesh Terrain (meshTerrains | select Label)"));
		if (!IsValid(Terrain) || Terrain->GetWorld() != UnrealMcpRql::EditorWorld())
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not a Mesh Terrain of the open level"), *TerrainName(Terrain)));
		if (!EditorComponent(Terrain))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no Mesh Partition editor component (not registered yet)"), *TerrainName(Terrain)));
		LoadTerrainActors(Terrain);
		return true;
	}

	namespace
	{
		/** The World Partition actor descriptors of the actors that hold a modifier (or base mesh) targeting @p Terrain. */
		void ForEachTerrainActorDesc(const AMeshPartition* Terrain, TFunctionRef<void(const FWorldPartitionActorDescInstance&)> Func)
		{
			UWorld* World = Terrain ? Terrain->GetWorld() : nullptr;
			UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
			if (!WorldPartition)
				return;
			const FGuid TerrainGuid = Terrain->GetActorGuid();
			FWorldPartitionHelpers::ForEachActorDescInstance(WorldPartition, [&](const FWorldPartitionActorDescInstance* Instance)
			{
				const FWorldPartitionActorDesc* Desc = Instance ? Instance->GetActorDesc() : nullptr;
				if (!Desc)
					return true;
				for (const TUniquePtr<FWorldPartitionComponentDesc>& Component : Desc->GetComponentDescs())
				{
					const UClass* Class = Component.IsValid() ? Component->GetComponentNativeClass() : nullptr;
					// The modifier components describe themselves with MeshPartition's FWorldPartitionModifierComponentDesc.
					if (Class && Class->IsChildOf(UModifierComponent::StaticClass())
						&& static_cast<const UE::MeshPartition::FWorldPartitionModifierComponentDesc*>(Component.Get())->MegaMeshGuid == TerrainGuid)
					{
						Func(*Instance);
						break;
					}
				}
				return true;
			});
		}
	}

	namespace
	{
		/**
		 * Whether the actor of @p Instance was deleted in this session: its descriptor stays until the level is saved, its
		 * package stays in memory, dirty (deleted on save) and without the actor. Unloaded actors' packages are not in memory
		 * or not dirty (World Partition does not unload dirty actors).
		 */
		bool IsDeletedActor(const FWorldPartitionActorDescInstance& Instance)
		{
			if (Instance.IsLoaded())
				return false;
			const UPackage* Package = FindObject<UPackage>(nullptr, *Instance.GetActorPackage().ToString());
			return Package && Package->IsDirty();
		}
	}

	int32 LoadTerrainActors(const AMeshPartition* Terrain)
	{
		TArray<FGuid> Unloaded;
		ForEachTerrainActorDesc(Terrain, [&Unloaded](const FWorldPartitionActorDescInstance& Instance)
		{
			if (!Instance.IsLoaded() && !IsDeletedActor(Instance))
				Unloaded.Add(Instance.GetGuid());
		});
		if (!Unloaded.IsEmpty())
		{
			// UWorldPartitionBlueprintLibrary::LoadActors (what Python's load_actors calls) keeps them in its per-world actor-list
			// loader adapter and releases it with the world; the class is MinimalAPI, so it is called through reflection.
			if (UFunction* LoadActors = UWorldPartitionBlueprintLibrary::StaticClass()->FindFunctionByName(TEXT("LoadActors")))
			{
				struct { TArray<FGuid> InActorsToLoad; } Params{ Unloaded };
				UWorldPartitionBlueprintLibrary::StaticClass()->GetDefaultObject()->ProcessEvent(LoadActors, &Params);
			}
			if (UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
				Component->UpdateModifierList();
		}
		return Unloaded.Num();
	}

	UMeshPartitionEditorComponent* EditorComponent(const AMeshPartition* Terrain)
	{
		return Terrain ? Cast<UMeshPartitionEditorComponent>(Terrain->GetMeshPartitionComponent()) : nullptr;
	}

	TArray<UModifierComponent*> Modifiers(const AMeshPartition* Terrain, bool bBases)
	{
		TArray<UModifierComponent*> Out;
		if (const UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
		{
			Out = Component->GetModifiersFiltered([bBases](const UModifierComponent* Modifier)
			{
				return IsValid(Modifier) && (bBases || !Modifier->IsBase());
			});
			// Build order: the definition's priority layer, then the sub-priority (higher is applied later).
			Out.StableSort([Component](const UModifierComponent& A, const UModifierComponent& B)
			{
				const uint32 LayerA = Component->GetModifierLayerIndex(A.GetType());
				const uint32 LayerB = Component->GetModifierLayerIndex(B.GetType());
				return LayerA != LayerB ? LayerA < LayerB : A.GetPriority() < B.GetPriority();
			});
		}
		return Out;
	}

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

	FString UniqueLabel(UWorld* World, const FString& Base)
	{
		TSet<FString> Used;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel().StartsWith(Base))
				Used.Add(It->GetActorLabel());
		}
		FString Label = Base;
		for (int32 Index = 2; Used.Contains(Label); ++Index)
			Label = FString::Printf(TEXT("%s%d"), *Base, Index);
		return Label;
	}

	UModifierComponent* SpawnModifier(AMeshPartition* Terrain, UClass* Class, const FVector& Location, const FRotator& Rotation, const FString& Label,
		AMeshPartition*& OutPrevious)
	{
		OutPrevious = nullptr;
		if (!Class)
			return UnrealMcpRql::Failed<UModifierComponent*>(TEXT("this engine's MeshPartitionEditor has no such modifier class"));
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

	AMeshPartition* TerrainOf(const UModifierComponent* Modifier)
	{
		return Modifier ? Modifier->GetAffectedMeshPartition() : nullptr;
	}

	FString ModifierKind(const UModifierComponent* Modifier)
	{
		if (!Modifier)
			return FString();
		FString Name = Modifier->GetClass()->GetName();
		Name.RemoveFromEnd(TEXT("Modifier"));
		return Name;
	}

	TArray<APreviewSection*> PreviewSections(const AMeshPartition* Terrain)
	{
		TArray<APreviewSection*> Out;
		if (const UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
		{
			Component->ForAllPreviewSections([&Out](APreviewSection* Section)
			{
				if (IsValid(Section))
					Out.Add(Section);
				return true;
			});
		}
		return Out;
	}

	namespace
	{
		/** Whether @p Section still shows its intermediate full-resolution preview mesh (its final static mesh is not made yet). */
		bool IsPreviewShown(const APreviewSection* Section)
		{
			const TSharedPtr<const UE::MeshPartition::FMeshData> Preview = Section->GetPreviewMesh();
			return Preview.IsValid() && Preview->TriangleCount() > 0;
		}
	}

	int32 RenderedTriangles(const APreviewSection* Section)
	{
		if (!Section)
			return 0;
		if (IsPreviewShown(Section))
			return Section->GetPreviewMesh()->TriangleCount();
		int32 Triangles = 0;
		for (const UStaticMeshComponent* Mesh : Section->GetMeshComponents())
		{
			const UStaticMesh* Static = Mesh ? Mesh->GetStaticMesh() : nullptr;
			if (Static && !Static->IsCompiling() && Static->GetRenderData() && Static->GetRenderData()->LODResources.Num() > 0)
				Triangles += Static->GetRenderData()->LODResources[0].GetNumTriangles();
		}
		return Triangles;
	}

	FBox BuiltBounds(const AMeshPartition* Terrain)
	{
		FBox Box(ForceInit);
		for (const APreviewSection* Section : PreviewSections(Terrain))
		{
			// World space (the section actor sits on the terrain's transform, its mesh does not move with it).
			const FBox Built = Section->GetPreviewMeshBounds();
			if (Built.IsValid)
				Box += Built;
		}
		return Box;
	}

	FBox BaseBounds(const AMeshPartition* Terrain)
	{
		FBox Box(ForceInit);
		if (const UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
		{
			for (const UModifierComponent* Base : Component->GetModifiersFiltered([](const UModifierComponent* Modifier) { return IsValid(Modifier) && Modifier->IsBase(); }))
				Box += Base->ComputeCombinedBounds();
		}
		return Box;
	}

	void GroundExtremes(const AMeshPartition* Terrain, const FBox& Base, TOptional<FVector>& OutHighest, TOptional<FVector>& OutLowest)
	{
		OutHighest.Reset();
		OutLowest.Reset();
		if (!Base.IsValid)
			return;
		// The highest and lowest vertices of the sections' collision, a few candidates each way: a vertex can lie under an
		// overhang (the underside of a Union shape past the terrain's edge), which no vertical trace reaches, so the first
		// candidate the trace from above lands on is the ground's extreme. Within the terrain's own area: the collision also
		// has a skirt a few cm outside and below its edges.
		constexpr int32 Candidates = 64;
		auto ByZ = [](bool bHighest) { return [bHighest](const FVector& A, const FVector& B) { return bHighest ? A.Z > B.Z : A.Z < B.Z; }; };
		TArray<FVector> High, Low;
		auto Keep = [&ByZ](TArray<FVector>& Best, const FVector& Point, bool bHighest)
		{
			Best.Add(Point);
			if (Best.Num() >= Candidates * 2)
			{
				Best.Sort(ByZ(bHighest));
				Best.SetNum(Candidates);
			}
		};
		const FBox2D Area(FVector2D(Base.Min) - FVector2D(1.0), FVector2D(Base.Max) + FVector2D(1.0));
		TArray<UPrimitiveComponent*> Components;
		for (const APreviewSection* Section : PreviewSections(Terrain))
		{
			Section->GetComponents(Components);
			for (UPrimitiveComponent* Component : Components)
			{
				const UBodySetup* Body = Component ? Component->GetBodySetup() : nullptr;
				if (!Body || !Component->IsCollisionEnabled())
					continue;
				const FTransform& Transform = Component->GetComponentTransform();
				for (const Chaos::FTriangleMeshImplicitObjectPtr& Mesh : Body->TriMeshGeometries)
				{
					if (!Mesh)
						continue;
					const auto& Points = Mesh->Particles();
					for (int32 Index = 0; Index < static_cast<int32>(Points.Size()); ++Index)
					{
						const FVector World = Transform.TransformPosition(FVector(Points.GetX(Index)));
						if (!Area.IsInside(FVector2D(World)))
							continue;
						Keep(High, World, true);
						Keep(Low, World, false);
					}
				}
			}
		}
		// The trace at the vertex itself, then 5 cm beside it (diagonally too): a pit's floor corner sits right under the rim of
		// one or two vertical walls.
		auto FirstOnGround = [Terrain, &ByZ](TArray<FVector>& Best, bool bHighest, TOptional<FVector>& Out)
		{
			Best.Sort(ByZ(bHighest));
			for (const FVector& Point : Best)
			{
				for (const FVector2D Offset : { FVector2D(0.0), FVector2D(5.0, 0.0), FVector2D(-5.0, 0.0), FVector2D(0.0, 5.0), FVector2D(0.0, -5.0),
					FVector2D(5.0, 5.0), FVector2D(5.0, -5.0), FVector2D(-5.0, 5.0), FVector2D(-5.0, -5.0) })
				{
					const FSurfaceSample Ground = SampleSurface(Terrain, Point.X + Offset.X, Point.Y + Offset.Y, /*bChannels*/ false);
					if (Ground.bInside && FMath::IsNearlyEqual(Ground.Height, Point.Z, 2.0))
					{
						Out = FVector(Point.X + Offset.X, Point.Y + Offset.Y, Ground.Height);
						return;
					}
				}
			}
		};
		FirstOnGround(High, true, OutHighest);
		FirstOnGround(Low, false, OutLowest);
	}

	FString FBuildState::Describe() const
	{
		TArray<FString> Parts;
		if (Unregistered > 0)
			Parts.Add(FString::Printf(TEXT("%d loaded modifier(s) not registered with the terrain yet"), Unregistered));
		if (bNoBase)
			return Parts.IsEmpty() ? FString(TEXT("built (the terrain has no base mesh)")) : FString::Join(Parts, TEXT(", "));
		if (bBuilding)
			Parts.Add(TEXT("sections are being built"));
		else if (Sections == 0)
			Parts.Add(TEXT("no section is built yet"));
		if (Cooking > 0)
			Parts.Add(FString::Printf(TEXT("%d section collision(s) cooking"), Cooking));
		if (Finishing > 0)
			Parts.Add(FString::Printf(TEXT("%d section(s) waiting for their final mesh"), Finishing));
		if (CompilingMeshes > 0)
			Parts.Add(FString::Printf(TEXT("%d section mesh(es) compiling"), CompilingMeshes));
		return Parts.IsEmpty() ? FString(TEXT("built")) : FString::Join(Parts, TEXT(", "));
	}

	namespace
	{
		bool HasCollision(const UPrimitiveComponent* Component)
		{
			const FBodyInstance* Body = Component->GetBodyInstance();
			return Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision || (Body && Body->IsValidBodyInstance());
		}

		/** Whether @p Terrain's preview transformer pipeline turns a built section into a static mesh (the default one does). */
		bool MakesStaticMeshes(const AMeshPartition* Terrain)
		{
			const UMeshPartitionDefinition* Definition = Terrain ? Terrain->GetMeshPartitionDefinition() : nullptr;
			const UE::MeshPartition::UTransformerPipeline* Pipeline = Definition ? Definition->GetPreviewSectionBuildVariant().TransformerPipeline.Get() : nullptr;
			if (!Pipeline)
				return false;
			for (const TInstancedStruct<UE::MeshPartition::FTransformer>& Transformer : Pipeline->GetTransformers())
			{
				for (const UStruct* Struct = Transformer.GetScriptStruct(); Struct; Struct = Struct->GetSuperStruct())
				{
					if (Struct->GetFName() == TEXT("StaticMeshTransformer"))
						return true;
				}
			}
			return false;
		}
	}

	FBuildState BuildState(const AMeshPartition* Terrain)
	{
		FBuildState State;
		const UMeshPartitionEditorComponent* Component = EditorComponent(Terrain);
		if (!Component)
			return State;
		const TSet<UModifierComponent*> Current(Component->GetModifiersFiltered([](const UModifierComponent* Modifier) { return IsValid(Modifier); }));
		State.bNoBase = !Algo::AnyOf(Current, [](const UModifierComponent* Modifier) { return Modifier->IsBase() && !Modifier->IsDisabled(); });
		// A modifier that loaded (World Partition) or was spawned joins the build when the editor component lists it.
		ForEachTerrainActorDesc(Terrain, [Terrain, &State, &Current](const FWorldPartitionActorDescInstance& Instance)
		{
			if (IsDeletedActor(Instance))
				return;
			++State.Actors;
			if (const AActor* Actor = Instance.IsLoaded() ? Instance.GetActor(/*bEvenIfPendingKill*/ false) : nullptr; IsValid(Actor))
			{
				++State.LoadedActors;
				TArray<UModifierComponent*> Components;
				Actor->GetComponents(Components);
				for (UModifierComponent* Modifier : Components)
					State.Unregistered += IsValid(Modifier) && TerrainOf(Modifier) == Terrain && !Current.Contains(Modifier) ? 1 : 0;
			}
		});
		State.bBuilding = Component->IsAnyPreviewSectionBuildActive();
		const bool bStaticMeshes = MakesStaticMeshes(Terrain);
		for (const APreviewSection* Section : PreviewSections(Terrain))
		{
			++State.Sections;
			State.Triangles += RenderedTriangles(Section);
			const bool bPreviewShown = IsPreviewShown(Section);
			bool bStaticMesh = false;
			for (const UStaticMeshComponent* Mesh : Section->GetMeshComponents())
			{
				const UStaticMesh* Static = Mesh ? Mesh->GetStaticMesh() : nullptr;
				if (!Static)
					continue;
				bStaticMesh = true;
				if (Static->IsCompiling())
					++State.CompilingMeshes;
				else if (!bPreviewShown)
					State.Cooking += HasCollision(Mesh) ? 0 : 1;
			}
			if (bPreviewShown)
			{
				TArray<UE::MeshPartition::UPreviewMeshComponent*> PreviewComponents;
				Section->GetComponents(PreviewComponents);
				for (const UE::MeshPartition::UPreviewMeshComponent* PreviewComponent : PreviewComponents)
					State.Cooking += HasCollision(PreviewComponent) ? 0 : 1;
				// The pipeline (paused while an interactive tool edits the section) still has to make the static mesh.
				State.Finishing += bStaticMeshes && !Section->IsTransformerPipelinedPaused() && !bStaticMesh ? 1 : 0;
			}
		}
		return State;
	}

	bool WaitForBuild(AMeshPartition* Terrain, double TimeoutSeconds)
	{
		UMeshPartitionEditorComponent* Component = EditorComponent(Terrain);
		if (!Component)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no Mesh Partition editor component"), *TerrainName(Terrain)));
		const double Start = FPlatformTime::Seconds();
		const double Deadline = Start + FMath::Clamp(TimeoutSeconds, MinTimeoutSeconds, MaxTimeoutSeconds);
		FBuildState State;
		int32 Rounds = 0;
		while (true)
		{
			// Queued modifier changes start their section builds and finished builds are applied to their sections here.
			// (first round too: a terrain or modifier spawned in this call may not be listed yet)
			if (Rounds == 0 || State.Unregistered > 0)
				Component->UpdateModifierList();
			Component->Update();
			// Collision cooking and the transformer pipeline finish through game-thread tasks.
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FAssetCompilingManager::Get().ProcessAsyncTasks(true);
			State = BuildState(Terrain);
			++Rounds;
			if (State.IsReady())
				break;
			if (FPlatformTime::Seconds() > Deadline)
			{
				return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s did not finish building within %.0f s (%s); call meshTerrain | call WaitForBuild(...) again with a longer timeout"),
					*TerrainName(Terrain), TimeoutSeconds, *State.Describe()));
			}
			FPlatformProcess::Sleep(0.01f);
		}
		UE_LOG(LogUnrealAIMeshTerrain, Log, TEXT("%s built in %.2f s (%d rounds): %d sections, %d triangles"),
			*TerrainName(Terrain), FPlatformTime::Seconds() - Start, Rounds, State.Sections, State.Triangles);
		return true;
	}

	bool Rebuild(AMeshPartition* Terrain, double TimeoutSeconds)
	{
		UMeshPartitionEditorComponent* Component = EditorComponent(Terrain);
		if (!Component)
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no Mesh Partition editor component"), *TerrainName(Terrain)));
		Component->UpdateModifierList();
		Component->ForceRebuildAllSections(UE::MeshPartition::EChangeType::TransientChange);
		return WaitForBuild(Terrain, TimeoutSeconds);
	}

	FTerrainTransaction::FTerrainTransaction(const FText& Description, TArrayView<UObject* const> Objects)
		: Transaction(Description)
	{
		for (UObject* Object : Objects)
			Modify(Object);
	}

	void FTerrainTransaction::Modify(UObject* Object)
	{
		if (Object)
		{
			Object->SetFlags(RF_Transactional);
			Object->Modify();
		}
	}

	TArray<FRegionMesh> BuildRegion(const AMeshPartition* Terrain, const FBox& Region, double TimeoutSeconds, bool bFailOnTimeout)
	{
		TArray<FRegionMesh> Out;
		const UMeshPartitionEditorComponent* Component = EditorComponent(Terrain);
		const UMeshPartitionDefinition* Definition = Component ? Component->GetMegaMeshDefinition() : nullptr;
		if (!Definition || !Region.IsValid)
			return Out;
		UE::MeshPartition::FBuilderSettings Settings;
		Settings.BuildType = UE::MeshPartition::EBuildType::Request;
		Settings.Transform = Terrain->GetActorTransform();
		Settings.ModifiersToProcess = Modifiers(Terrain, /*bBases*/ true);
		Settings.TypePriorities = Definition->GetModifierTypePriorities();
		Settings.FilterBounds.Add(UE::Geometry::FOrientedBox3d(Region));
		Settings.MaxSectionComplexity = Definition->GetPreviewSectionBuildVariant().MaxSectionComplexity;
		Settings.bBuildSpatial = true;
		Settings.bCacheResult = true;
		Settings.bAllowDDCRead = true;
		TArray<UE::MeshPartition::FBuildTaskHandle> Handles = UE::MeshPartition::Build::LaunchBuilds(Settings);
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!UE::MeshPartition::Build::AreAllTasksComplete(Handles))
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				for (UE::MeshPartition::FBuildTaskHandle& Handle : Handles)
					Handle.Cancel();
				if (bFailOnTimeout)
					UnrealMcpRql::Fail(FString::Printf(TEXT("evaluating %s's modifiers took longer than %.0f s"), *TerrainName(Terrain), TimeoutSeconds));
				return {};
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.005f);
		}
		for (const UE::MeshPartition::FBuildTaskHandle& Handle : Handles)
		{
			const TSharedPtr<UE::MeshPartition::FBuildTask>& Task = Handle.GetTask();
			if (Task.IsValid() && Task->GetMesh().IsValid())
				Out.Add({ Task->GetMesh(), Task->GetSpatial(), Task->GetTransform() });
		}
		return Out;
	}

	FSurfaceSample SampleSurface(const AMeshPartition* Terrain, double X, double Y, bool bChannels, TArray<FRegionMesh>* Evaluated)
	{
		FSurfaceSample Out;
		const FBox Bounds = BuiltBounds(Terrain);
		if (!Bounds.IsValid)
			return Out;
		const FVector Start(X, Y, Bounds.Max.Z + 1000.0);
		const FVector End(X, Y, Bounds.Min.Z - 1000.0);
		// The rendered sections' collision: exactly what physics, line traces and placed actors meet.
		const FCollisionQueryParams Params(SCENE_QUERY_STAT(MeshTerrainSurface), /*bTraceComplex*/ true);
		TArray<UPrimitiveComponent*> Components;
		for (const APreviewSection* Section : PreviewSections(Terrain))
		{
			Section->GetComponents(Components);
			for (UPrimitiveComponent* Component : Components)
			{
				const FBox Box = Component->Bounds.GetBox();
				FHitResult Hit;
				if (Component->IsVisible() && Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision
					&& X >= Box.Min.X && X <= Box.Max.X && Y >= Box.Min.Y && Y <= Box.Max.Y
					&& Component->LineTraceComponent(Hit, Start, End, Params)
					&& (!Out.bInside || Hit.ImpactPoint.Z > Out.Height))
				{
					Out.bInside = true;
					Out.Height = Hit.ImpactPoint.Z;
					Out.Normal = Hit.ImpactNormal;
				}
			}
		}
		if (!Out.bInside || !bChannels)
			return Out;
		// The paint channels are not in the collision: evaluate the modifier stack around the point (cached per region).
		// The builder keeps only triangles wholly inside the region: retry wider for coarse geometry (quads over ~2.5 m, a
		// Boolean shape's triangles of 100+ m), and keep what was evaluated in Evaluated for the caller's next points.
		const auto HitIn = [&Out, &Start, &End](const TArray<FRegionMesh>& Parts)
		{
			for (const FRegionMesh& Part : Parts)
			{
				if (!Part.Spatial.IsValid())
					continue;
				const FVector LocalStart = Part.Transform.InverseTransformPosition(Start);
				// Parts cached for other points: skip the ones that are not under this one before tracing them (the ray is
				// vertical in an unrotated terrain's space).
				const UE::Geometry::FAxisAlignedBox3d Box = Part.Spatial->GetBoundingBox();
				if (Part.Transform.GetRotation().IsIdentity(1e-6) && (LocalStart.X < Box.Min.X || LocalStart.X > Box.Max.X || LocalStart.Y < Box.Min.Y || LocalStart.Y > Box.Max.Y))
					continue;
				const FVector LocalDirection = Part.Transform.InverseTransformVector(End - Start).GetSafeNormal();
				double Distance = 0.0;
				int32 Triangle = INDEX_NONE;
				FVector3d Bary;
				if (!Part.Spatial->FindNearestHitTriangle(FRay3d(LocalStart, LocalDirection), Distance, Triangle, Bary))
					continue;
				const UE::Geometry::FIndex3i Corners = Part.Mesh->GetTriangle(Triangle);
				for (const FName& Channel : Part.Mesh->GetWeightLayerNames())
				{
					Out.Channels.Add(Channel, Part.Mesh->GetWeightLayerValue(Channel, Corners.A) * Bary.X + Part.Mesh->GetWeightLayerValue(Channel, Corners.B) * Bary.Y
						+ Part.Mesh->GetWeightLayerValue(Channel, Corners.C) * Bary.Z);
				}
				Out.ModelHeight = Part.Transform.TransformPosition(LocalStart + LocalDirection * Distance).Z;
				// The evaluated face's normal: a trace that lands on an edge of the collision (a section seam, the terrain's
				// border) reports the edge's normal (45 degrees or more on flat ground), not the ground's.
				// (the inverse transpose of the terrain's transform: right under a non-uniform scale too)
				const FVector Face = Part.Transform.GetRotation().RotateVector(Part.Mesh->GetTriNormal(Triangle) * Part.Transform.GetSafeScaleReciprocal(Part.Transform.GetScale3D())).GetSafeNormal();
				if (!Face.IsNearlyZero())
					Out.Normal = Face;
				return true;
			}
			return false;
		};
		TArray<FRegionMesh> Local;
		TArray<FRegionMesh>& Parts = Evaluated ? *Evaluated : Local;
		if (HitIn(Parts))
			return Out;
		// Each step builds whole sections (the region only filters them): stop once a region covers the whole terrain, and
		// treat a timeout as a miss (no Channels). Keep only the region that hit (or the widest, on a miss): the smaller ones
		// lie inside it; at most MaxCachedRegions of them for scattered points.
		constexpr int32 MaxCachedRegions = 32;
		TArray<FRegionMesh> Around;
		for (const double Half : { 250.0, 2500.0, 25000.0, 250000.0 })
		{
			const FBox Region(FVector(X - Half, Y - Half, Bounds.Min.Z - 1000.0), FVector(X + Half, Y + Half, Bounds.Max.Z + 1000.0));
			Around = BuildRegion(Terrain, Region, DefaultBuildTimeoutSeconds, /*bFailOnTimeout*/ false);
			if (HitIn(Around) || (Region.Min.X <= Bounds.Min.X && Region.Min.Y <= Bounds.Min.Y && Region.Max.X >= Bounds.Max.X && Region.Max.Y >= Bounds.Max.Y))
				break;
		}
		if (Parts.Num() >= MaxCachedRegions)
			Parts.RemoveAt(0, Parts.Num() - MaxCachedRegions + 1);
		Parts.Append(MoveTemp(Around));
		return Out;
	}

	AMeshPartition* SpawnFlatTerrain(const FString& Label, const FVector& Center, double SizeX, double SizeY, int32 QuadsX, int32 QuadsY)
	{
		UWorld* World = UnrealMcpRql::EditorWorld();
		if (!World)
			return UnrealMcpRql::Failed<AMeshPartition*>(TEXT("no editor world is open"));
		UE::MeshPartition::FMegaMeshRectangleGeneratorParams Params;
		Params.MeshSize = FVector2D(SizeX, SizeY);
		Params.MeshResolution = FInt32Point(QuadsX, QuadsY);
		// The Create tool's automatic sections: at most 500 x 500 quads per section. ComputeSectionResolution returns the
		// number of sections on X and Y (not quads per section, as its comment says).
		Params.SectionsResolution = UE::MeshPartition::FRectangleGeneratorUtils::ComputeSectionResolution(Params.MeshResolution, 500 * 500 * 2);
		if (Params.SectionsResolution.X <= 0 || Params.SectionsResolution.Y <= 0)
			return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("%d x %d quads is not a terrain resolution"), QuadsX, QuadsY));
		Params.bSaveAndUnload = false;
		Params.LocationVolumesResolution = FInt32Point(0, 0);
		UE::MeshPartition::FMegaMeshRectangleGenerator Generator(Params, *World, FTransform(Center));
		if (!Generator.Generate(nullptr) || !Generator.GetMegaMesh())
		{
			DestroyTerrain(Generator.GetMegaMesh()); // whatever a failed generation spawned (null-safe)
			return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("Mesh Terrain creation failed: %s"), *Generator.GetErrorText().ToString()));
		}
		AMeshPartition* Terrain = Generator.GetMegaMesh();
		if (!Label.IsEmpty())
			Terrain->SetActorLabel(Label);
		return Terrain;
	}

	void DestroyTerrain(AMeshPartition* Terrain)
	{
		if (!IsValid(Terrain))
			return;
		UWorld* World = Terrain->GetWorld();
		// The terrain's own actors go whole: its base meshes (attached to it) and modifier actors whose modifiers all shape
		// it. A modifier on any other actor (a Blueprint, an actor that also shapes another terrain) goes alone, as Remove does.
		TArray<AActor*> Actors;
		TArray<UModifierComponent*> Components;
		for (UModifierComponent* Modifier : Modifiers(Terrain, /*bBases*/ true))
		{
			AActor* Owner = Modifier->GetOwner();
			if (!Owner || Owner == Terrain)
				continue;
			TArray<UModifierComponent*> Siblings;
			Owner->GetComponents(Siblings);
			const bool bTerrainActor = (Modifier->IsBase() || Cast<UE::MeshPartition::AModifierActor>(Owner) || Owner->GetAttachParentActor() == Terrain)
				&& Algo::AllOf(Siblings, [Terrain](const UModifierComponent* Sibling) { return TerrainOf(Sibling) == Terrain; });
			if (bTerrainActor)
				Actors.AddUnique(Owner);
			else
				Components.Add(Modifier);
		}
		Actors.Add(Terrain);
		const FScopedTransaction Transaction(NSLOCTEXT("UnrealAIMeshTerrain", "DestroyTerrain", "Delete Mesh Terrain"));
		for (UModifierComponent* Modifier : Components)
		{
			Modifier->GetOwner()->Modify();
			Modifier->Modify();
			Modifier->DestroyComponent();
		}
		for (AActor* Actor : Actors)
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
	}
}
