// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealAIMeshTerrainCommon.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "WorldPartition/WorldPartition.h"

#include "MeshPartition.h"
#include "MeshPartitionModifierActor.h"

// ============================================================================================
//  UE Automation spec: a Mesh Terrain on a World Partition level that was reopened. Its base meshes and modifiers are
//  spatially loaded actors that stay unloaded after the open; the operations load them (UnrealAIMeshTerrain::
//  LoadTerrainActors, through the core UnrealMcpRql::LoadWorldPartitionActors) without an unsaved change or an undo step,
//  and a modifier deleted in this session stays deleted.
//
//  The level comes from the engine's Open World template; the terrain sits far outside every loaded region.
//
//  Run via:  Automation RunTests UnrealAIMeshTerrain.WorldPartition
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIMeshTerrainWorldPartitionSpec, "UnrealAIMeshTerrain.WorldPartition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	using AMeshPartition = UE::MeshPartition::AMeshPartition;

	static constexpr const TCHAR* Folder = TEXT("UnrealAIMeshTerrainWpSpec");

	static FString WpJson(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** Run a plan; a test error with its result when it fails. */
	bool WpRun(const FString& What, const FString& Plan, bool bMutate = true, TSharedPtr<FJsonObject>* OutResult = nullptr)
	{
		TSharedPtr<FJsonObject> Result;
		const bool bOk = UnrealMcpRqlTools::RunPlan(Plan, bMutate, &Result);
		TestTrue(*FString::Printf(TEXT("%s (%s)"), *What, *WpJson(Result).Left(600)), bOk);
		if (OutResult)
			*OutResult = Result;
		return bOk;
	}

	/** `editor | call <Method>(<Args>)` */
	static FString WpEditorCall(const FString& Method, const FString& Args)
	{
		return FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"%s","args":[%s]}]})RQL"), *Method, *Args);
	}

	static FString WpMapPackage()
	{
		return FString::Printf(TEXT("/Game/%s/WpMap"), Folder);
	}

	static int32 WpModifierActors(const FString& Label)
	{
		int32 Count = 0;
		for (TActorIterator<UE::MeshPartition::AModifierActor> It(UnrealMcpRql::EditorWorld()); It; ++It)
			Count += IsValid(*It) && It->GetActorLabel() == Label ? 1 : 0;
		return Count;
	}

	static AMeshPartition* WpTerrain(const FString& Label)
	{
		for (TActorIterator<AMeshPartition> It(UnrealMcpRql::EditorWorld()); It; ++It)
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
END_DEFINE_SPEC(FUnrealAIMeshTerrainWorldPartitionSpec)

void FUnrealAIMeshTerrainWorldPartitionSpec::Define()
{
	It("loads a reopened terrain's modifiers for an operation, unsaved changes none, and keeps a modifier deleted in this session deleted", [this]()
	{
		ON_SCOPE_EXIT
		{
			UnrealMcpRqlTools::RunPlan(WpEditorCall(TEXT("NewLevel"), TEXT("")), true);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			WpRemoveFiles();
		};
		WpRemoveFiles();
		const FString Map = WpMapPackage();
		// editor | call NewLevel("/Game/UnrealAIMeshTerrainWpSpec/WpMap", "/Engine/Maps/Templates/OpenWorld")
		if (!WpRun(TEXT("a World Partition level from the Open World template"),
			WpEditorCall(TEXT("NewLevel"), FString::Printf(TEXT(R"({"lit":"%s"},{"lit":"/Engine/Maps/Templates/OpenWorld"})"), *Map))))
			return;
		const UWorld* World = UnrealMcpRql::EditorWorld();
		if (!TestTrue(TEXT("it has World Partition"), World && World->GetWorldPartition()))
			return;

		// A 100 m terrain far outside the loaded region with a pad 3 m up at its centre, saved and reopened.
		const FString Label = TEXT("WpSpecTerrain");
		const FString Pad = TEXT("WpSpecPad");
		const double X = 400000.0, Y = 400000.0;
		// meshTerrainCreate | call Create("WpSpecTerrain", [400000, 400000, 0], 100)
		if (!WpRun(TEXT("created"), FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainCreate"]},"stages":[{"op":"call","method":"Create","args":[{"lit":"%s"},{"list":[{"lit":%.17g},{"lit":%.17g},{"lit":0}]},{"lit":100}]}]})RQL"), *Label, X, Y)))
			return;
		// meshTerrainModifier | call AddPatch(unreal.find("WpSpecTerrain"), [400000, 400000, 300], 1500, 500, "WpSpecPad")
		if (!WpRun(TEXT("pad added"), FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifier"]},"stages":[{"op":"call","method":"AddPatch","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},{"list":[{"lit":%.17g},{"lit":%.17g},{"lit":300}]},{"lit":1500},{"lit":500},{"lit":"%s"}]}]})RQL"), *Label, X, Y, *Pad)))
			return;
		if (!WpRun(TEXT("saved"), WpEditorCall(TEXT("SaveLevel"), TEXT("")))
			|| !WpRun(TEXT("reopened"), WpEditorCall(TEXT("OpenLevel"), FString::Printf(TEXT(R"({"lit":"%s"})"), *Map))))
			return;
		AMeshPartition* Terrain = WpTerrain(Label);
		if (!TestNotNull(TEXT("the terrain is there after reopening"), Terrain))
			return;
		if (!TestEqual(TEXT("the pad's actor is not loaded after reopening"), WpModifierActors(Pad), 0))
			return;

		// meshTerrain | call Surface(unreal.find("WpSpecTerrain"), [400000, 400000, 0]) in rql-query: the read loads the pad
		const int32 UndoBefore = WpUndoSteps();
		TSharedPtr<FJsonObject> Read;
		WpRun(TEXT("Surface runs read-only"), FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrain"]},"stages":[{"op":"call","method":"Surface","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},{"list":[{"lit":%.17g},{"lit":%.17g},{"lit":0}]}]}]})RQL"), *Label, X, Y), false, &Read);
		TestEqual(TEXT("Surface loaded the pad's actor"), WpModifierActors(Pad), 1);
		const UnrealAIMeshTerrain::FSurfaceSample Sample = UnrealAIMeshTerrain::SampleSurface(Terrain, X, Y, false);
		TestTrue(*FString::Printf(TEXT("the terrain is built with the pad: 300 cm at its centre (got %.2f)"), Sample.Height), Sample.bInside && FMath::IsNearlyEqual(Sample.Height, 300.0, 1.0));
		TestEqual(TEXT("loading added no undo step"), WpUndoSteps(), UndoBefore);
		TestTrue(*FString::Printf(TEXT("loading left nothing unsaved (%s)"), *FString::Join(WpDirtyPackages(), TEXT(", "))), WpDirtyPackages().IsEmpty());

		// meshTerrainModifier | call Remove(unreal.find("WpSpecPad").Modifier), unsaved: the next load must not bring it back
		if (!WpRun(TEXT("pad removed"), FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["meshTerrainModifier"]},"stages":[{"op":"call","method":"Remove","args":[{"path":["Modifier"],"on":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"%s"}]}}]}]})RQL"), *Pad)))
			return;
		TestEqual(TEXT("the pad's actor is gone"), WpModifierActors(Pad), 0);
		UnrealAIMeshTerrain::LoadTerrainActors(Terrain);
		TestEqual(TEXT("loading the terrain's actors again does not bring the deleted pad back"), WpModifierActors(Pad), 0);
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
