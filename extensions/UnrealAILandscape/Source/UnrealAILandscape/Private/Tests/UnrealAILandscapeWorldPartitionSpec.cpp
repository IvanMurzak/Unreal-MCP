// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Landscape.h"
#include "LandscapeStreamingProxy.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "WorldPartition/WorldPartition.h"

// ============================================================================================
//  UE Automation spec: a landscape on a World Partition level that was reopened. Its streaming proxies are spatially
//  loaded actors that stay unloaded after the open; the read-only operations and sources load them (LoadDescs, through the
//  core UnrealMcpRql::LoadWorldPartitionActors) without an unsaved change or an undo step (wave 3: a C-R9 run that only
//  read the landscape was suspected of dirtying all 16 proxies), and a landscape deleted in this session stays deleted.
//
//  The level comes from the engine's Open World template; the landscape sits far outside every loaded region.
//
//  Run via:  Automation RunTests UnrealAILandscape.WorldPartition
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAILandscapeWorldPartitionSpec, "UnrealAILandscape.WorldPartition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static constexpr const TCHAR* Folder = TEXT("UnrealAILandscapeWpSpec");
	static constexpr const TCHAR* Label = TEXT("WpSpecLand");

	static FString WpJson(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** Run a plan; a test error with its result when it fails. */
	bool WpRun(const FString& What, const FString& Plan, bool bMutate = true)
	{
		TSharedPtr<FJsonObject> Result;
		const bool bOk = UnrealMcpRqlTools::RunPlan(Plan, bMutate, &Result);
		TestTrue(*FString::Printf(TEXT("%s (%s)"), *What, *WpJson(Result).Left(600)), bOk);
		return bOk;
	}

	/** `<Source> | call <Method>(<Args>)` */
	static FString WpCall(const TCHAR* Source, const FString& Method, const FString& Args)
	{
		return FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["%s"]},"stages":[{"op":"call","method":"%s","args":[%s]}]})RQL"), Source, *Method, *Args);
	}

	/** `unreal.find("WpSpecLand")` as a plan argument */
	static FString WpFind()
	{
		return FString::Printf(TEXT(R"({"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]})"), Label);
	}

	static FString WpMapPackage()
	{
		return FString::Printf(TEXT("/Game/%s/WpMap"), Folder);
	}

	/** Loaded streaming proxies of the landscape @p Guid. */
	static int32 WpProxies(const FGuid& Guid)
	{
		int32 Count = 0;
		for (TActorIterator<ALandscapeStreamingProxy> It(UnrealMcpRql::EditorWorld()); It; ++It)
			Count += IsValid(*It) && It->GetLandscapeGuid() == Guid ? 1 : 0;
		return Count;
	}

	static ALandscape* WpLandscape()
	{
		for (TActorIterator<ALandscape> It(UnrealMcpRql::EditorWorld()); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == Label)
				return *It;
		}
		return nullptr;
	}

	static TArray<FString> WpDirtyPackages()
	{
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyWorldPackages(Dirty);
		FEditorFileUtils::GetDirtyContentPackages(Dirty);
		TArray<FString> Names;
		for (const UPackage* Package : Dirty)
			Names.Add(Package->GetName());
		return Names;
	}

	static int32 WpUndoSteps()
	{
		return GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
	}

	/** Nothing loading did may show up as an unsaved change or an undo step (other specs may leave unsaved assets). */
	void WpNothingChanged(const TCHAR* What, int32 UndoBefore, const TArray<FString>& DirtyBefore)
	{
		TestEqual(*FString::Printf(TEXT("%s added no undo step"), What), WpUndoSteps(), UndoBefore);
		TArray<FString> Dirty = WpDirtyPackages();
		Dirty.RemoveAll([&DirtyBefore](const FString& Name) { return DirtyBefore.Contains(Name); });
		TestTrue(*FString::Printf(TEXT("%s left nothing unsaved (%s)"), What, *FString::Join(Dirty, TEXT(", "))), Dirty.IsEmpty());
	}

	static void WpRemoveFiles()
	{
		IFileManager::Get().DeleteDirectory(*FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Folder), false, true);
		for (const TCHAR* External : { TEXT("__ExternalActors__"), TEXT("__ExternalObjects__") })
		{
			const FString Root = FPaths::ProjectContentDir() / External;
			IFileManager::Get().DeleteDirectory(*(Root / Folder), false, true);
			bool bEmpty = true;
			IFileManager::Get().IterateDirectory(*Root, [&bEmpty](const TCHAR*, bool) { bEmpty = false; return false; });
			if (bEmpty)
				IFileManager::Get().DeleteDirectory(*Root, false, false);
		}
	}
END_DEFINE_SPEC(FUnrealAILandscapeWorldPartitionSpec)

void FUnrealAILandscapeWorldPartitionSpec::Define()
{
	It("loads a reopened landscape's streaming proxies for a read, unsaved changes none, and keeps a landscape deleted in this session deleted", [this]()
	{
		ON_SCOPE_EXIT
		{
			UnrealMcpRqlTools::RunPlan(WpCall(TEXT("editor"), TEXT("NewLevel"), TEXT("")), true);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			WpRemoveFiles();
		};
		WpRemoveFiles();
		const FString Map = WpMapPackage();
		// editor | call NewLevel("/Game/UnrealAILandscapeWpSpec/WpMap", "/Engine/Maps/Templates/OpenWorld")
		if (!WpRun(TEXT("a World Partition level from the Open World template"),
			WpCall(TEXT("editor"), TEXT("NewLevel"), FString::Printf(TEXT(R"({"lit":"%s"},{"lit":"/Engine/Maps/Templates/OpenWorld"})"), *Map))))
			return;
		const UWorld* World = UnrealMcpRql::EditorWorld();
		if (!TestTrue(TEXT("it has World Partition"), World && World->GetWorldPartition()))
			return;

		// A 600 m landscape far outside the loaded region (2 x 2 streaming proxies at least), saved and reopened.
		const double X = 500000.0, Y = 500000.0;
		// landscape | call Create("WpSpecLand", [500000, 500000, 0], 600)
		if (!WpRun(TEXT("created"), WpCall(TEXT("landscape"), TEXT("Create"),
			FString::Printf(TEXT(R"({"lit":"%s"},{"list":[{"lit":%.17g},{"lit":%.17g},{"lit":0}]},{"lit":600})"), Label, X, Y))))
			return;
		// landscape | call Sculpt(unreal.find("WpSpecLand"), [500000, 500000, 0], 10000, 2000): a hill to read back
		if (!WpRun(TEXT("sculpted"), WpCall(TEXT("landscape"), TEXT("Sculpt"),
			FString::Printf(TEXT(R"(%s,{"list":[{"lit":%.17g},{"lit":%.17g},{"lit":0}]},{"lit":10000},{"lit":2000})"), *WpFind(), X, Y))))
			return;
		// The Open World template has a landscape of its own: count only this one's proxies.
		const ALandscape* Created = WpLandscape();
		if (!TestNotNull(TEXT("the landscape is there"), Created))
			return;
		const FGuid Guid = Created->GetLandscapeGuid();
		const int32 AllProxies = WpProxies(Guid);
		if (!TestTrue(*FString::Printf(TEXT("the landscape has several streaming proxies (%d)"), AllProxies), AllProxies >= 4))
			return;
		if (!WpRun(TEXT("saved"), WpCall(TEXT("editor"), TEXT("SaveLevel"), TEXT("")))
			|| !WpRun(TEXT("reopened"), WpCall(TEXT("editor"), TEXT("OpenLevel"), FString::Printf(TEXT(R"({"lit":"%s"})"), *Map))))
			return;
		ALandscape* Landscape = WpLandscape();
		if (!TestNotNull(TEXT("the landscape actor is there after reopening"), Landscape))
			return;
		if (!TestEqual(TEXT("no streaming proxy is loaded after reopening"), WpProxies(Guid), 0))
			return;

		// landscape | call Surface(unreal.find("WpSpecLand"), [500000, 500000, 0]) in rql-query: loads the proxies there
		int32 UndoBefore = WpUndoSteps();
		TArray<FString> DirtyBefore = WpDirtyPackages();
		WpRun(TEXT("Surface runs read-only"), WpCall(TEXT("landscape"), TEXT("Surface"),
			FString::Printf(TEXT(R"(%s,{"list":[{"lit":%.17g},{"lit":%.17g},{"lit":0}]})"), *WpFind(), X, Y)), false);
		const int32 Near = WpProxies(Guid);
		TestTrue(*FString::Printf(TEXT("Surface loaded the proxies at its point (%d of %d)"), Near, AllProxies), Near > 0);
		// Collision holds the hill only after a merge, which needs rendering (the suite's -nullrhi editor keeps the edit unmerged).
		if (FApp::CanEverRender())
		{
			const TOptional<float> Height = Landscape->GetHeightAtLocation(FVector(X, Y, 0.0));
			TestTrue(*FString::Printf(TEXT("the loaded ground is the saved hill (%.1f cm)"), Height.Get(-1.0f)), Height.IsSet() && FMath::IsNearlyEqual(Height.GetValue(), 2000.0f, 5.0f));
		}
		WpNothingChanged(TEXT("Surface"), UndoBefore, DirtyBefore);

		// landscapes | select Label in rql-query: the source loads every proxy of the map
		UndoBefore = WpUndoSteps();
		DirtyBefore = WpDirtyPackages();
		WpRun(TEXT("landscapes lists"), TEXT(R"RQL({"rql":"0.1","source":{"path":["landscapes"]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"), false);
		TestEqual(TEXT("landscapes loaded every streaming proxy"), WpProxies(Guid), AllProxies);
		WpNothingChanged(TEXT("landscapes"), UndoBefore, DirtyBefore);

		// The edit log is the landscape actor's: the streaming proxies do not repeat it (DEV-113: 16 copies in a listing).
		// landscapes | where not IsPrimary and Edits.count() > 0 | count
		TSharedPtr<FJsonObject> Repeated;
		UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["landscapes"]},"stages":[{"op":"where","pred":{"op":"and","args":[{"op":"not","args":[{"path":["IsPrimary"]}]},{"op":">","args":[{"call":"count","on":{"path":["Edits"]},"args":[]},{"lit":0}]}]}},{"op":"count"}]})RQL"), false, &Repeated);
		double RepeatedCount = -1.0;
		TestTrue(*FString::Printf(TEXT("no streaming proxy repeats the edit log (%s)"), *WpJson(Repeated).Left(300)),
			Repeated.IsValid() && Repeated->TryGetNumberField(TEXT("value"), RepeatedCount) && RepeatedCount == 0.0);

		// landscape | call Delete(unreal.find("WpSpecLand")), unsaved: a later listing must not load the proxies back
		if (!WpRun(TEXT("deleted"), WpCall(TEXT("landscape"), TEXT("Delete"), WpFind())))
			return;
		TestEqual(TEXT("the proxies are gone"), WpProxies(Guid), 0);
		WpRun(TEXT("landscapes lists again"), TEXT(R"RQL({"rql":"0.1","source":{"path":["landscapes"]},"stages":[{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"), false);
		TestEqual(TEXT("listing again does not bring the deleted proxies back"), WpProxies(Guid), 0);
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
