// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainCommon.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlPlay.h"

#include "Algo/AnyOf.h"
#include "AssetCompilingManager.h"
#include "Components/StaticMeshComponent.h"
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

	int32 LoadTerrainActors(const AMeshPartition* Terrain)
	{
		TArray<FGuid> Unloaded;
		ForEachTerrainActorDesc(Terrain, [&Unloaded](const FWorldPartitionActorDescInstance& Instance)
		{
			if (!Instance.IsLoaded())
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
		State.bNoBase = !Algo::AnyOf(Current, [](const UModifierComponent* Modifier) { return Modifier->IsBase() && !Modifier->GetIsDisabledFlag(); });
		// A modifier that loaded (World Partition) or was spawned joins the build when the editor component lists it.
		ForEachTerrainActorDesc(Terrain, [Terrain, &State, &Current](const FWorldPartitionActorDescInstance& Instance)
		{
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
		const double Deadline = Start + FMath::Clamp(TimeoutSeconds, 1.0, 600.0);
		FBuildState State;
		int32 Rounds = 0;
		while (true)
		{
			// Queued modifier changes start their section builds and finished builds are applied to their sections here.
			if (State.Unregistered > 0)
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

	TArray<FRegionMesh> BuildRegion(const AMeshPartition* Terrain, const FBox& Region, double TimeoutSeconds)
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

	FSurfaceSample SampleSurface(const AMeshPartition* Terrain, double X, double Y, bool bChannels, const TArray<FRegionMesh>* Evaluated)
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
		// The builder keeps only triangles wholly inside the region: retry wider for a coarse terrain (quads over ~2.5 m).
		const auto HitIn = [&Out, &Start, &End](const TArray<FRegionMesh>& Parts)
		{
			for (const FRegionMesh& Part : Parts)
			{
				if (!Part.Spatial.IsValid())
					continue;
				const FVector LocalStart = Part.Transform.InverseTransformPosition(Start);
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
				return true;
			}
			return false;
		};
		const auto Around = [Terrain, X, Y, &Bounds](double Half)
		{
			return BuildRegion(Terrain, FBox(FVector(X - Half, Y - Half, Bounds.Min.Z - 1000.0), FVector(X + Half, Y + Half, Bounds.Max.Z + 1000.0)));
		};
		if (!HitIn(Evaluated ? *Evaluated : Around(250.0)))
			HitIn(Around(2500.0));
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
			return UnrealMcpRql::Failed<AMeshPartition*>(FString::Printf(TEXT("Mesh Terrain creation failed: %s"), *Generator.GetErrorText().ToString()));
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
		TArray<AActor*> Actors;
		for (UModifierComponent* Modifier : Modifiers(Terrain, /*bBases*/ true))
		{
			if (AActor* Owner = Modifier->GetOwner(); Owner && Owner != Terrain)
				Actors.AddUnique(Owner);
		}
		Actors.Add(Terrain);
		const FScopedTransaction Transaction(NSLOCTEXT("UnrealAIMeshTerrain", "DestroyTerrain", "Delete Mesh Terrain"));
		for (AActor* Actor : Actors)
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
	}
}
