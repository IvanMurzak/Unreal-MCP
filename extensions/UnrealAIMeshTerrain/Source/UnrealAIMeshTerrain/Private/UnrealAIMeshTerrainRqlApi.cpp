// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIMeshTerrainRqlApi.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "IUnrealMcpRqlModule.h"

#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"

#include "MeshPartition.h"

namespace UnrealAIMeshTerrainRqlApiLocal
{
	FUnrealAIMeshTerrainBuild BuildResult(const UE::MeshPartition::AMeshPartition* Terrain, double Start)
	{
		const UnrealAIMeshTerrain::FBuildState State = UnrealAIMeshTerrain::BuildState(Terrain);
		FUnrealAIMeshTerrainBuild Out;
		Out.Ready = State.IsReady();
		Out.Sections = State.Sections;
		Out.Triangles = State.Triangles;
		Out.Modifiers = UnrealAIMeshTerrain::Modifiers(Terrain).Num();
		Out.Seconds = UnrealAIMeshTerrain::SecondsSince(Start);
		Out.State = State.Describe();
		return Out;
	}

	/** The guards, then @p Build (UnrealAIMeshTerrain::Rebuild or WaitForBuild) timed, as the operations' result. */
	FUnrealAIMeshTerrainBuild RunBuild(UE::MeshPartition::AMeshPartition* Terrain, double TimeoutSeconds, bool (*Build)(UE::MeshPartition::AMeshPartition*, double))
	{
		if (!UnrealAIMeshTerrain::CheckTimeout(TimeoutSeconds) || !UnrealAIMeshTerrain::CheckTerrain(Terrain))
			return FUnrealAIMeshTerrainBuild();
		const double Start = FPlatformTime::Seconds();
		return Build(Terrain, TimeoutSeconds) ? BuildResult(Terrain, Start) : FUnrealAIMeshTerrainBuild();
	}
}

FUnrealAIMeshTerrainBuild UUnrealAIMeshTerrainRqlApi::Rebuild(UE::MeshPartition::AMeshPartition* Terrain, double TimeoutSeconds)
{
	return UnrealAIMeshTerrainRqlApiLocal::RunBuild(Terrain, TimeoutSeconds, &UnrealAIMeshTerrain::Rebuild);
}

FUnrealAIMeshTerrainBuild UUnrealAIMeshTerrainRqlApi::WaitForBuild(UE::MeshPartition::AMeshPartition* Terrain, double TimeoutSeconds)
{
	return UnrealAIMeshTerrainRqlApiLocal::RunBuild(Terrain, TimeoutSeconds, [](UE::MeshPartition::AMeshPartition* Built, double Seconds) { return UnrealAIMeshTerrain::WaitForBuild(Built, Seconds); });
}

FUnrealAIMeshTerrainSurface UUnrealAIMeshTerrainRqlApi::Surface(UE::MeshPartition::AMeshPartition* Terrain, FVector Point)
{
	const TArray<FUnrealAIMeshTerrainSurface> Out = SurfacePoints(Terrain, { Point });
	return Out.IsEmpty() ? FUnrealAIMeshTerrainSurface() : Out[0];
}

TArray<FUnrealAIMeshTerrainSurface> UUnrealAIMeshTerrainRqlApi::SurfacePoints(UE::MeshPartition::AMeshPartition* Terrain, const TArray<FVector>& Points)
{
	TArray<FUnrealAIMeshTerrainSurface> Out;
	if (Points.Num() > 10000)
		return UnrealMcpRql::Failed<TArray<FUnrealAIMeshTerrainSurface>>(FString::Printf(TEXT("%d points: at most 10000 per call"), Points.Num()));
	if (!UnrealAIMeshTerrain::CheckTerrain(Terrain) || !UnrealAIMeshTerrain::WaitForBuild(Terrain))
		return Out;
	// The paint channels come from evaluating the modifier stack: once over all the points (cached by the builder).
	const FBox Built = UnrealAIMeshTerrain::BuiltBounds(Terrain);
	FBox Region(ForceInit);
	for (const FVector& Point : Points)
		Region += FVector(Point.X, Point.Y, 0.0);
	TArray<UnrealAIMeshTerrain::FRegionMesh> Evaluated;
	// Points spread wider than 500 m (a long road, scattered spawn points) evaluate a small region each instead.
	constexpr double MaxSharedRegion = 50000.0;
	const bool bShared = Built.IsValid && Region.IsValid && Region.GetSize().X <= MaxSharedRegion && Region.GetSize().Y <= MaxSharedRegion;
	if (bShared)
	{
		Region = Region.ExpandBy(FVector(250.0, 250.0, 0.0));
		Evaluated = UnrealAIMeshTerrain::BuildRegion(Terrain, FBox(FVector(Region.Min.X, Region.Min.Y, Built.Min.Z - 1000.0), FVector(Region.Max.X, Region.Max.Y, Built.Max.Z + 1000.0)));
	}
	// Each scattered point evaluates its own region: bound the whole call, not only each evaluation.
	const double Start = FPlatformTime::Seconds();
	for (const FVector& Point : Points)
	{
		if (!bShared && FPlatformTime::Seconds() - Start > UnrealAIMeshTerrain::SampleBudgetSeconds)
		{
			return UnrealMcpRql::Failed<TArray<FUnrealAIMeshTerrainSurface>>(FString::Printf(
				TEXT("sampling %d points spread over more than 500 m took over %.0f s (%d done; each such point evaluates the terrain around it): pass points within 500 m of each other per call, or fewer points"),
				Points.Num(), UnrealAIMeshTerrain::SampleBudgetSeconds, Out.Num()));
		}
		const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, Point.X, Point.Y, /*bChannels*/ true, &Evaluated);
		FUnrealAIMeshTerrainSurface& Surface = Out.AddDefaulted_GetRef();
		if (!Sample.bInside)
			continue;
		Surface.Inside = true;
		Surface.Height = Sample.Height;
		Surface.Normal = Sample.Normal.Z < 0.0 ? -Sample.Normal : Sample.Normal;
		Surface.Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Surface.Normal.Z, -1.0, 1.0)));
		Surface.Location = FVector(Point.X, Point.Y, Sample.Height);
		for (const TPair<FName, float>& Channel : Sample.Channels)
			Surface.Channels.Add(Channel.Key.ToString(), FMath::RoundToDouble(Channel.Value * 1000.0) / 1000.0);
	}
	return Out;
}

FString UUnrealAIMeshTerrainRqlApi::EnableMeshTerrain()
{
	TArray<FString> Done;
	bool bRestart = false;
	for (const TCHAR* Name : { TEXT("MeshPartition"), TEXT("MeshTerrainMode") })
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
		if (!Plugin.IsValid())
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("this engine has no %s plugin: Mesh Terrain needs Unreal Engine 5.8 or newer"), Name));
		const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
		const bool bListed = Project && Project->Plugins.ContainsByPredicate([Name](const FPluginReferenceDescriptor& Reference)
		{
			return Reference.Name == Name && Reference.bEnabled;
		});
		if (!bListed)
		{
			FText Reason;
			if (!IProjectManager::Get().SetPluginEnabled(Name, true, Reason) || !IProjectManager::Get().SaveCurrentProjectToDisk(Reason))
				return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not enable %s in the project file: %s"), Name, *Reason.ToString()));
			Done.Add(FString::Printf(TEXT("%s is now enabled in the project file"), Name));
		}
		else
		{
			Done.Add(FString::Printf(TEXT("%s is enabled in the project file"), Name));
		}
		bRestart |= !Plugin->IsEnabled();
	}
	Done.Add(bRestart ? TEXT("restart the Unreal Editor to load them") : TEXT("loaded now, no restart needed"));
	return FString::Join(Done, TEXT("; "));
}
