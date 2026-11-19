// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UnrealMcpRqlTools.h"
#include "UObject/Package.h"
#include "WorldPartition/WorldPartition.h"

/**
 * World Partition actors that are not loaded (docs/rql.md "World Partition"). On a World Partition level `actors` lists only
 * the loaded actors: wave agents never saw a lake or orphaned landscape proxies outside the loaded regions and spent up to 34
 * Python calls loading regions. Now the answer warns, `unloadedActors` lists the rest, and `editor | call LoadActors / LoadRegion`
 * loads them without marking anything unsaved or adding an undo step.
 *
 * The spec builds a real World Partition level from the engine's Open World template, places a cube far outside every
 * loaded region, saves, and reopens it (reopening loads no region, so the cube is not loaded).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlWorldPartitionSpec, "UnrealMcp.Tools.Rql.WorldPartition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static constexpr const TCHAR* MapPackage = TEXT("/Game/UnrealMcpWpSpec/WpSpecMap");

	TSharedPtr<FJsonObject> Run(const TCHAR* Plan, bool bMutate = false)
	{
		return UnrealMcpRqlSpecHelpers::Exec(*this, Plan, bMutate);
	}

	FString Said(const TSharedPtr<FJsonObject>& Result)
	{
		return UnrealMcpRqlSpecHelpers::Codes(Result) + TEXT(" | ") + UnrealMcpRqlSpecHelpers::Hints(Result);
	}

	double CountFar()
	{
		// actors | where Label == "WpSpecFar" | count
		return UnrealMcpRqlSpecHelpers::Number(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"WpSpecFar"}]}},{"op":"count"}]})RQL")));
	}

	double CountUnloadedFar()
	{
		// unloadedActors | where Label == "WpSpecFar" | count
		return UnrealMcpRqlSpecHelpers::Number(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["unloadedActors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"WpSpecFar"}]}},{"op":"count"}]})RQL")));
	}

	bool Reopen()
	{
		// editor | call OpenLevel("/Game/UnrealMcpWpSpec/WpSpecMap")
		const TSharedPtr<FJsonObject> Opened = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"OpenLevel","args":[{"lit":"/Game/UnrealMcpWpSpec/WpSpecMap"}]}]})RQL"), true);
		return TestTrue(*FString::Printf(TEXT("reopened (%s)"), *Said(Opened)), UnrealMcpRqlSpecHelpers::IsOk(Opened));
	}

	static TArray<FString> DirtyPackages()
	{
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyWorldPackages(Dirty);
		FEditorFileUtils::GetDirtyContentPackages(Dirty);
		TArray<FString> Names;
		for (const UPackage* Package : Dirty)
			Names.Add(Package->GetName());
		return Names;
	}

	static int32 UndoSteps()
	{
		return GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
	}

	static void RemoveMapFiles()
	{
		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("UnrealMcpWpSpec"));
		IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		// The external actors of the level live beside it, under __ExternalActors__ / __ExternalObjects__.
		for (const TCHAR* External : { TEXT("__ExternalActors__"), TEXT("__ExternalObjects__") })
		{
			const FString Root = FPaths::ProjectContentDir() / External;
			IFileManager::Get().DeleteDirectory(*(Root / TEXT("UnrealMcpWpSpec")), false, true);
			// The level made the folder: leave none behind when nothing else is in it.
			bool bEmpty = true;
			IFileManager::Get().IterateDirectory(*Root, [&bEmpty](const TCHAR*, bool) { bEmpty = false; return false; });
			if (bEmpty)
				IFileManager::Get().DeleteDirectory(*Root, false, false);
		}
	}

	/**
	 * A World Partition level from the Open World template with the cube "WpSpecFar" far outside every loaded region,
	 * saved and reopened (no region loads on open, so the cube is not loaded). False after a test error.
	 */
	bool MakeFarCubeLevel()
	{
		using namespace UnrealMcpRqlSpecHelpers;
		RemoveMapFiles();
		// editor | call NewLevel("/Game/UnrealMcpWpSpec/WpSpecMap", "/Engine/Maps/Templates/OpenWorld")
		const TSharedPtr<FJsonObject> Created = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"NewLevel","args":[{"lit":"/Game/UnrealMcpWpSpec/WpSpecMap"},{"lit":"/Engine/Maps/Templates/OpenWorld"}]}]})RQL"), true);
		if (!TestTrue(*FString::Printf(TEXT("a World Partition level from the Open World template (%s)"), *Said(Created)), IsOk(Created)))
			return false;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!TestTrue(TEXT("it has World Partition"), World && World->GetWorldPartition()))
			return false;
		// editor | call SpawnActor("/Engine/BasicShapes/Cube", "WpSpecFar", [400000, 400000, 0])
		const TSharedPtr<FJsonObject> Spawned = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"/Engine/BasicShapes/Cube"},{"lit":"WpSpecFar"},{"list":[{"lit":400000},{"lit":400000},{"lit":0}]}]}]})RQL"), true);
		TestTrue(*FString::Printf(TEXT("cube placed far away (%s)"), *Said(Spawned)), IsOk(Spawned));
		// editor | call SaveLevel()
		const TSharedPtr<FJsonObject> Saved = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SaveLevel","args":[]}]})RQL"), true);
		return TestTrue(*FString::Printf(TEXT("saved (%s)"), *Said(Saved)), IsOk(Saved)) && Reopen()
			&& TestEqual(TEXT("the cube is not loaded after reopening"), CountUnloadedFar(), 1.0);
	}

	/** Leave a plain level open and drop the spec's files. */
	void LeaveLevel()
	{
		// editor | call NewLevel()
		Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"NewLevel","args":[]}]})RQL"), true);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		RemoveMapFiles();
	}

END_DEFINE_SPEC(FUnrealMcpRqlWorldPartitionSpec)

void FUnrealMcpRqlWorldPartitionSpec::Define()
{
	It("warns about unloaded actors, lists them in unloadedActors and loads them by name or region, unsaved changes none", [this]()
	{
		using namespace UnrealMcpRqlSpecHelpers;
		ON_SCOPE_EXIT { LeaveLevel(); };
		if (!MakeFarCubeLevel())
			return;

		// The cube is not loaded: actors misses it and says so.
		const TSharedPtr<FJsonObject> Listed = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"WpSpecFar"}]}},{"op":"count"}]})RQL"));
		TestEqual(TEXT("actors does not list the unloaded cube"), Number(Listed), 0.0);
		TestTrue(*FString::Printf(TEXT("and warns how many are not loaded and how to see them (%s)"), *Said(Listed)),
			Said(Listed).Contains(TEXT("world-partition-unloaded")) && Said(Listed).Contains(TEXT("World Partition actor(s) are not loaded (only loaded actors are listed)"))
			&& Said(Listed).Contains(TEXT("unloadedActors")) && Said(Listed).Contains(TEXT("editor | call LoadActors(")));

		// unloadedActors | where Label == "WpSpecFar" | select Class Location.X Package SpatiallyLoaded
		const TSharedPtr<FJsonObject> Row = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["unloadedActors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"WpSpecFar"}]}},{"op":"select","fields":[{"name":"Class","expr":{"path":["Class"]}},{"name":"Location.X","expr":{"path":["Location","X"]}},{"name":"Package","expr":{"path":["Package"]}},{"name":"SpatiallyLoaded","expr":{"path":["SpatiallyLoaded"]}}]}]})RQL"));
		TestEqual(*FString::Printf(TEXT("unloadedActors lists it (%s)"), *Said(Row)), RowCount(Row), 1);
		TestEqual(TEXT("its class"), CellText(Row, 0, 0), FString(TEXT("StaticMeshActor")));
		TestEqual(TEXT("its location"), CellText(Row, 0, 1), FString(TEXT("400000")));
		TestTrue(*FString::Printf(TEXT("its external package (%s)"), *CellText(Row, 0, 2)), CellText(Row, 0, 2).Contains(TEXT("__ExternalActors__")));
		TestEqual(TEXT("spatially loaded"), CellText(Row, 0, 3), FString(TEXT("true")));

		// unreal.find("WpSpecFar") | select Label: names the reason and the load
		const TSharedPtr<FJsonObject> Find = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"WpSpecFar"}]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"));
		TestTrue(*FString::Printf(TEXT("unreal.find says it is not loaded and how to load it (%s)"), *Said(Find)),
			!IsOk(Find) && Said(Find).Contains(TEXT("not-loaded")) && Said(Find).Contains(TEXT("LoadActors([\"WpSpecFar\"])")));

		// unreal.find("WpSpecFr") | select Label: a typo of an unloaded label names it
		const TSharedPtr<FJsonObject> Typo = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"WpSpecFr"}]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"));
		TestTrue(*FString::Printf(TEXT("a typo suggests the unloaded actor (%s)"), *Said(Typo)),
			!IsOk(Typo) && Said(Typo).Contains(TEXT("not-found")) && Said(Typo).Contains(TEXT("'WpSpecFar' (StaticMeshActor, not loaded)")));

		// editor | call LoadActors(["NoSuchWpActor"]): nothing loads on a typo
		const TSharedPtr<FJsonObject> LoadTypo = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"LoadActors","args":[{"list":[{"lit":"WpSpecFar"},{"lit":"NoSuchWpActor"}]}]}]})RQL"));
		TestTrue(*FString::Printf(TEXT("an unknown name fails (%s)"), *Said(LoadTypo)), !IsOk(LoadTypo) && Said(LoadTypo).Contains(TEXT("no World Partition actor 'NoSuchWpActor'")));
		TestEqual(TEXT("and loads nothing"), CountFar(), 0.0);

		// rql-query editor | call LoadActors(["WpSpecFar"]): a read, no unsaved change, no undo step
		const TArray<FString> DirtyBefore = DirtyPackages();
		const int32 StepsBefore = UndoSteps();
		const TSharedPtr<FJsonObject> Loaded = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"LoadActors","args":[{"list":[{"lit":"WpSpecFar"}]}]}]})RQL"));
		TestTrue(*FString::Printf(TEXT("rql-query loads it (%s)"), *Said(Loaded)), IsOk(Loaded) && CellText(Loaded, 0, 2).Contains(TEXT("StaticMeshActor#")));
		TestEqual(TEXT("actors lists it now"), CountFar(), 1.0);
		TestEqual(TEXT("unloadedActors no longer does"), CountUnloadedFar(), 0.0);
		TestEqual(*FString::Printf(TEXT("nothing marked unsaved (%s)"), *FString::Join(DirtyPackages(), TEXT(", "))), DirtyPackages(), DirtyBefore);
		TestEqual(TEXT("no undo step"), UndoSteps(), StepsBefore);
		// Loading it again is harmless: an already loaded actor is returned.
		const TSharedPtr<FJsonObject> Again = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"LoadActors","args":[{"list":[{"lit":"WpSpecFar"}]}]}]})RQL"));
		TestTrue(*FString::Printf(TEXT("loading a loaded actor returns it (%s)"), *Said(Again)), IsOk(Again) && CellText(Again, 0, 2).Contains(TEXT("StaticMeshActor#")));

		// By region, after a fresh open: editor | call LoadRegion([399000, 399000], [401000, 401000])
		if (!Reopen())
			return;
		TestEqual(TEXT("unloaded again after reopening"), CountUnloadedFar(), 1.0);
		const TArray<FString> DirtyBeforeRegion = DirtyPackages();
		const TSharedPtr<FJsonObject> Region = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"LoadRegion","args":[{"list":[{"lit":399000},{"lit":399000}]},{"list":[{"lit":401000},{"lit":401000}]}]}]})RQL"));
		TestTrue(*FString::Printf(TEXT("LoadRegion loads what overlaps the rectangle (%s)"), *Said(Region)), IsOk(Region) && CellText(Region, 0, 2).Contains(TEXT("StaticMeshActor#")));
		TestEqual(TEXT("actors lists it"), CountFar(), 1.0);
		TestEqual(TEXT("LoadRegion marks nothing unsaved"), DirtyPackages(), DirtyBeforeRegion);
		// editor | call LoadRegion([1, 1], [0, 0])
		const TSharedPtr<FJsonObject> Inverted = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"LoadRegion","args":[{"list":[{"lit":1},{"lit":1}]},{"list":[{"lit":0},{"lit":0}]}]}]})RQL"));
		TestTrue(*FString::Printf(TEXT("an inverted rectangle fails (%s)"), *Said(Inverted)), !IsOk(Inverted) && Said(Inverted).Contains(TEXT("must not exceed Max")));
	});

	It("loads by GUID and by region through the public API for RQL modules, unsaved changes none", [this]()
	{
		using namespace UnrealMcpRqlSpecHelpers;
		ON_SCOPE_EXIT { LeaveLevel(); };
		if (!MakeFarCubeLevel())
			return;
		const TArray<FString> DirtyBefore = DirtyPackages();
		const int32 StepsBefore = UndoSteps();
		// A region beside the cube loads nothing; one over it loads it.
		TArray<AActor*> Loaded;
		FString Error;
		const FBox2D OverCube(FVector2D(399000, 399000), FVector2D(401000, 401000));
		bool bOk = UnrealMcpRql::LoadWorldPartitionRegion(FBox2D(FVector2D(-1000, -1000), FVector2D(1000, 1000)), &Loaded, &Error);
		TestTrue(*FString::Printf(TEXT("a region away from the cube (%s)"), *Error), bOk);
		TestFalse(TEXT("does not load it"), Loaded.ContainsByPredicate([](const AActor* A) { return A && A->GetActorLabel() == TEXT("WpSpecFar"); }));
		FBox2D Inverted(FVector2D(1, 1), FVector2D(0, 0));
		Inverted.bIsValid = true;
		bOk = UnrealMcpRql::LoadWorldPartitionRegion(Inverted, &Loaded, &Error);
		TestTrue(*FString::Printf(TEXT("an inverted box is refused (%s)"), *Error), !bOk && Error.Contains(TEXT("not a valid box")) && Loaded.IsEmpty());
		TestFalse(TEXT("an unset box is refused"), UnrealMcpRql::LoadWorldPartitionRegion(FBox2D(ForceInit), &Loaded));
		TestTrue(TEXT("an empty GUID list is nothing to do"), UnrealMcpRql::LoadWorldPartitionActors({}));
		bOk = UnrealMcpRql::LoadWorldPartitionRegion(OverCube, &Loaded, &Error);
		TestTrue(*FString::Printf(TEXT("a region over the cube (%s)"), *Error), bOk);
		TestTrue(TEXT("returns it"), Loaded.Num() == 1 && Loaded[0] && Loaded[0]->GetActorLabel() == TEXT("WpSpecFar"));
		TestEqual(TEXT("actors lists it"), CountFar(), 1.0);
		TestTrue(TEXT("the same region again loads nothing new"), UnrealMcpRql::LoadWorldPartitionRegion(OverCube, &Loaded) && Loaded.IsEmpty());
		TestEqual(*FString::Printf(TEXT("nothing marked unsaved (%s)"), *FString::Join(DirtyPackages(), TEXT(", "))), DirtyPackages(), DirtyBefore);
		TestEqual(TEXT("no undo step"), UndoSteps(), StepsBefore);

		// By GUID (unloadedActors | where Label == "WpSpecFar" | select Guid), after a fresh open.
		if (!Reopen())
			return;
		const TSharedPtr<FJsonObject> Row = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["unloadedActors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"WpSpecFar"}]}},{"op":"select","fields":[{"name":"Guid","expr":{"path":["Guid"]}}]}]})RQL"));
		FGuid Guid;
		if (!TestTrue(*FString::Printf(TEXT("its GUID (%s)"), *CellText(Row)), FGuid::Parse(CellText(Row), Guid)))
			return;
		const TArray<FString> DirtyBeforeGuid = DirtyPackages();
		const int32 StepsBeforeGuid = UndoSteps();
		bOk = UnrealMcpRql::LoadWorldPartitionActors({ Guid, Guid, FGuid::NewGuid() }, &Error);
		TestTrue(*FString::Printf(TEXT("loads by GUID, skipping a repeat and an unknown one (%s)"), *Error), bOk);
		TestEqual(TEXT("actors lists it"), CountFar(), 1.0);
		TestTrue(TEXT("a loaded actor's GUID is nothing to do"), UnrealMcpRql::LoadWorldPartitionActors({ Guid }));
		TestEqual(TEXT("nothing marked unsaved"), DirtyPackages(), DirtyBeforeGuid);
		TestEqual(TEXT("no undo step"), UndoSteps(), StepsBeforeGuid);
	});

	It("stays quiet on a level without World Partition, where LoadActors says why it has nothing to do and the public API loads nothing", [this]()
	{
		using namespace UnrealMcpRqlSpecHelpers;
		// editor | call NewLevel()
		const TSharedPtr<FJsonObject> Created = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"NewLevel","args":[]}]})RQL"), true);
		if (!TestTrue(*FString::Printf(TEXT("a plain level (%s)"), *Said(Created)), IsOk(Created)))
			return;
		// actors | count
		const TSharedPtr<FJsonObject> Count = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"count"}]})RQL"));
		TestFalse(*FString::Printf(TEXT("no warning (%s)"), *Said(Count)), Said(Count).Contains(TEXT("world-partition-unloaded")));
		// unloadedActors | count
		TestEqual(TEXT("unloadedActors is empty"), Number(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["unloadedActors"]},"stages":[{"op":"count"}]})RQL"))), 0.0);
		// editor | call LoadActors(["Anything"])
		const TSharedPtr<FJsonObject> Load = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"LoadActors","args":[{"list":[{"lit":"Anything"}]}]}]})RQL"));
		TestTrue(*FString::Printf(TEXT("LoadActors explains (%s)"), *Said(Load)), !IsOk(Load) && Said(Load).Contains(TEXT("has no World Partition: every actor is loaded")));
		// The public API has nothing to load there: success, nothing loaded.
		TArray<AActor*> Loaded;
		TestTrue(TEXT("LoadWorldPartitionActors without World Partition"), UnrealMcpRql::LoadWorldPartitionActors({ FGuid::NewGuid() }));
		TestTrue(TEXT("LoadWorldPartitionRegion without World Partition"), UnrealMcpRql::LoadWorldPartitionRegion(FBox2D(FVector2D(0, 0), FVector2D(1, 1)), &Loaded) && Loaded.IsEmpty());
	});

	It("fails a change over actors that reaches no loaded actor while some are not loaded, and changes a loaded one with the warning", [this]()
	{
		using namespace UnrealMcpRqlSpecHelpers;
		ON_SCOPE_EXIT { LeaveLevel(); };
		if (!MakeFarCubeLevel())
			return;
		// rql-mutate actors | where Label == "WpSpecFar" | set HiddenInEditor = true: the cube is not loaded, so nothing is
		// changed; it used to answer an empty receipt with a warning, which agents read as done (DEV-113).
		const TSharedPtr<FJsonObject> Unloaded = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"WpSpecFar"}]}},{"op":"set","assign":[{"target":{"path":["HiddenInEditor"]},"value":{"lit":true}}]}]})RQL"), true);
		TestTrue(*FString::Printf(TEXT("fails, says nothing was changed and how to load it (%s)"), *Said(Unloaded)),
			!IsOk(Unloaded) && Said(Unloaded).Contains(TEXT("world-partition-unloaded")) && Said(Unloaded).Contains(TEXT("nothing was changed"))
			&& Said(Unloaded).Contains(TEXT("editor | call LoadActors(")));
		// The same as a read stays an empty answer with the warning.
		const TSharedPtr<FJsonObject> Read = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"WpSpecFar"}]}}]})RQL"));
		TestTrue(*FString::Printf(TEXT("a read only warns (%s)"), *Said(Read)), IsOk(Read) && Said(Read).Contains(TEXT("world-partition-unloaded")));
		// editor | call SpawnActor("/Engine/BasicShapes/Cube", "WpSpecNear", [0, 0, 0]): a loaded actor beside the unloaded one
		const TSharedPtr<FJsonObject> Spawned = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"/Engine/BasicShapes/Cube"},{"lit":"WpSpecNear"},{"list":[{"lit":0},{"lit":0},{"lit":0}]}]}]})RQL"), true);
		TestTrue(*FString::Printf(TEXT("a loaded cube (%s)"), *Said(Spawned)), IsOk(Spawned));
		// actors | where Label == "WpSpecNear" | set HiddenInEditor = true
		const TSharedPtr<FJsonObject> Loaded = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"WpSpecNear"}]}},{"op":"set","assign":[{"target":{"path":["HiddenInEditor"]},"value":{"lit":true}}]}]})RQL"), true);
		TestTrue(*FString::Printf(TEXT("a change that reaches a loaded actor succeeds, with the warning (%s)"), *Said(Loaded)),
			IsOk(Loaded) && RowCount(Loaded) == 1 && Said(Loaded).Contains(TEXT("world-partition-unloaded")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
