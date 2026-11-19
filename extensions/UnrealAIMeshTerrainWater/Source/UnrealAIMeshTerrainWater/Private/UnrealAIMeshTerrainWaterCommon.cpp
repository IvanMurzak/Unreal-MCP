// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainWaterCommon.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlPlay.h"

#include "Algo/AnyOf.h"
#include "AssetCompilingManager.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Interfaces/IPluginManager.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionEditorSubsystem.h"
#include "MeshPartitionMeshData.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionModifierComponentDesc.h"
#include "MeshPartitionPreviewComponents.h"
#include "MeshPartitionPreviewSection.h"
#include "MeshPartitionTransformerPipeline.h"
#include "MeshPartitionWaterModifier.h"

#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyTypes.h"
#include "WaterSplineComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIMeshTerrainWater, Log, All);

namespace UnrealAIMeshTerrainWater
{
	using APreviewSection = UE::MeshPartition::APreviewSection;

	double BuildTimeoutForTests = 0.0;

	namespace
	{
		FString NameOf(const AActor* Actor)
		{
			return Actor ? Actor->GetActorNameOrLabel() : FString(TEXT("null"));
		}

		FString Find(const AActor* Actor)
		{
			return FString::Printf(TEXT("unreal.find(\"%s\")"), *NameOf(Actor));
		}

		bool PluginEnabled(const TCHAR* Name)
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
			return Plugin.IsValid() && Plugin->IsEnabled();
		}
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

	FString UnavailableReason()
	{
		for (const TCHAR* Name : RequiredPlugins)
		{
			if (!PluginEnabled(Name))
			{
				return FString::Printf(TEXT("the %s plugin is not enabled in this project: enable it (Edit > Plugins) and restart the editor, or call meshTerrainWater | call EnableMeshTerrainWater()"), Name);
			}
		}
		if (!GEditor || !UE::MeshPartition::UMeshPartitionEditorSubsystem::Get())
			return TEXT("the Mesh Partition editor subsystem is not running (water on Mesh Terrain needs the Unreal Editor)");
		if (UnrealMcpRql::PlayWorld())
			return TEXT("a Play In Editor session is running: stop it first (editor | call StopPlay()): water on Mesh Terrain edits the editor world");
		if (!UnrealMcpRql::EditorWorld())
			return TEXT("no editor world is open");
		return FString();
	}

	bool CheckAvailable()
	{
		const FString Reason = UnavailableReason();
		return Reason.IsEmpty() || UnrealMcpRql::Failed<bool>(Reason);
	}

	bool CheckTerrain(const AMeshPartition* Terrain)
	{
		if (!CheckAvailable())
			return false;
		if (!Terrain)
		{
			TArray<FString> Labels;
			for (TActorIterator<AMeshPartition> It(UnrealMcpRql::EditorWorld()); It; ++It)
				Labels.Add(It->GetActorLabel());
			return UnrealMcpRql::Failed<bool>(Labels.IsEmpty()
				? FString(TEXT("Terrain is null and the level has no Mesh Terrain: create one first (meshTerrainCreate | call Create(...), the Mesh Terrain extension, or Mesh Terrain Mode)"))
				: FString::Printf(TEXT("Terrain is null: pass unreal.find(\"<label>\") of a Mesh Terrain (the level has %s)"), *FString::Join(Labels, TEXT(", "))));
		}
		if (!IsValid(Terrain) || Terrain->GetWorld() != UnrealMcpRql::EditorWorld())
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s is not a Mesh Terrain of the open level"), *NameOf(Terrain)));
		if (!EditorComponent(Terrain))
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no Mesh Partition editor component (not registered yet)"), *NameOf(Terrain)));
		LoadTerrainActors(Terrain);
		return true;
	}

	namespace
	{
		/** The unloaded World Partition actors of @p World that @p Match accepts (by their descriptor). */
		TArray<FGuid> UnloadedActors(UWorld* World, TFunctionRef<bool(const FWorldPartitionActorDesc&)> Match)
		{
			TArray<FGuid> Out;
			UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
			if (!WorldPartition)
				return Out;
			FWorldPartitionHelpers::ForEachActorDescInstance(WorldPartition, [&](const FWorldPartitionActorDescInstance* Instance)
			{
				const FWorldPartitionActorDesc* Desc = Instance ? Instance->GetActorDesc() : nullptr;
				if (Desc && !Instance->IsLoaded() && Match(*Desc))
					Out.Add(Instance->GetGuid());
				return true;
			});
			return Out;
		}
	}

	void LoadTerrainActors(const AMeshPartition* Terrain)
	{
		UWorld* World = Terrain ? Terrain->GetWorld() : nullptr;
		const FGuid TerrainGuid = Terrain ? Terrain->GetActorGuid() : FGuid();
		const TArray<FGuid> Unloaded = UnloadedActors(World, [&TerrainGuid](const FWorldPartitionActorDesc& Desc)
		{
			for (const TUniquePtr<FWorldPartitionComponentDesc>& Component : Desc.GetComponentDescs())
			{
				const UClass* Class = Component.IsValid() ? Component->GetComponentNativeClass() : nullptr;
				// The modifier components describe themselves with MeshPartition's FWorldPartitionModifierComponentDesc.
				if (Class && Class->IsChildOf(UModifierComponent::StaticClass())
					&& static_cast<const UE::MeshPartition::FWorldPartitionModifierComponentDesc*>(Component.Get())->MegaMeshGuid == TerrainGuid)
				{
					return true;
				}
			}
			return false;
		});
		// The core loader skips the actors deleted in this session (their descriptors stay until the level is saved).
		if (!Unloaded.IsEmpty() && UnrealMcpRql::LoadWorldPartitionActors(Unloaded))
		{
			if (UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
				Component->UpdateModifierList();
		}
	}

	void LoadActorsOf(UClass* Class)
	{
		if (!UnavailableReason().IsEmpty())
			return;
		const TArray<FGuid> Unloaded = UnloadedActors(UnrealMcpRql::EditorWorld(), [Class](const FWorldPartitionActorDesc& Desc)
		{
			const UClass* Native = Desc.GetActorNativeClass();
			return Native && Native->IsChildOf(Class);
		});
		if (!Unloaded.IsEmpty())
			UnrealMcpRql::LoadWorldPartitionActors(Unloaded);
	}

	TArray<AWaterBody*> WaterBodies(bool bLoad)
	{
		UWorld* World = UnrealMcpRql::EditorWorld();
		if (bLoad)
			LoadActorsOf(AWaterBody::StaticClass());
		TArray<AWaterBody*> Out;
		if (!World)
			return Out;
		for (TActorIterator<AWaterBody> It(World); It; ++It)
		{
			if (IsValid(*It))
				Out.Add(*It);
		}
		return Out;
	}

	UMeshPartitionEditorComponent* EditorComponent(const AMeshPartition* Terrain)
	{
		return Terrain ? Cast<UMeshPartitionEditorComponent>(Terrain->GetMeshPartitionComponent()) : nullptr;
	}

	namespace
	{
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
	}

	FBox TerrainBounds(const AMeshPartition* Terrain)
	{
		FBox Bounds(ForceInit);
		for (const APreviewSection* Section : PreviewSections(Terrain))
			Bounds += Section->GetPreviewMeshBounds();
		if (Bounds.IsValid)
			return Bounds;
		// Not built yet: its base meshes' extent.
		if (const UMeshPartitionEditorComponent* Component = EditorComponent(Terrain))
		{
			for (const UModifierComponent* Base : Component->GetModifiersFiltered([](const UModifierComponent* M) { return IsValid(M) && M->IsBase(); }))
				Bounds += Base->ComputeCombinedBounds();
		}
		return Bounds;
	}

	FBox TerrainsBounds(UWorld* World)
	{
		FBox Bounds(ForceInit);
		if (!World)
			return Bounds;
		for (TActorIterator<AMeshPartition> It(World); It; ++It)
		{
			if (IsValid(*It))
				Bounds += TerrainBounds(*It);
		}
		return Bounds;
	}

	TArray<UPrimitiveComponent*> TerrainColliders(const AMeshPartition* Terrain)
	{
		TArray<UPrimitiveComponent*> Out, Components;
		for (const APreviewSection* Section : PreviewSections(Terrain))
		{
			Section->GetComponents(Components);
			for (UPrimitiveComponent* Component : Components)
			{
				if (Component && Component->IsCollisionEnabled())
					Out.Add(Component);
			}
		}
		return Out;
	}

	TOptional<double> GroundHeight(TConstArrayView<UPrimitiveComponent*> Colliders, double X, double Y)
	{
		TOptional<double> Best;
		const FVector Start(X, Y, 1.0e7), End(X, Y, -1.0e7);
		const FCollisionQueryParams Params(SCENE_QUERY_STAT(UnrealAIMeshTerrainWaterGround), /*bTraceComplex*/ true);
		for (UPrimitiveComponent* Component : Colliders)
		{
			// Only a component whose box is over the point can be hit.
			const FBoxSphereBounds& Box = Component->Bounds;
			if (FMath::Abs(X - Box.Origin.X) > Box.BoxExtent.X || FMath::Abs(Y - Box.Origin.Y) > Box.BoxExtent.Y)
				continue;
			FHitResult Hit;
			if (Component->LineTraceComponent(Hit, Start, End, Params) && (!Best.IsSet() || Hit.ImpactPoint.Z > Best.GetValue()))
				Best = Hit.ImpactPoint.Z;
		}
		return Best;
	}

	TOptional<double> GroundHeight(const AMeshPartition* Terrain, double X, double Y)
	{
		return GroundHeight(TerrainColliders(Terrain), X, Y);
	}

	FString FBuildState::Describe() const
	{
		if (bNoBase)
			return TEXT("built (the terrain has no base mesh)");
		TArray<FString> Parts;
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

		bool IsPreviewShown(const APreviewSection* Section)
		{
			const TSharedPtr<const UE::MeshPartition::FMeshData> Preview = Section->GetPreviewMesh();
			return Preview.IsValid() && Preview->TriangleCount() > 0;
		}

		/** Whether @p Terrain's preview transformer pipeline turns a built section into a static mesh (the default one does). */
		bool MakesStaticMeshes(const AMeshPartition* Terrain)
		{
			const UE::MeshPartition::UMeshPartitionDefinition* Definition = Terrain ? Terrain->GetMeshPartitionDefinition() : nullptr;
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

	namespace
	{
		/** BuildState with @p bStaticMeshes (MakesStaticMeshes) given: WaitForBuild reads it once, not every round. */
		FBuildState BuildState(const AMeshPartition* Terrain, bool bStaticMeshes)
		{
			FBuildState State;
			const UMeshPartitionEditorComponent* Component = EditorComponent(Terrain);
			if (!Component)
				return State;
			State.bNoBase = !Algo::AnyOf(Component->GetModifiersFiltered([](const UModifierComponent* Modifier) { return IsValid(Modifier); }),
				[](const UModifierComponent* Modifier) { return Modifier->IsBase() && !Modifier->IsDisabled(); });
			State.bBuilding = Component->IsAnyPreviewSectionBuildActive();
			for (const APreviewSection* Section : PreviewSections(Terrain))
			{
				++State.Sections;
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
					State.Finishing += bStaticMeshes && !Section->IsTransformerPipelinedPaused() && !bStaticMesh ? 1 : 0;
				}
			}
			return State;
		}
	}

	FBuildState BuildState(const AMeshPartition* Terrain)
	{
		return BuildState(Terrain, MakesStaticMeshes(Terrain));
	}

	bool WaitForBuild(AMeshPartition* Terrain, double TimeoutSeconds, FString& OutPending)
	{
		OutPending.Reset();
		UMeshPartitionEditorComponent* Component = EditorComponent(Terrain);
		if (!Component)
		{
			OutPending = FString::Printf(TEXT("%s has no Mesh Partition editor component"), *NameOf(Terrain));
			return false;
		}
		const double Start = FPlatformTime::Seconds();
		const double Deadline = Start + (BuildTimeoutForTests > 0.0 ? BuildTimeoutForTests : FMath::Clamp(TimeoutSeconds, MinTimeoutSeconds, MaxTimeoutSeconds));
		const bool bStaticMeshes = MakesStaticMeshes(Terrain);
		// The loaded water modifiers bound to the terrain: one spawned or rebound in this call may join the terrain's
		// modifier list only after a later UpdateModifierList; until every one is listed the build is not done.
		TArray<const UModifierComponent*> Expected;
		for (TActorIterator<AWaterBody> It(Terrain->GetWorld()); It; ++It)
		{
			for (const UWaterModifier* Modifier : WaterModifiers(*It))
			{
				if (Modifier->GetAffectedMeshPartition() == Terrain && !Modifier->GetIsDisabledFlag())
					Expected.Add(Modifier);
			}
		}
		const auto Unregistered = [Component, &Expected]()
		{
			const TArray<UModifierComponent*> Listed = Component->GetModifiersFiltered([](const UModifierComponent* Modifier) { return IsValid(Modifier); });
			return Expected.ContainsByPredicate([&Listed](const UModifierComponent* Modifier) { return !Listed.Contains(Modifier); });
		};
		FBuildState State;
		bool bUnregistered = true;
		int32 Rounds = 0;
		while (true)
		{
			// Queued modifier changes start their section builds and finished builds are applied (a modifier spawned or
			// changed in this call first joins the terrain's modifier list).
			if (bUnregistered)
				Component->UpdateModifierList();
			Component->Update();
			// Collision cooking and the transformer pipeline finish through game-thread tasks.
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FAssetCompilingManager::Get().ProcessAsyncTasks(true);
			State = BuildState(Terrain, bStaticMeshes);
			bUnregistered = Unregistered();
			++Rounds;
			if (State.IsReady() && !bUnregistered)
				break;
			if (FPlatformTime::Seconds() > Deadline)
			{
				OutPending = FString::Printf(TEXT("%s still building after %.0f s (%s%s)"), *NameOf(Terrain), FPlatformTime::Seconds() - Start, *State.Describe(),
					bUnregistered ? TEXT(", a water modifier not registered with it yet") : TEXT(""));
				return false;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		UE_LOG(LogUnrealAIMeshTerrainWater, Log, TEXT("%s built in %.2f s (%d rounds)"), *NameOf(Terrain), FPlatformTime::Seconds() - Start, Rounds);
		return true;
	}

	FString WaterType(const AWaterBody* Body)
	{
		const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		if (!Component)
			return TEXT("Unknown");
		switch (Component->GetWaterBodyType())
		{
		case EWaterBodyType::River:      return TEXT("River");
		case EWaterBodyType::Lake:       return TEXT("Lake");
		case EWaterBodyType::Ocean:      return TEXT("Ocean");
		case EWaterBodyType::Transition: return TEXT("Custom");
		default:                         return TEXT("Unknown");
		}
	}

	UClass* ModifierClassFor(const AWaterBody* Body)
	{
		const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		if (!Component)
			return nullptr;
		// The Lake / River modifier headers are private to MeshPartitionWater: their classes by script path (the module is
		// loaded: this one depends on it).
		static UClass* const River = FindObject<UClass>(nullptr, TEXT("/Script/MeshPartitionWater.RiverModifier"));
		static UClass* const Lake = FindObject<UClass>(nullptr, TEXT("/Script/MeshPartitionWater.LakeModifier"));
		switch (Component->GetWaterBodyType())
		{
		case EWaterBodyType::River: return River;
		case EWaterBodyType::Lake:  return Lake;
		default:                    return nullptr;
		}
	}

	TArray<UWaterModifier*> WaterModifiers(const AWaterBody* Body)
	{
		TArray<UWaterModifier*> Out;
		if (Body)
			Body->GetComponents(Out);
		Out.RemoveAll([](const UWaterModifier* Modifier) { return !IsValid(Modifier); });
		return Out;
	}

	UWaterModifier* MatchingModifier(const AWaterBody* Body)
	{
		const UClass* Class = ModifierClassFor(Body);
		if (!Class)
			return nullptr;
		for (UWaterModifier* Modifier : WaterModifiers(Body))
		{
			if (Modifier->GetClass() == Class)
				return Modifier;
		}
		return nullptr;
	}

	AMeshPartition* TerrainOf(const AWaterBody* Body)
	{
		const UWaterModifier* Modifier = MatchingModifier(Body);
		AMeshPartition* Terrain = Modifier ? Modifier->GetAffectedMeshPartition() : nullptr;
		return IsValid(Terrain) ? Terrain : nullptr;
	}

	TArray<AWaterBody*> BodiesOf(const AMeshPartition* Terrain, bool bLoad)
	{
		TArray<AWaterBody*> Out = WaterBodies(bLoad);
		Out.RemoveAll([Terrain](const AWaterBody* Body) { return !Terrain || TerrainOf(Body) != Terrain; });
		return Out;
	}

	AActor* FindFirst(UWorld* World, UClass* Class)
	{
		if (!World)
			return nullptr;
		for (TActorIterator<AActor> It(World, Class); It; ++It)
		{
			if (IsValid(*It))
				return *It;
		}
		return nullptr;
	}

	bool IsWaterMaterial(const UMaterialInterface* Material)
	{
		const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
		return Base && Base->MaterialDomain == MD_Surface && Base->GetUsageByFlag(MATUSAGE_Water);
	}

	namespace
	{
		/** Whether @p Body's spline (or actor) lies over some Mesh Terrain of its world (XY). */
		bool OverMeshTerrain(const AWaterBody* Body)
		{
			FBox2D Area(ForceInit);
			Area += FVector2D(Body->GetActorLocation());
			if (const UWaterSplineComponent* Spline = Body->GetWaterSpline())
			{
				for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
					Area += FVector2D(Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World));
			}
			for (TActorIterator<AMeshPartition> It(Body->GetWorld()); It; ++It)
			{
				const FBox Box = IsValid(*It) ? TerrainBounds(*It) : FBox(ForceInit);
				if (Box.IsValid && FBox2D(FVector2D(Box.Min), FVector2D(Box.Max)).Intersect(Area))
					return true;
			}
			return false;
		}
	}

	FString Problem(const AWaterBody* Body)
	{
		const UWaterBodyComponent* Component = Body ? Body->GetWaterBodyComponent() : nullptr;
		if (!Component)
			return TEXT("it has no water body component");
		const FString Unavailable = UnavailableReason();
		if (!Unavailable.IsEmpty())
			return Unavailable;
		const FString Me = Find(Body);
		const TArray<UWaterModifier*> Modifiers = WaterModifiers(Body);
		const UWaterModifier* Modifier = MatchingModifier(Body);
		const AMeshPartition* Terrain = Modifier && IsValid(Modifier->GetAffectedMeshPartition()) ? Modifier->GetAffectedMeshPartition() : nullptr;
		if (Component->GetWaterBodyType() == EWaterBodyType::Ocean)
		{
			if (Modifiers.Num() > 0)
			{
				return FString::Printf(TEXT("it carries %d Mesh Partition water modifier(s) (%s): an ocean needs none on a Mesh Terrain (its spline only crops the water under the ground), and the experimental OceanModifier can override the ground other modifiers shaped under it (lakes, rivers, hills): meshTerrainWater | call Detach(%s)"),
					Modifiers.Num(), *Modifiers[0]->GetClass()->GetName(), *Me);
			}
		}
		else if (const UClass* Wanted = ModifierClassFor(Body); Wanted && (!Modifiers.IsEmpty() || OverMeshTerrain(Body)))
		{
			if (!Modifier)
				return FString::Printf(TEXT("it does not shape any Mesh Terrain (no %s): meshTerrainWater | call Attach(%s, unreal.find(\"<terrain>\"))"), *Wanted->GetName(), *Me);
			if (Modifiers.Num() > 1)
				return FString::Printf(TEXT("it carries %d water modifiers, one is enough (a second shapes the terrain twice or undoes the first): meshTerrainWater | call Attach(%s, unreal.find(\"<terrain>\")) keeps one"), Modifiers.Num(), *Me);
			if (!Terrain)
				return FString::Printf(TEXT("its %s is bound to no Mesh Terrain: meshTerrainWater | call Attach(%s, unreal.find(\"<terrain>\"))"), *Wanted->GetName(), *Me);
			if (Modifier->GetIsDisabledFlag())
				return FString::Printf(TEXT("its %s is disabled, so it does not shape %s: meshTerrainWater | call Attach(%s, %s) enables it"), *Wanted->GetName(), *NameOf(Terrain), *Me, *Find(Terrain));
			if (!Component->bAffectsLandscape)
				return FString::Printf(TEXT("its Affects Landscape (bAffectsLandscape) is off, so its %s does not shape %s: meshTerrainWater | call Attach(%s, %s) turns it on"), *Wanted->GetName(), *NameOf(Terrain), *Me, *Find(Terrain));
		}
		const UMaterialInterface* Material = Component->GetWaterMaterial();
		if (!Material)
			return FString::Printf(TEXT("no water material, so it is invisible: meshTerrainWater | call SetMaterial(%s) gives it the Water defaults"), *Me);
		if (!IsWaterMaterial(Material))
			return FString::Printf(TEXT("its WaterMaterial %s is not a water material: meshTerrainWater | call SetMaterial(%s)"), *Material->GetName(), *Me);
		if (Component->GetWaterBodyType() != EWaterBodyType::Transition && !Component->GetWaterZone())
			return TEXT("no water zone renders it: meshTerrainWater | call SetZone()");
		if (Terrain)
		{
			const FBuildState State = BuildState(Terrain);
			if (!State.IsReady())
				return FString::Printf(TEXT("%s is still building (%s): meshTerrainWater | call WaitForBuild(%s)"), *NameOf(Terrain), *State.Describe(), *Me);
		}
		if (!FApp::CanEverRender())
			return TEXT("this editor does not render (-nullrhi): the water mesh is built in a rendering editor");
		// UWaterBodyInfoMeshComponent is private to the Water plugin: find the info mesh by its component name.
		bool bMesh = false;
		TArray<UStaticMeshComponent*> Meshes;
		Body->GetComponents(Meshes);
		for (const UStaticMeshComponent* Mesh : Meshes)
			bMesh |= Mesh->GetFName() == TEXT("WaterInfoMeshComponent") && Mesh->GetStaticMesh() != nullptr;
		if (!bMesh && Component->ShouldGenerateWaterMeshTile())
			return FString::Printf(TEXT("its water mesh is not built: meshTerrainWater | call SetMaterial(%s, \"%s\") rebuilds it"), *Me, *Material->GetPathName());
		if (Material->IsCompiling())
			return TEXT("its material's shaders are still compiling: it shows when they finish (read MeshTerrainWater again)");
		return FString();
	}
}
