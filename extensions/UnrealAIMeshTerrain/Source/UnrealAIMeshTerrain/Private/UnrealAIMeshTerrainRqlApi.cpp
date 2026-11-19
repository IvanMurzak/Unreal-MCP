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

	/** A sample on the terrain (bInside) at @p Point as the operations' answer. */
	FUnrealAIMeshTerrainSurface ToSurface(const FVector& Point, const UnrealAIMeshTerrain::FSurfaceSample& Sample)
	{
		FUnrealAIMeshTerrainSurface Surface;
		Surface.Inside = true;
		Surface.Height = Sample.Height;
		Surface.Normal = Sample.Normal.Z < 0.0 ? -Sample.Normal : Sample.Normal;
		Surface.Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Surface.Normal.Z, -1.0, 1.0)));
		Surface.Location = FVector(Point.X, Point.Y, Sample.Height);
		for (const TPair<FName, float>& Channel : Sample.Channels)
			Surface.Channels.Add(Channel.Key.ToString(), FMath::RoundToDouble(Channel.Value * 1000.0) / 1000.0);
		return Surface;
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
	using namespace UnrealAIMeshTerrain;
	TArray<FUnrealAIMeshTerrainSurface> Out;
	if (Points.Num() > 10000)
		return UnrealMcpRql::Failed<TArray<FUnrealAIMeshTerrainSurface>>(FString::Printf(TEXT("%d points: at most 10000 per call"), Points.Num()));
	// The build wait (at most DefaultBuildTimeoutSeconds) leaves the sampling at least 30 s: together under the server's 300 s.
	const FCallBudget Budget(DefaultBuildTimeoutSeconds + 30.0);
	if (!CheckTerrain(Terrain) || !UnrealAIMeshTerrain::WaitForBuild(Terrain)) // (not this class's WaitForBuild)
		return Out;
	// The paint channels come from evaluating the modifier stack, which builds whole sections (cached by the builder): once
	// per cell of at most 500 m that holds points (all of them when they lie within 500 m), not once per point.
	const FBox Built = BuiltBounds(Terrain);
	FBox2D Spread(ForceInit);
	for (const FVector& Point : Points)
		Spread += FVector2D(Point);
	constexpr double CellSize = 50000.0;
	const bool bOneCell = Spread.GetSize().GetMax() <= CellSize;
	TMap<FIntPoint, TArray<int32>> Cells;
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FIntPoint Cell = bOneCell ? FIntPoint::ZeroValue : FIntPoint(FMath::FloorToInt32(Points[Index].X / CellSize), FMath::FloorToInt32(Points[Index].Y / CellSize));
		Cells.FindOrAdd(Cell).Add(Index);
	}
	Out.SetNum(Points.Num());
	// The sampling gets what the build wait left of the call's budget, at most SampleBudgetSeconds.
	const double SampleStart = FPlatformTime::Seconds();
	const double Deadline = SampleStart + FCallBudget::Left(SampleBudgetSeconds);
	int32 Done = 0;
	// Checked after every point: past the deadline the evaluations give up at once, and the rest would read no Channels.
	const auto TimedOut = [&Points, &Done, SampleStart, Deadline]()
	{
		return FPlatformTime::Seconds() > Deadline && UnrealMcpRql::Failed<bool>(FString::Printf(
			TEXT("sampling %d points took over %.0f s (%d done; the terrain is evaluated once per 500 m cell holding points): pass fewer points, or points closer together, per call"),
			Points.Num(), FPlatformTime::Seconds() - SampleStart, Done));
	};
	for (const TPair<FIntPoint, TArray<int32>>& Cell : Cells)
	{
		FBox2D Area(ForceInit);
		for (const int32 Index : Cell.Value)
			Area += FVector2D(Points[Index]);
		// The builder keeps only the triangles wholly inside the region: 25 m around the points holds the ones under them
		// (points on a line, or along the terrain's rim, would otherwise each evaluate a region of their own).
		Area = Area.ExpandBy(2500.0);
		TArray<FRegionMesh> Evaluated;
		if (Built.IsValid)
		{
			Evaluated = BuildRegion(Terrain, FBox(FVector(Area.Min, Built.Min.Z - 1000.0), FVector(Area.Max, Built.Max.Z + 1000.0)),
				FMath::Max(Deadline - FPlatformTime::Seconds(), 0.0), /*bFailOnTimeout*/ false);
		}
		for (const int32 Index : Cell.Value)
		{
			const FSurfaceSample Sample = SampleSurface(Terrain, Points[Index].X, Points[Index].Y, /*bChannels*/ true, &Evaluated);
			if (TimedOut())
				return TArray<FUnrealAIMeshTerrainSurface>();
			++Done;
			if (Sample.bInside)
				Out[Index] = UnrealAIMeshTerrainRqlApiLocal::ToSurface(Points[Index], Sample);
		}
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
