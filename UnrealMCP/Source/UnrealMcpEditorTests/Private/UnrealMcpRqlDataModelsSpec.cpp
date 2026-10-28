// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRqlSpecFixtures.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UnrealMcpRqlTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PointLightComponent.h"
#include "Components/TextBlock.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "WidgetBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#if __has_include("StructUtils/UserDefinedStruct.h")
#include "StructUtils/UserDefinedStruct.h" // moved from Engine/ to CoreUObject in UE 5.8
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "HAL/FileManager.h"
#include "ISettingsModule.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

/**
 * The data models reflection does not reach (docs/rql.md): DataTable rows, gameplay tags, Project Settings sections and
 * Blueprint graphs. Each case reads, writes, checks the validation errors, undo, dirty marking and what reaches disk
 * (the saved and reloaded asset, the written ini). Plans come from Rql.Parser 0.2.0 (the query is the comment before
 * each); "@NAME@" placeholders are filled with ids and paths known only at run time.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlDataModelsSpec, "UnrealMcp.Tools.Rql.DataModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	int32 Serial = 0;
	FString Table;      // /Game/RqlSpecData/DT_<n>
	FString Blueprint;  // /Game/RqlSpecData/BP_<n>
	TArray<UObject*> Created;
	bool bRegisteredSettings = false;
	bool bTagsSettingsIniExisted = true;

	TSharedPtr<FJsonObject> Run(const FString& Plan, bool bMutate = false,
		std::initializer_list<TPair<const TCHAR*, FString>> Values = {})
	{
		FString Filled = UnrealMcpRqlSpecHelpers::Fill(Plan, { { TEXT("DT"), Table }, { TEXT("BP"), Blueprint },
			{ TEXT("WBP"), WidgetBlueprintPath }, { TEXT("LS"), SequencePath } });
		return UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(Filled, Values), bMutate);
	}

	/** A mutation expected to succeed; returns the result (the receipt). */
	TSharedPtr<FJsonObject> Must(const TCHAR* What, const FString& Plan, std::initializer_list<TPair<const TCHAR*, FString>> Values = {})
	{
		const TSharedPtr<FJsonObject> Result = Run(Plan, true, Values);
		TestTrue(*FString::Printf(TEXT("%s (%s)"), What, *UnrealMcpRqlSpecHelpers::Codes(Result)), UnrealMcpRqlSpecHelpers::IsOk(Result));
		return Result;
	}

	/** A mutation expected to fail with a diagnostic containing @p Expected. */
	void Refuses(const TCHAR* What, const FString& Plan, const FString& Expected, std::initializer_list<TPair<const TCHAR*, FString>> Values = {})
	{
		const TSharedPtr<FJsonObject> Result = Run(Plan, true, Values);
		TestFalse(*FString::Printf(TEXT("%s: refused"), What), UnrealMcpRqlSpecHelpers::IsOk(Result));
		TestTrue(*FString::Printf(TEXT("%s: says '%s' (%s)"), What, *Expected, *UnrealMcpRqlSpecHelpers::Codes(Result)),
			UnrealMcpRqlSpecHelpers::Codes(Result).Contains(Expected));
	}

	/** The object a call returned (the receipt's result column). */
	static FString ResultRef(const TSharedPtr<FJsonObject>& Receipt)
	{
		return UnrealMcpRqlSpecHelpers::CellText(Receipt, 0, 2);
	}

	UDataTable* FindTable() const
	{
		return FindObject<UDataTable>(nullptr, *(Table + TEXT(".") + FPackageName::GetShortName(Table)));
	}

	UBlueprint* FindBlueprint() const
	{
		return FindObject<UBlueprint>(nullptr, *(Blueprint + TEXT(".") + FPackageName::GetShortName(Blueprint)));
	}

	/** A Blueprint actor with a double variable Health and a bool variable bArmed, registered with the asset registry. */
	UBlueprint* MakeBlueprint()
	{
		UPackage* Package = CreatePackage(*Blueprint);
		UBlueprint* Made = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, *FPackageName::GetShortName(Blueprint),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		FEdGraphPinType Real;
		Real.PinCategory = UEdGraphSchema_K2::PC_Real;
		Real.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		FBlueprintEditorUtils::AddMemberVariable(Made, TEXT("Health"), Real);
		FEdGraphPinType Bool;
		Bool.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(Made, TEXT("bArmed"), Bool);
		FKismetEditorUtilities::CompileBlueprint(Made);
		FAssetRegistryModule::AssetCreated(Made);
		Created.Add(Made);
		return Made;
	}

	FString WidgetBlueprintPath;
	FString SequencePath;

	/** An empty Widget Blueprint (no root widget), as a Blueprint created from UserWidget. */
	UWidgetBlueprint* MakeWidgetBlueprint()
	{
		UPackage* Package = CreatePackage(*WidgetBlueprintPath);
		UWidgetBlueprint* Made = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package,
			*FPackageName::GetShortName(WidgetBlueprintPath), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
		if (Made)
		{
			FAssetRegistryModule::AssetCreated(Made);
			Created.Add(Made);
		}
		return Made;
	}

	/** A Level Sequence at 30 fps, frames 0..150. */
	ULevelSequence* MakeSequence()
	{
		UPackage* Package = CreatePackage(*SequencePath);
		ULevelSequence* Made = NewObject<ULevelSequence>(Package, *FPackageName::GetShortName(SequencePath), RF_Public | RF_Standalone | RF_Transactional);
		Made->Initialize();
		Made->GetMovieScene()->SetDisplayRate(FFrameRate(30, 1));
		FAssetRegistryModule::AssetCreated(Made);
		Created.Add(Made);
		return Made;
	}

	static void DestroySpecActors()
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
			return;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel().StartsWith(TEXT("RqlSeqLamp")))
				World->EditorDestroyActor(*It, true);
		}
	}

	static FString SettingsIni()
	{
		return FPaths::ConvertRelativePathToFull(GetDefault<UUnrealMcpRqlSpecSettings>()->GetDefaultConfigFilename());
	}

	static FString TagIni()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("Tags"), TEXT("RqlSpecTags.ini")));
	}

	static FString FileText(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	/** Remove every gameplay tag of the specs (their ini source too) and refresh the tag tree. */
	void DropTags()
	{
		UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
		FGameplayTagContainer All;
		Manager.RequestAllGameplayTags(All, /*OnlyIncludeDictionaryTags*/ true);
		TArray<FString> Names;
		for (const FGameplayTag& Tag : All.GetGameplayTagArray())
		{
			if (Tag.ToString().StartsWith(TEXT("RqlSpec")))
				Names.Add(Tag.ToString());
		}
		Names.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); }); // children first
		for (const FString& Name : Names)
			Run(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"RemoveGameplayTag","args":[{"lit":"%s"}]}]})RQL"), *Name), true);
		// The spec's tag source keeps its tags and redirectors in memory after its ini is deleted: empty it.
		if (const FGameplayTagSource* Source = Manager.FindTagSource(TEXT("RqlSpecTags.ini")))
		{
			if (UGameplayTagsList* List = Source->SourceTagList)
			{
				List->GameplayTagList.Reset();
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
				List->GameplayTagRedirects.Reset(); // per-source redirectors (UE 5.6+; before, only the settings have them)
#endif
			}
		}
		// RenameGameplayTag may leave redirectors in the project's GameplayTags settings too: drop the specs' ones.
		UGameplayTagsSettings* Settings = GetMutableDefault<UGameplayTagsSettings>();
		const int32 Redirects = Settings->GameplayTagRedirects.RemoveAll([](const FGameplayTagRedirect& Redirect)
		{
			return Redirect.OldTagName.ToString().StartsWith(TEXT("RqlSpec"));
		});
		if (Redirects > 0)
			Settings->TryUpdateDefaultConfigFile();
		if (!bTagsSettingsIniExisted)
			IFileManager::Get().Delete(*FPaths::ConvertRelativePathToFull(Settings->GetDefaultConfigFilename()), false, true);
		if (IFileManager::Get().FileExists(*TagIni()))
			IFileManager::Get().Delete(*TagIni(), false, true);
		Manager.EditorRefreshGameplayTagTree();
	}

END_DEFINE_SPEC(FUnrealMcpRqlDataModelsSpec)

void FUnrealMcpRqlDataModelsSpec::Define()
{
	BeforeEach([this]()
	{
		++Serial;
		Table = FString::Printf(TEXT("/Game/RqlSpecData/DT_Spec%d_%d"), Serial, FMath::Rand());
		Blueprint = FString::Printf(TEXT("/Game/RqlSpecData/BP_Spec%d_%d"), Serial, FMath::Rand());
		WidgetBlueprintPath = FString::Printf(TEXT("/Game/RqlSpecData/WBP_Spec%d_%d"), Serial, FMath::Rand());
		SequencePath = FString::Printf(TEXT("/Game/RqlSpecData/LS_Spec%d_%d"), Serial, FMath::Rand());
	});
	AfterEach([this]()
	{
		// Actors first: placed instances of a spec Blueprint must not outlive their class (a garbage class with live
		// instances breaks the next garbage collection).
		DestroySpecActors();
		for (UObject* Object : Created)
			UnrealMcpRqlSpecHelpers::DropObject(Object);
		Created.Reset();
		if (UDataTable* Made = FindTable())
			UnrealMcpRqlSpecHelpers::DropObject(Made);
		if (GEditor)
			GEditor->ResetTransaction(FText::FromString(TEXT("RQL data model specs")));
	});

	Describe("DataTables", [this]()
	{
		It("creates a table, adds, edits, renames, duplicates and removes rows", [this]()
		{
			Refuses(TEXT("not a row struct"), /* editor | call CreateDataTable("@DT@", "Vector") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"Vector"}]}]})RQL"), TEXT("not a struct DataTable rows can use"));
			const TSharedPtr<FJsonObject> Made = Must(TEXT("table created"), /* editor | call CreateDataTable("@DT@", "UnrealMcpRqlSpecRow") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"UnrealMcpRqlSpecRow"}]}]})RQL"));
			TestTrue(TEXT("returns the table"), ResultRef(Made).StartsWith(TEXT("DataTable#")));
			UDataTable* Data = FindTable();
			if (!TestNotNull(TEXT("table"), Data))
				return;
			TestTrue(TEXT("row struct"), Data->GetRowStruct() == FUnrealMcpRqlSpecRow::StaticStruct());
			Refuses(TEXT("the path is taken"), /* editor | call CreateDataTable("@DT@", "UnrealMcpRqlSpecRow") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"UnrealMcpRqlSpecRow"}]}]})RQL"), TEXT("already exists"));

			int32 Changes = 0;
			const FDelegateHandle Handle = Data->OnDataTableChanged().AddLambda([&Changes]() { ++Changes; });
			ON_SCOPE_EXIT { Data->OnDataTableChanged().Remove(Handle); };

			Must(TEXT("row added with values"), /* unreal.asset("@DT@") | call AddRow("Sword", {Damage: 12, Label: "steel", Offset: "(1, 2, 3)", Mode: "Run"}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Sword"},{"object":[{"key":"Damage","value":{"lit":12}},{"key":"Label","value":{"lit":"steel"}},{"key":"Offset","value":{"lit":"(1, 2, 3)"}},{"key":"Mode","value":{"lit":"Run"}}]}]}]})RQL"));
			const FUnrealMcpRqlSpecRow* Sword = Data->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Sword"), TEXT("spec"));
			TestTrue(TEXT("row values"), Sword && Sword->Damage == 12 && Sword->Label == TEXT("steel") && Sword->Offset.Equals(FVector(1, 2, 3))
				&& Sword->Mode == EUnrealMcpRqlSpecMode::Run);
			Must(TEXT("row added with defaults"), /* unreal.asset("@DT@") | call AddRow("Shield") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Shield"}]}]})RQL"));
			const TSharedPtr<FJsonObject> Info = Run(/* unreal.asset("@DT@") | select RowNames RowCount RowStructName */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"select","fields":[{"name":"RowNames","expr":{"path":["RowNames"]}},{"name":"RowCount","expr":{"path":["RowCount"]}},{"name":"RowStructName","expr":{"path":["RowStructName"]}}]}]})RQL"));
			TestEqual(TEXT("row names"), UnrealMcpRqlSpecHelpers::CellText(Info, 0, 0), FString(TEXT("[\"Sword\",\"Shield\"]")));
			TestEqual(TEXT("row count"), UnrealMcpRqlSpecHelpers::CellText(Info, 0, 1), FString(TEXT("2")));
			TestEqual(TEXT("row struct name"), UnrealMcpRqlSpecHelpers::CellText(Info, 0, 2), FString(TEXT("UnrealMcpRqlSpecRow")));

			const TSharedPtr<FJsonObject> Read = Run(/* unreal.asset("@DT@") | select Rows["Sword"].Damage Rows["sword"].Offset */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"select","fields":[{"name":"Rows[\"Sword\"].Damage","expr":{"path":["Rows",{"key":"Sword"},"Damage"]}},{"name":"Rows[\"sword\"].Offset","expr":{"path":["Rows",{"key":"sword"},"Offset"]}}]}]})RQL"));
			TestEqual(TEXT("a row member"), UnrealMcpRqlSpecHelpers::CellText(Read, 0, 0), FString(TEXT("12")));
			TestEqual(TEXT("row names are case-insensitive"), UnrealMcpRqlSpecHelpers::CellText(Read, 0, 1), FString(TEXT("(1, 2, 3)")));
			const int32 Before = Changes;
			Must(TEXT("row member written in place"), /* unreal.asset("@DT@") | set Rows["Sword"].Damage = Rows["Sword"].Damage + 8, Rows["Sword"].Offset.Z = 30 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Rows",{"key":"Sword"},"Damage"]},"value":{"op":"+","args":[{"path":["Rows",{"key":"Sword"},"Damage"]},{"lit":8}]}},{"target":{"path":["Rows",{"key":"Sword"},"Offset","Z"]},"value":{"lit":30}}]}]})RQL"));
			Sword = Data->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Sword"), TEXT("spec"));
			TestTrue(TEXT("written"), Sword && Sword->Damage == 20 && FMath::IsNearlyEqual(Sword->Offset.Z, 30.0));
			TestTrue(TEXT("the table was notified"), Changes > Before);
			TestTrue(TEXT("the package is dirty"), Data->GetOutermost()->IsDirty());

			const TSharedPtr<FJsonObject> Strong = Run(/* unreal.asset("@DT@") | get Rows | where Value.Damage > 10 | select Key */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"get","expr":{"path":["Rows"]}},{"op":"where","pred":{"op":">","args":[{"path":["Value","Damage"]},{"lit":10}]}},{"op":"select","fields":[{"name":"Key","expr":{"path":["Key"]}}]}]})RQL"));
			TestEqual(TEXT("rows stream as entries"), UnrealMcpRqlSpecHelpers::RowCount(Strong), 1);
			TestEqual(TEXT("the strong one"), UnrealMcpRqlSpecHelpers::CellText(Strong, 0, 0), FString(TEXT("Sword")));

			Must(TEXT("a new key adds a row"), /* unreal.asset("@DT@") | set Rows["Bow"] = {Damage: 7, Label: "yew"} */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Rows",{"key":"Bow"}]},"value":{"object":[{"key":"Damage","value":{"lit":7}},{"key":"Label","value":{"lit":"yew"}}]}}]}]})RQL"));
			TestTrue(TEXT("the new row"), Data->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Bow"), TEXT("spec")) != nullptr);
			Must(TEXT("renamed"), /* unreal.asset("@DT@") | call RenameRow("Shield", "Buckler") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"RenameRow","args":[{"lit":"Shield"},{"lit":"Buckler"}]}]})RQL"));
			Must(TEXT("duplicated"), /* unreal.asset("@DT@") | call DuplicateRow("Sword", "Sword2") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"DuplicateRow","args":[{"lit":"Sword"},{"lit":"Sword2"}]}]})RQL"));
			const FUnrealMcpRqlSpecRow* Copy = Data->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Sword2"), TEXT("spec"));
			TestTrue(TEXT("the copy has the values"), Copy && Copy->Damage == 20);
			Must(TEXT("removed"), /* unreal.asset("@DT@") | call RemoveRow("Sword2") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"RemoveRow","args":[{"lit":"Sword2"}]}]})RQL"));
			TestEqual(TEXT("rows now"), Data->GetRowNames(), TArray<FName>({ TEXT("Sword"), TEXT("Buckler"), TEXT("Bow") }));
			const TSharedPtr<FJsonObject> Listed = Run(/* dataTables | where Path contains "@DT@" | select RowCount */ TEXT(R"RQL({"rql":"0.1","source":{"path":["dataTables"]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Path"]},{"lit":"@DT@"}]}},{"op":"select","fields":[{"name":"RowCount","expr":{"path":["RowCount"]}}]}]})RQL"));
			TestEqual(TEXT("dataTables source"), UnrealMcpRqlSpecHelpers::CellText(Listed), FString(TEXT("3")));
		});

		It("validates rows and values, leaving the table as it was", [this]()
		{
			Must(TEXT("table"), /* editor | call CreateDataTable("@DT@", "UnrealMcpRqlSpecRow") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"UnrealMcpRqlSpecRow"}]}]})RQL"));
			Must(TEXT("row"), /* unreal.asset("@DT@") | call AddRow("Sword", {Damage: 1}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Sword"},{"object":[{"key":"Damage","value":{"lit":1}}]}]}]})RQL"));
			UDataTable* Data = FindTable();
			if (!TestNotNull(TEXT("table"), Data))
				return;
			Refuses(TEXT("duplicate row"), /* unreal.asset("@DT@") | call AddRow("sword") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"sword"}]}]})RQL"), TEXT("already has a row"));
			Refuses(TEXT("invalid name"), /* unreal.asset("@DT@") | call AddRow("bad name") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"bad name"}]}]})RQL"), TEXT("not a valid row name"));
			Refuses(TEXT("empty name"), /* unreal.asset("@DT@") | call AddRow("  ") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"  "}]}]})RQL"), TEXT("cannot be empty"));
			Refuses(TEXT("values that do not fit"), /* unreal.asset("@DT@") | call AddRow("Axe", {Nope: 1}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Axe"},{"object":[{"key":"Nope","value":{"lit":1}}]}]}]})RQL"), TEXT("no member 'Nope'"));
			TestTrue(TEXT("no half-made row"), Data->FindRowUnchecked(TEXT("Axe")) == nullptr);
			Refuses(TEXT("a wrong member type"), /* unreal.asset("@DT@") | set Rows["Sword"].Damage = "lots" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Rows",{"key":"Sword"},"Damage"]},"value":{"lit":"lots"}}]}]})RQL"), TEXT("Damage"));
			Refuses(TEXT("an unknown row"), /* unreal.asset("@DT@") | call RemoveRow("Nope") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"RemoveRow","args":[{"lit":"Nope"}]}]})RQL"), TEXT("has no row 'Nope'"));
			Must(TEXT("second row"), /* unreal.asset("@DT@") | call AddRow("Shield") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Shield"}]}]})RQL"));
			Refuses(TEXT("rename onto an existing row"), /* unreal.asset("@DT@") | call RenameRow("Sword", "Shield") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"RenameRow","args":[{"lit":"Sword"},{"lit":"Shield"}]}]})RQL"), TEXT("already has a row"));
			TestEqual(TEXT("values kept"), Data->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Sword"), TEXT("spec"))->Damage, 1);
		});

		It("undoes row edits and row adds, one step per mutation", [this]()
		{
			if (!GEditor)
				return;
			Must(TEXT("table"), /* editor | call CreateDataTable("@DT@", "UnrealMcpRqlSpecRow") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"UnrealMcpRqlSpecRow"}]}]})RQL"));
			UDataTable* Data = FindTable();
			if (!TestNotNull(TEXT("table"), Data))
				return;
			Must(TEXT("row"), /* unreal.asset("@DT@") | call AddRow("Sword", {Damage: 1}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Sword"},{"object":[{"key":"Damage","value":{"lit":1}}]}]}]})RQL"));
			Must(TEXT("edit"), /* unreal.asset("@DT@") | set Rows["Sword"].Damage = 99 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Rows",{"key":"Sword"},"Damage"]},"value":{"lit":99}}]}]})RQL"));
			TestEqual(TEXT("edited"), Data->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Sword"), TEXT("spec"))->Damage, 99);
			GEditor->UndoTransaction();
			const FUnrealMcpRqlSpecRow* Row = Data->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Sword"), TEXT("spec"));
			TestTrue(TEXT("edit undone"), Row && Row->Damage == 1);
			GEditor->UndoTransaction();
			TestTrue(TEXT("add undone"), Data->FindRowUnchecked(TEXT("Sword")) == nullptr);
			GEditor->RedoTransaction();
			TestTrue(TEXT("add redone"), Data->FindRowUnchecked(TEXT("Sword")) != nullptr);
		});

		// RQL main 602ce7c: a set on the entries `get Rows` streams writes through the table (plans: Rql.Parser 0.3.0-dev.602ce7c).
		It("writes rows streamed as entries through the table: dry run, write, notification, undo", [this]()
		{
			Must(TEXT("table"), /* editor | call CreateDataTable("@DT@", "UnrealMcpRqlSpecRow") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"UnrealMcpRqlSpecRow"}]}]})RQL"));
			UDataTable* Data = FindTable();
			if (!TestNotNull(TEXT("table"), Data))
				return;
			Must(TEXT("strong row"), /* unreal.asset("@DT@") | call AddRow("Sword", {Damage: 12}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Sword"},{"object":[{"key":"Damage","value":{"lit":12}}]}]}]})RQL"));
			Must(TEXT("weak row"), /* unreal.asset("@DT@") | call AddRow("Shield", {Damage: 1}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Shield"},{"object":[{"key":"Damage","value":{"lit":1}}]}]}]})RQL"));
			auto Damage = [Data](const TCHAR* Row)
			{
				const FUnrealMcpRqlSpecRow* Found = Data->FindRow<FUnrealMcpRqlSpecRow>(Row, TEXT("spec"));
				return Found ? Found->Damage : -1;
			};
			const FString Double = /* unreal.asset("@DT@") | get Rows | where Value.Damage > 10 | set Value.Damage = Value.Damage * 2 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"get","expr":{"path":["Rows"]}},{"op":"where","pred":{"op":">","args":[{"path":["Value","Damage"]},{"lit":10}]}},{"op":"set","assign":[{"target":{"path":["Value","Damage"]},"value":{"op":"*","args":[{"path":["Value","Damage"]},{"lit":2}]}}]}]})RQL");

			const TSharedPtr<FJsonObject> Dry = UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(Double, { { TEXT("DT"), Table } }), true, /*bDryRun*/ true);
			TestTrue(*FString::Printf(TEXT("dry run ok (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Dry)), UnrealMcpRqlSpecHelpers::IsOk(Dry));
			TestEqual(TEXT("dry run: one receipt row"), UnrealMcpRqlSpecHelpers::RowCount(Dry), 1);
			TestEqual(TEXT("dry run changed nothing"), Damage(TEXT("Sword")), 12);

			int32 Changes = 0;
			const FDelegateHandle Handle = Data->OnDataTableChanged().AddLambda([&Changes]() { ++Changes; });
			ON_SCOPE_EXIT { Data->OnDataTableChanged().Remove(Handle); };
			Data->GetOutermost()->SetDirtyFlag(false); // CreateDataTable / AddRow dirtied it: the entry write must do it again
			const TSharedPtr<FJsonObject> Written = Must(TEXT("entries written"), Double);
			TestTrue(*FString::Printf(TEXT("the receipt names the table (%s)"), *UnrealMcpRqlSpecHelpers::CellText(Written, 0, 0)),
				UnrealMcpRqlSpecHelpers::CellText(Written, 0, 0).StartsWith(TEXT("DataTable#")));
			TestTrue(TEXT("the matching row doubled, the other kept"), Damage(TEXT("Sword")) == 24 && Damage(TEXT("Shield")) == 1);
			TestTrue(TEXT("the table was notified"), Changes > 0);
			TestTrue(TEXT("the package is dirty"), Data->GetOutermost()->IsDirty());
			if (GEditor)
			{
				GEditor->UndoTransaction();
				TestEqual(TEXT("one undo step restores the row"), Damage(TEXT("Sword")), 12);
			}
		});

		It("saves rows to disk: a reloaded table has them", [this]()
		{
			Must(TEXT("table"), /* editor | call CreateDataTable("@DT@", "UnrealMcpRqlSpecRow") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"UnrealMcpRqlSpecRow"}]}]})RQL"));
			Must(TEXT("row"), /* unreal.asset("@DT@") | call AddRow("Sword", {Damage: 42, Label: "saved"}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Sword"},{"object":[{"key":"Damage","value":{"lit":42}},{"key":"Label","value":{"lit":"saved"}}]}]}]})RQL"));
			Must(TEXT("saved"), /* editor | call SaveAsset(unreal.asset("@DT@")) */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SaveAsset","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]}]}]})RQL"));
			const FString File = FPackageName::LongPackageNameToFilename(Table, FPackageName::GetAssetPackageExtension());
			TestTrue(TEXT("the file exists"), IFileManager::Get().FileExists(*File));
			UDataTable* Data = FindTable();
			UPackage* Package = Data ? Data->GetOutermost() : nullptr;
			if (!TestNotNull(TEXT("package"), Package))
				return;
			FText Error;
			UPackageTools::ReloadPackages({ Package }, Error, EReloadPackagesInteractionMode::AssumePositive);
			UDataTable* Reloaded = FindTable();
			const FUnrealMcpRqlSpecRow* Row = Reloaded ? Reloaded->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Sword"), TEXT("spec")) : nullptr;
			TestTrue(TEXT("the reloaded row"), Row && Row->Damage == 42 && Row->Label == TEXT("saved"));
			const TSharedPtr<FJsonObject> Read = Run(/* unreal.asset("@DT@") | select Rows["Sword"].Label */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"select","fields":[{"name":"Rows[\"Sword\"].Label","expr":{"path":["Rows",{"key":"Sword"},"Label"]}}]}]})RQL"));
			TestEqual(TEXT("RQL reads the reloaded table"), UnrealMcpRqlSpecHelpers::CellText(Read), FString(TEXT("saved")));
			Must(TEXT("deleted"), /* editor | call DeleteAsset("@DT@", true) */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"DeleteAsset","args":[{"lit":"@DT@"},{"lit":true}]}]})RQL"));
			TestFalse(TEXT("the file is gone"), IFileManager::Get().FileExists(*File));
		});
	});

	Describe("user-defined structs", [this]()
	{
		It("adds, removes, renames fields and sets defaults; a DataTable of the struct uses them", [this]()
		{
			const FString Struct = Table + TEXT("_Row");
			const TPair<const TCHAR*, FString> Path(TEXT("UDS"), Struct);
			UPackage* Package = CreatePackage(*Struct);
			UUserDefinedStruct* Made = FStructureEditorUtils::CreateUserDefinedStruct(Package, *FPackageName::GetShortName(Struct), RF_Public | RF_Standalone | RF_Transactional);
			if (!TestNotNull(TEXT("struct"), Made))
				return;
			FAssetRegistryModule::AssetCreated(Made);
			Created.Add(Made);
			const FString Initial = FStructureEditorUtils::GetVarDesc(Made)[0].FriendlyName;

			Must(TEXT("a float field"), /* unreal.asset("@UDS@") | call AddField("Health", "float", 100) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"call","method":"AddField","args":[{"lit":"Health"},{"lit":"float"},{"lit":100}]}]})RQL"), { Path });
			Must(TEXT("an int field"), /* unreal.asset("@UDS@") | call AddField("Tier", "int") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"call","method":"AddField","args":[{"lit":"Tier"},{"lit":"int"}]}]})RQL"), { Path });
			Must(TEXT("the initial field removed"), /* unreal.asset("@UDS@") | call RemoveField("@INITIAL@") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"call","method":"RemoveField","args":[{"lit":"@INITIAL@"}]}]})RQL"), { Path, { TEXT("INITIAL"), Initial } });
			Must(TEXT("renamed"), /* unreal.asset("@UDS@") | call RenameField("Tier", "Level") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"call","method":"RenameField","args":[{"lit":"Tier"},{"lit":"Level"}]}]})RQL"), { Path });
			Must(TEXT("default"), /* unreal.asset("@UDS@") | call SetFieldDefault("Level", 3) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"call","method":"SetFieldDefault","args":[{"lit":"Level"},{"lit":3}]}]})RQL"), { Path });
			Refuses(TEXT("unknown type"), /* unreal.asset("@UDS@") | call AddField("Speed", "NoSuchType") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"call","method":"AddField","args":[{"lit":"Speed"},{"lit":"NoSuchType"}]}]})RQL"), TEXT("unknown variable type"), { Path });
			Refuses(TEXT("a taken name"), /* unreal.asset("@UDS@") | call AddField("health", "int") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"call","method":"AddField","args":[{"lit":"health"},{"lit":"int"}]}]})RQL"), TEXT("already has a field"), { Path });
			Refuses(TEXT("an unknown field"), /* unreal.asset("@UDS@") | call RemoveField("Nope") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"call","method":"RemoveField","args":[{"lit":"Nope"}]}]})RQL"), TEXT("fields: Health, Level"), { Path });
			const FString Fields = UnrealMcpRqlSpecHelpers::CellText(Run(/* unreal.asset("@UDS@") | select Fields */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@UDS@"}]},"stages":[{"op":"select","fields":[{"name":"Fields","expr":{"path":["Fields"]}}]}]})RQL"), false, { Path }));
			TestTrue(*FString::Printf(TEXT("fields (%s)"), *Fields), Fields.Contains(TEXT("\"Name\":\"Health\",\"Type\":\"float\""))
				&& Fields.Contains(TEXT("\"Name\":\"Level\",\"Type\":\"int\",\"Default\":\"3\"")) && !Fields.Contains(Initial));

			Must(TEXT("a table of the struct"), /* editor | call CreateDataTable("@DT@", "@UDS@") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"@UDS@"}]}]})RQL"), { Path });
			Must(TEXT("a row with the fields"), /* unreal.asset("@DT@") | call AddRow("Orc", {Health: 120, Level: 2}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Orc"},{"object":[{"key":"Health","value":{"lit":120}},{"key":"Level","value":{"lit":2}}]}]}]})RQL"), { Path });
			const TSharedPtr<FJsonObject> Rows = Run(/* unreal.asset("@DT@") | get Rows | select Key Value.Health Value.Level */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"get","expr":{"path":["Rows"]}},{"op":"select","fields":[{"name":"Key","expr":{"path":["Key"]}},{"name":"Value.Health","expr":{"path":["Value","Health"]}},{"name":"Value.Level","expr":{"path":["Value","Level"]}}]}]})RQL"));
			TestEqual(TEXT("row"), UnrealMcpRqlSpecHelpers::CellText(Rows, 0, 0), FString(TEXT("Orc")));
			TestEqual(TEXT("health"), UnrealMcpRqlSpecHelpers::CellText(Rows, 0, 1), FString(TEXT("120")));
			TestEqual(TEXT("level"), UnrealMcpRqlSpecHelpers::CellText(Rows, 0, 2), FString(TEXT("2")));
		});
	});

	Describe("gameplay tags", [this]()
	{
		BeforeEach([this]()
		{
			bTagsSettingsIniExisted = IFileManager::Get().FileExists(*FPaths::ConvertRelativePathToFull(GetDefault<UGameplayTagsSettings>()->GetDefaultConfigFilename()));
		});
		AfterEach([this]() { DropTags(); });

		It("adds, lists, renames and removes tags in a tag ini", [this]()
		{
			DropTags();
			Must(TEXT("added"), /* editor | call AddGameplayTag("RqlSpec.Weapon.Sword", "a sword", "RqlSpecTags.ini") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.Weapon.Sword"},{"lit":"a sword"},{"lit":"RqlSpecTags.ini"}]}]})RQL"));
			Must(TEXT("added a second"), /* editor | call AddGameplayTag("RqlSpec.State.Stunned", "", "RqlSpecTags.ini") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.State.Stunned"},{"lit":""},{"lit":"RqlSpecTags.ini"}]}]})RQL"));
			TestTrue(TEXT("registered"), UGameplayTagsManager::Get().RequestGameplayTag(TEXT("RqlSpec.Weapon.Sword"), false).IsValid());
			TestTrue(TEXT("written to the ini"), FileText(TagIni()).Contains(TEXT("RqlSpec.Weapon.Sword")));
			const TSharedPtr<FJsonObject> Row = Run(/* gameplayTags | where Name == "RqlSpec.Weapon.Sword" | select Comment Source Parent Explicit Children */ TEXT(R"RQL({"rql":"0.1","source":{"path":["gameplayTags"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSpec.Weapon.Sword"}]}},{"op":"select","fields":[{"name":"Comment","expr":{"path":["Comment"]}},{"name":"Source","expr":{"path":["Source"]}},{"name":"Parent","expr":{"path":["Parent"]}},{"name":"Explicit","expr":{"path":["Explicit"]}},{"name":"Children","expr":{"path":["Children"]}}]}]})RQL"));
			TestEqual(TEXT("comment"), UnrealMcpRqlSpecHelpers::CellText(Row, 0, 0), FString(TEXT("a sword")));
			TestEqual(TEXT("source"), UnrealMcpRqlSpecHelpers::CellText(Row, 0, 1), FString(TEXT("RqlSpecTags.ini")));
			TestEqual(TEXT("parent"), UnrealMcpRqlSpecHelpers::CellText(Row, 0, 2), FString(TEXT("RqlSpec.Weapon")));
			TestEqual(TEXT("explicit"), UnrealMcpRqlSpecHelpers::CellText(Row, 0, 3), FString(TEXT("true")));
			const TSharedPtr<FJsonObject> Parent = Run(/* gameplayTags | where Name == "RqlSpec.Weapon" | select Explicit Children */ TEXT(R"RQL({"rql":"0.1","source":{"path":["gameplayTags"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSpec.Weapon"}]}},{"op":"select","fields":[{"name":"Explicit","expr":{"path":["Explicit"]}},{"name":"Children","expr":{"path":["Children"]}}]}]})RQL"));
			TestEqual(TEXT("the implicit parent"), UnrealMcpRqlSpecHelpers::CellText(Parent, 0, 0), FString(TEXT("false")));
			TestEqual(TEXT("its child"), UnrealMcpRqlSpecHelpers::CellText(Parent, 0, 1), FString(TEXT("1")));

			Refuses(TEXT("duplicate"), /* editor | call AddGameplayTag("RqlSpec.Weapon.Sword") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.Weapon.Sword"}]}]})RQL"), TEXT("already exists"));
			Refuses(TEXT("invalid"), /* editor | call AddGameplayTag("RqlSpec.Bad,Tag") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.Bad,Tag"}]}]})RQL"), TEXT("not a valid gameplay tag"));
			Must(TEXT("renamed"), /* editor | call RenameGameplayTag("RqlSpec.Weapon.Sword", "RqlSpec.Weapon.Blade") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"RenameGameplayTag","args":[{"lit":"RqlSpec.Weapon.Sword"},{"lit":"RqlSpec.Weapon.Blade"}]}]})RQL"));
			TestTrue(TEXT("the new name"), UGameplayTagsManager::Get().IsDictionaryTag(TEXT("RqlSpec.Weapon.Blade")));
			Refuses(TEXT("rename onto a tag"), /* editor | call RenameGameplayTag("RqlSpec.Weapon.Blade", "RqlSpec.State.Stunned") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"RenameGameplayTag","args":[{"lit":"RqlSpec.Weapon.Blade"},{"lit":"RqlSpec.State.Stunned"}]}]})RQL"), TEXT("already exists"));
			Must(TEXT("removed"), /* editor | call RemoveGameplayTag("RqlSpec.State.Stunned") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"RemoveGameplayTag","args":[{"lit":"RqlSpec.State.Stunned"}]}]})RQL"));
			TestFalse(TEXT("gone"), UGameplayTagsManager::Get().IsDictionaryTag(TEXT("RqlSpec.State.Stunned")));
			Refuses(TEXT("empty segment"), /* editor | call AddGameplayTag("RqlSpec..Bad", "", "RqlSpecTags.ini") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec..Bad"},{"lit":""},{"lit":"RqlSpecTags.ini"}]}]})RQL"), TEXT("empty segment"));
			Refuses(TEXT("remove an unknown tag"), /* editor | call RemoveGameplayTag("RqlSpec.Nothing") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"RemoveGameplayTag","args":[{"lit":"RqlSpec.Nothing"}]}]})RQL"), TEXT("is not a gameplay tag"));
		});

		It("gives an implicit parent tag no comment (not the comment of the child that created it)", [this]()
		{
			DropTags();
			Must(TEXT("child"), /* editor | call AddGameplayTag("RqlSpec.Implicit.Child", "only the child", "RqlSpecTags.ini") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.Implicit.Child"},{"lit":"only the child"},{"lit":"RqlSpecTags.ini"}]}]})RQL"));
			const TSharedPtr<FJsonObject> Parent = Run(/* gameplayTags | where Name == "RqlSpec.Implicit" | select Comment Explicit */ TEXT(R"RQL({"rql":"0.1","source":{"path":["gameplayTags"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSpec.Implicit"}]}},{"op":"select","fields":[{"name":"Comment","expr":{"path":["Comment"]}},{"name":"Explicit","expr":{"path":["Explicit"]}}]}]})RQL"));
			TestEqual(TEXT("no comment"), UnrealMcpRqlSpecHelpers::CellText(Parent, 0, 0), FString());
			TestEqual(TEXT("implicit"), UnrealMcpRqlSpecHelpers::CellText(Parent, 0, 1), FString(TEXT("false")));
		});

		It("renames a tag with children: the renamed tag stays explicit with its comment", [this]()
		{
			DropTags();
			Must(TEXT("parent"), /* editor | call AddGameplayTag("RqlSpec.Gear", "gear", "RqlSpecTags.ini") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.Gear"},{"lit":"gear"},{"lit":"RqlSpecTags.ini"}]}]})RQL"));
			Must(TEXT("child"), /* editor | call AddGameplayTag("RqlSpec.Gear.Helmet", "", "RqlSpecTags.ini") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.Gear.Helmet"},{"lit":""},{"lit":"RqlSpecTags.ini"}]}]})RQL"));
			Must(TEXT("renamed"), /* editor | call RenameGameplayTag("RqlSpec.Gear", "RqlSpec.Kit") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"RenameGameplayTag","args":[{"lit":"RqlSpec.Gear"},{"lit":"RqlSpec.Kit"}]}]})RQL"));
			const TSharedPtr<FJsonObject> Rows = Run(/* gameplayTags | where Name starts-with "RqlSpec.Kit" | select Name Explicit Comment */ TEXT(R"RQL({"rql":"0.1","source":{"path":["gameplayTags"]},"stages":[{"op":"where","pred":{"op":"starts-with","args":[{"path":["Name"]},{"lit":"RqlSpec.Kit"}]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Explicit","expr":{"path":["Explicit"]}},{"name":"Comment","expr":{"path":["Comment"]}}]}]})RQL"));
			TestEqual(TEXT("the tag and its child"), UnrealMcpRqlSpecHelpers::RowCount(Rows), 2);
			TestEqual(TEXT("the renamed tag"), UnrealMcpRqlSpecHelpers::CellText(Rows, 0, 0), FString(TEXT("RqlSpec.Kit")));
			TestEqual(TEXT("still explicit"), UnrealMcpRqlSpecHelpers::CellText(Rows, 0, 1), FString(TEXT("true")));
			TestEqual(TEXT("comment kept"), UnrealMcpRqlSpecHelpers::CellText(Rows, 0, 2), FString(TEXT("gear")));
			TestEqual(TEXT("the renamed child"), UnrealMcpRqlSpecHelpers::CellText(Rows, 1, 0), FString(TEXT("RqlSpec.Kit.Helmet")));
			// The old name stays resolvable through the redirector the rename leaves; its entry is gone.
			TestFalse(TEXT("the old entry is gone"), FileText(TagIni()).Contains(TEXT("Tag=\"RqlSpec.Gear\"")));
			TestTrue(TEXT("written to the ini"), FileText(TagIni()).Contains(TEXT("Tag=\"RqlSpec.Kit\"")));
		});

		It("reads and writes tag properties as text, validated and with parent tags", [this]()
		{
			DropTags();
			Must(TEXT("tag"), /* editor | call AddGameplayTag("RqlSpec.Weapon.Sword", "", "RqlSpecTags.ini") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.Weapon.Sword"},{"lit":""},{"lit":"RqlSpecTags.ini"}]}]})RQL"));
			Must(TEXT("tag"), /* editor | call AddGameplayTag("RqlSpec.Weapon.Bow", "", "RqlSpecTags.ini") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"AddGameplayTag","args":[{"lit":"RqlSpec.Weapon.Bow"},{"lit":""},{"lit":"RqlSpecTags.ini"}]}]})RQL"));
			UPackage* Package = CreatePackage(TEXT("/Game/RqlSpecData/TagFixture"));
			UUnrealMcpRqlSpecFixture* Fixture = NewObject<UUnrealMcpRqlSpecFixture>(Package, TEXT("TagFixture"), RF_Public | RF_Standalone | RF_Transactional);
			Created.Add(Fixture);
			const FString Path = TEXT("/Game/RqlSpecData/TagFixture.TagFixture");

			Must(TEXT("a tag from text"), /* unreal.asset("@O@") | set Tag = "RqlSpec.Weapon.Sword" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tag"]},"value":{"lit":"RqlSpec.Weapon.Sword"}}]}]})RQL"), { { TEXT("O"), Path } });
			TestTrue(TEXT("stored"), Fixture->Tag == FGameplayTag::RequestGameplayTag(TEXT("RqlSpec.Weapon.Sword")));
			const TSharedPtr<FJsonObject> Read = Run(/* unreal.asset("@O@") | select Tag Tag.Parent */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"select","fields":[{"name":"Tag","expr":{"path":["Tag"]}},{"name":"Tag.Parent","expr":{"path":["Tag","Parent"]}}]}]})RQL"), false, { { TEXT("O"), Path } });
			TestEqual(TEXT("reads as text"), UnrealMcpRqlSpecHelpers::CellText(Read, 0, 0), FString(TEXT("RqlSpec.Weapon.Sword")));
			TestEqual(TEXT("its parent"), UnrealMcpRqlSpecHelpers::CellText(Read, 0, 1), FString(TEXT("RqlSpec.Weapon")));
			const TSharedPtr<FJsonObject> Match = Run(/* unreal.asset("@O@") | where Tag == "rqlspec.weapon.sword" | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Tag"]},{"lit":"rqlspec.weapon.sword"}]}},{"op":"count"}]})RQL"), false, { { TEXT("O"), Path } });
			TestEqual(TEXT("compares with text"), UnrealMcpRqlSpecHelpers::Number(Match), 1.0);
			Refuses(TEXT("an unknown tag"), /* unreal.asset("@O@") | set Tag = "RqlSpec.Weapon.Swrd" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tag"]},"value":{"lit":"RqlSpec.Weapon.Swrd"}}]}]})RQL"), TEXT("not a registered gameplay tag"), { { TEXT("O"), Path } });
			Refuses(TEXT("suggests similar tags"), /* unreal.asset("@O@") | set Tag = "Sword" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tag"]},"value":{"lit":"Sword"}}]}]})RQL"), TEXT("similar: RqlSpec.Weapon.Sword"), { { TEXT("O"), Path } });
			Refuses(TEXT("the raw name is not writable"), /* unreal.asset("@O@") | set Tag.Name = "X" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tag","Name"]},"value":{"lit":"X"}}]}]})RQL"), TEXT("readonly-member"), { { TEXT("O"), Path } });
			Refuses(TEXT("null is not a tag (structs are never null)"), /* unreal.asset("@O@") | set Tag = null */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tag"]},"value":{"lit":null}}]}]})RQL"), TEXT("null is not allowed"), { { TEXT("O"), Path } });
			Must(TEXT("empty text clears"), /* unreal.asset("@O@") | set Tag = "" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tag"]},"value":{"lit":""}}]}]})RQL"), { { TEXT("O"), Path } });
			TestFalse(TEXT("no tag"), Fixture->Tag.IsValid());

			Must(TEXT("a container from a list"), /* unreal.asset("@O@") | set Tags = ["RqlSpec.Weapon.Sword", "RqlSpec.Weapon.Bow"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tags"]},"value":{"list":[{"lit":"RqlSpec.Weapon.Sword"},{"lit":"RqlSpec.Weapon.Bow"}]}}]}]})RQL"), { { TEXT("O"), Path } });
			TestEqual(TEXT("two tags"), Fixture->Tags.Num(), 2);
			TestTrue(TEXT("parent tags are cached (HasTag of the parent)"), Fixture->Tags.HasTag(FGameplayTag::RequestGameplayTag(TEXT("RqlSpec.Weapon"))));
			const TSharedPtr<FJsonObject> Names = Run(/* unreal.asset("@O@") | select Tags Tags.Num */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"select","fields":[{"name":"Tags","expr":{"path":["Tags"]}},{"name":"Tags.Num","expr":{"path":["Tags","Num"]}}]}]})RQL"), false, { { TEXT("O"), Path } });
			TestEqual(TEXT("reads as a list"), UnrealMcpRqlSpecHelpers::CellText(Names, 0, 0), FString(TEXT("[\"RqlSpec.Weapon.Sword\",\"RqlSpec.Weapon.Bow\"]")));
			TestEqual(TEXT("count"), UnrealMcpRqlSpecHelpers::CellText(Names, 0, 1), FString(TEXT("2")));
			const TSharedPtr<FJsonObject> Equal = Run(/* unreal.asset("@O@") | where Tags == ["RqlSpec.Weapon.Bow", "RqlSpec.Weapon.Sword"] | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Tags"]},{"list":[{"lit":"RqlSpec.Weapon.Bow"},{"lit":"RqlSpec.Weapon.Sword"}]}]}},{"op":"count"}]})RQL"), false, { { TEXT("O"), Path } });
			TestEqual(TEXT("set equality in any order"), UnrealMcpRqlSpecHelpers::Number(Equal), 1.0);
			const TSharedPtr<FJsonObject> Contains = Run(/* unreal.asset("@O@") | where Tags.Names contains "RqlSpec.Weapon.Bow" | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Tags","Names"]},{"lit":"RqlSpec.Weapon.Bow"}]}},{"op":"count"}]})RQL"), false, { { TEXT("O"), Path } });
			TestEqual(TEXT("contains on the names"), UnrealMcpRqlSpecHelpers::Number(Contains), 1.0);
			Must(TEXT("comma-separated text"), /* unreal.asset("@O@") | set Tags = "RqlSpec.Weapon.Bow" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tags"]},"value":{"lit":"RqlSpec.Weapon.Bow"}}]}]})RQL"), { { TEXT("O"), Path } });
			TestEqual(TEXT("one tag"), Fixture->Tags.Num(), 1);
			Refuses(TEXT("an unknown tag in the list"), /* unreal.asset("@O@") | set Tags = ["RqlSpec.Weapon.Bow", "RqlSpec.Nope"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Tags"]},"value":{"list":[{"lit":"RqlSpec.Weapon.Bow"},{"lit":"RqlSpec.Nope"}]}}]}]})RQL"), TEXT("RqlSpec.Nope"), { { TEXT("O"), Path } });
			TestEqual(TEXT("the container is kept"), Fixture->Tags.Num(), 1);
			const TSharedPtr<FJsonObject> Query = Run(/* unreal.asset("@O@") | select Query.Description */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"select","fields":[{"name":"Query.Description","expr":{"path":["Query","Description"]}}]}]})RQL"), false, { { TEXT("O"), Path } });
			TestTrue(TEXT("a tag query reads"), UnrealMcpRqlSpecHelpers::IsOk(Query));
			Refuses(TEXT("tag query members are read-only"), /* unreal.asset("@O@") | set Query.Description = "x" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@O@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Query","Description"]},"value":{"lit":"x"}}]}]})RQL"), TEXT("readonly-member"), { { TEXT("O"), Path } });

			// Tags inside DataTable rows (a struct member, written through the row).
			Must(TEXT("table"), /* editor | call CreateDataTable("@DT@", "UnrealMcpRqlSpecRow") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateDataTable","args":[{"lit":"@DT@"},{"lit":"UnrealMcpRqlSpecRow"}]}]})RQL"));
			Must(TEXT("tagged row"), /* unreal.asset("@DT@") | call AddRow("Tagged", {Tag: "RqlSpec.Weapon.Sword", Tags: ["RqlSpec.Weapon.Bow"]}) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"call","method":"AddRow","args":[{"lit":"Tagged"},{"object":[{"key":"Tag","value":{"lit":"RqlSpec.Weapon.Sword"}},{"key":"Tags","value":{"list":[{"lit":"RqlSpec.Weapon.Bow"}]}}]}]}]})RQL"));
			const FUnrealMcpRqlSpecRow* Row = FindTable() ? FindTable()->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Tagged"), TEXT("spec")) : nullptr;
			TestTrue(TEXT("row tags"), Row && Row->Tag.ToString() == TEXT("RqlSpec.Weapon.Sword") && Row->Tags.Num() == 1);
			Must(TEXT("row tag edited"), /* unreal.asset("@DT@") | set Rows["Tagged"].Tag = "RqlSpec.Weapon.Bow" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@DT@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Rows",{"key":"Tagged"},"Tag"]},"value":{"lit":"RqlSpec.Weapon.Bow"}}]}]})RQL"));
			Row = FindTable()->FindRow<FUnrealMcpRqlSpecRow>(TEXT("Tagged"), TEXT("spec"));
			TestTrue(TEXT("row tag written"), Row && Row->Tag.ToString() == TEXT("RqlSpec.Weapon.Bow"));
		});
	});

	Describe("settings", [this]()
	{
		BeforeEach([this]()
		{
			// The spec's settings class is a UDeveloperSettings: the editor registers those as Project Settings sections;
			// register it here when this module loaded after that pass.
			bRegisteredSettings = false;
			const TSharedPtr<FJsonObject> Listed = Run(/* settings | where Section == "UnrealMcpRqlSpec" | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["settings"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Section"]},{"lit":"UnrealMcpRqlSpec"}]}},{"op":"count"}]})RQL"));
			if (UnrealMcpRqlSpecHelpers::Number(Listed) < 1.0)
			{
				if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
				{
					Settings->RegisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("UnrealMcpRqlSpec"), FText::FromString(TEXT("Unreal MCP RQL Spec")),
						FText::FromString(TEXT("RQL spec settings")), GetMutableDefault<UUnrealMcpRqlSpecSettings>());
					bRegisteredSettings = true;
				}
			}
			IFileManager::Get().Delete(*SettingsIni(), false, true);
		});
		AfterEach([this]()
		{
			UUnrealMcpRqlSpecSettings* Defaults = GetMutableDefault<UUnrealMcpRqlSpecSettings>();
			Defaults->Level = 1;
			Defaults->Greeting = TEXT("hello");
			Defaults->Items.Reset();
			Defaults->NotSaved = 0;
			IFileManager::Get().Delete(*SettingsIni(), false, true);
			if (bRegisteredSettings)
			{
				if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
					Settings->UnregisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("UnrealMcpRqlSpec"));
			}
		});

		It("lists the auto-discovered developer settings (Physics) without a settings window having been opened", [this]()
		{
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
			const TSharedPtr<FJsonObject> Physics = Run(/* unreal.settings("Physics") | select DefaultGravityZ */ TEXT(R"RQL({"rql":"0.1","source":{"call":"settings","on":{"path":["unreal"]},"args":[{"lit":"Physics"}]},"stages":[{"op":"select","fields":[{"name":"DefaultGravityZ","expr":{"path":["DefaultGravityZ"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("the Physics section resolves (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Physics)), UnrealMcpRqlSpecHelpers::IsOk(Physics));
			const TSharedPtr<FJsonValue> Gravity = UnrealMcpRqlSpecHelpers::Cell(Physics, 0, 0);
			TestTrue(TEXT("its gravity reads"), Gravity.IsValid() && FMath::IsNearlyEqual(Gravity->AsNumber(), GetDefault<UPhysicsSettings>()->DefaultGravityZ));
#endif
		});

		It("lists Project Settings and Editor Preferences sections with their objects", [this]()
		{
			const TSharedPtr<FJsonObject> Maps = Run(/* settings | where Container == "Project" and Section == "Maps" | select Category DisplayName Object.$type */ TEXT(R"RQL({"rql":"0.1","source":{"path":["settings"]},"stages":[{"op":"where","pred":{"op":"and","args":[{"op":"==","args":[{"path":["Container"]},{"lit":"Project"}]},{"op":"==","args":[{"path":["Section"]},{"lit":"Maps"}]}]}},{"op":"select","fields":[{"name":"Category","expr":{"path":["Category"]}},{"name":"DisplayName","expr":{"path":["DisplayName"]}},{"name":"Object.$type","expr":{"path":["Object",{"meta":"type"}]}}]}]})RQL"));
			TestEqual(*FString::Printf(TEXT("Maps & Modes (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Maps)), UnrealMcpRqlSpecHelpers::CellText(Maps, 0, 2), FString(TEXT("GameMapsSettings")));
			const TSharedPtr<FJsonObject> Editor = Run(/* settings | where Container == "Editor" | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["settings"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Container"]},{"lit":"Editor"}]}},{"op":"count"}]})RQL"));
			TestTrue(TEXT("editor preferences listed"), UnrealMcpRqlSpecHelpers::Number(Editor) >= 1.0);
			const TSharedPtr<FJsonObject> Map = Run(/* unreal.settings("Maps & Modes") | select GameDefaultMap */ TEXT(R"RQL({"rql":"0.1","source":{"call":"settings","on":{"path":["unreal"]},"args":[{"lit":"Maps & Modes"}]},"stages":[{"op":"select","fields":[{"name":"GameDefaultMap","expr":{"path":["GameDefaultMap"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("a soft path setting reads as a path (%s)"), *UnrealMcpRqlSpecHelpers::CellText(Map)),
				UnrealMcpRqlSpecHelpers::CellText(Map).IsEmpty() || UnrealMcpRqlSpecHelpers::CellText(Map).StartsWith(TEXT("/")));
			const TSharedPtr<FJsonObject> ByName = Run(/* unreal.settings("GameMapsSettings") | select $type */ TEXT(R"RQL({"rql":"0.1","source":{"call":"settings","on":{"path":["unreal"]},"args":[{"lit":"GameMapsSettings"}]},"stages":[{"op":"select","fields":[{"name":"$type","expr":{"path":[{"meta":"type"}]}}]}]})RQL"));
			TestEqual(TEXT("unreal.settings by class name"), UnrealMcpRqlSpecHelpers::CellText(ByName), FString(TEXT("GameMapsSettings")));
			const TSharedPtr<FJsonObject> Unknown = Run(/* unreal.settings("NoSuchRqlSection") | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"settings","on":{"path":["unreal"]},"args":[{"lit":"NoSuchRqlSection"}]},"stages":[{"op":"count"}]})RQL"));
			TestFalse(TEXT("unknown section fails"), UnrealMcpRqlSpecHelpers::IsOk(Unknown));
			TestTrue(*FString::Printf(TEXT("not-found (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Unknown)), UnrealMcpRqlSpecHelpers::Codes(Unknown).Contains(TEXT("not-found")));
		});

		It("writes config properties and saves them to the ini; refuses the others", [this]()
		{
			Must(TEXT("set by section name"), /* unreal.settings("UnrealMcpRqlSpec") | set Level = 5, Greeting = "hi" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"settings","on":{"path":["unreal"]},"args":[{"lit":"UnrealMcpRqlSpec"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Level"]},"value":{"lit":5}},{"target":{"path":["Greeting"]},"value":{"lit":"hi"}}]}]})RQL"));
			const UUnrealMcpRqlSpecSettings* Defaults = GetDefault<UUnrealMcpRqlSpecSettings>();
			TestTrue(TEXT("in memory"), Defaults->Level == 5 && Defaults->Greeting == TEXT("hi"));
			const FString Ini = FileText(SettingsIni());
			TestTrue(*FString::Printf(TEXT("Level saved (%s)"), *Ini), Ini.Contains(TEXT("Level=5")));
			TestTrue(TEXT("Greeting saved"), Ini.Contains(TEXT("Greeting=hi")));
			TestTrue(TEXT("in the class's section"), Ini.Contains(TEXT("UnrealMcpRqlSpecSettings]")));

			Must(TEXT("an array (the section is saved)"), /* settings | where Section == "UnrealMcpRqlSpec" | get Object | set Items = ["a", "b"] */ TEXT(R"RQL({"rql":"0.1","source":{"path":["settings"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Section"]},{"lit":"UnrealMcpRqlSpec"}]}},{"op":"get","expr":{"path":["Object"]}},{"op":"set","assign":[{"target":{"path":["Items"]},"value":{"list":[{"lit":"a"},{"lit":"b"}]}}]}]})RQL"));
			const FString WithItems = FileText(SettingsIni());
			TestTrue(*FString::Printf(TEXT("items saved (%s)"), *WithItems), WithItems.Contains(TEXT("Items=a")) && WithItems.Contains(TEXT("Items=b")));

			Refuses(TEXT("not a config property"), /* unreal.settings("UnrealMcpRqlSpecSettings") | set NotSaved = 1 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"settings","on":{"path":["unreal"]},"args":[{"lit":"UnrealMcpRqlSpecSettings"}]},"stages":[{"op":"set","assign":[{"target":{"path":["NotSaved"]},"value":{"lit":1}}]}]})RQL"), TEXT("not a config property"));
			TestEqual(TEXT("not written"), Defaults->NotSaved, 0);

			IFileManager::Get().Delete(*SettingsIni(), false, true);
			const TSharedPtr<FJsonObject> Dry = Run(/* unreal.settings("UnrealMcpRqlSpec") | set Level = 9 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"settings","on":{"path":["unreal"]},"args":[{"lit":"UnrealMcpRqlSpec"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Level"]},"value":{"lit":9}}]}]})RQL"), true);
			(void)Dry;
			const FString Reloaded = FileText(SettingsIni());
			TestTrue(TEXT("saved again after the file was removed"), Reloaded.Contains(TEXT("Level=9")));
			// Re-read the ini into the class defaults: what a restart would load.
			GetMutableDefault<UUnrealMcpRqlSpecSettings>()->Level = 0;
			GetMutableDefault<UUnrealMcpRqlSpecSettings>()->ReloadConfig(nullptr, *SettingsIni());
			TestEqual(TEXT("reloaded from the ini"), GetDefault<UUnrealMcpRqlSpecSettings>()->Level, 9);
		});

		It("leaves the ini alone on a dry run, and class defaults outside settings stay read-only", [this]()
		{
			bool bOk = false;
			UnrealMcpRqlTools::ExecutePlan(/* unreal.settings("UnrealMcpRqlSpec") | set Level = 3 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"settings","on":{"path":["unreal"]},"args":[{"lit":"UnrealMcpRqlSpec"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Level"]},"value":{"lit":3}}]}]})RQL"), true, /*bDryRun*/ true, bOk);
			TestTrue(TEXT("dry run ok"), bOk);
			TestEqual(TEXT("not applied"), GetDefault<UUnrealMcpRqlSpecSettings>()->Level, 1);
			TestFalse(TEXT("nothing written"), IFileManager::Get().FileExists(*SettingsIni()));
			Refuses(TEXT("a class default that is not a settings section"), /* unreal.asset("/Script/UnrealMcpEditorTests.Default__UnrealMcpRqlSpecFixture") | set Small = 1 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Script/UnrealMcpEditorTests.Default__UnrealMcpRqlSpecFixture"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Small"]},"value":{"lit":1}}]}]})RQL"), TEXT("class default object"));
		});
	});

	Describe("Blueprint graphs", [this]()
	{
		It("lists graphs, adds events, calls, variables and flow nodes", [this]()
		{
			UBlueprint* Made = MakeBlueprint();
			const TSharedPtr<FJsonObject> Graphs = Run(/* unreal.asset("@BP@") | get Graphs | select Name Kind */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"get","expr":{"path":["Graphs"]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Kind","expr":{"path":["Kind"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("graphs (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Graphs)), UnrealMcpRqlSpecHelpers::RowCount(Graphs) >= 1);
			TestEqual(TEXT("the event graph"), UnrealMcpRqlSpecHelpers::CellText(Graphs, 0, 1), FString(TEXT("EventGraph")));
			const TSharedPtr<FJsonObject> Status = Run(/* unreal.asset("@BP@") | select Status EventGraph.Name */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"select","fields":[{"name":"Status","expr":{"path":["Status"]}},{"name":"EventGraph.Name","expr":{"path":["EventGraph","Name"]}}]}]})RQL"));
			TestEqual(TEXT("compiled"), UnrealMcpRqlSpecHelpers::CellText(Status, 0, 0), FString(TEXT("UpToDate")));
			const TSharedPtr<FJsonObject> Parent = Run(/* unreal.asset("@BP@") | select ParentClass */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"select","fields":[{"name":"ParentClass","expr":{"path":["ParentClass"]}}]}]})RQL"));
			TestEqual(*FString::Printf(TEXT("parent class (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Parent)), UnrealMcpRqlSpecHelpers::CellText(Parent, 0, 0), FString(TEXT("Actor")));
			const TSharedPtr<FJsonObject> Listed = Run(/* blueprints | where Path contains "@BP@" | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["blueprints"]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Path"]},{"lit":"@BP@"}]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("blueprints source"), UnrealMcpRqlSpecHelpers::Number(Listed), 1.0);

			const FString Event = ResultRef(Must(TEXT("event"), /* unreal.asset("@BP@").EventGraph | call AddEvent("BeginPlay") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddEvent","args":[{"lit":"BeginPlay"}]}]})RQL")));
			TestTrue(TEXT("an event node"), Event.StartsWith(TEXT("K2Node_Event#")));
			Refuses(TEXT("the same event again"), /* unreal.asset("@BP@").EventGraph | call AddEvent("BeginPlay") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddEvent","args":[{"lit":"BeginPlay"}]}]})RQL"), TEXT("already has the event"));
			Refuses(TEXT("not an event"), /* unreal.asset("@BP@").EventGraph | call AddEvent("NoSuchRqlEvent") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddEvent","args":[{"lit":"NoSuchRqlEvent"}]}]})RQL"), TEXT("not an overridable event"));
			const FString Print = ResultRef(Must(TEXT("call"), /* unreal.asset("@BP@").EventGraph | call AddCallFunction("PrintString", 400, 0) */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"PrintString"},{"lit":400},{"lit":0}]}]})RQL")));
			TestTrue(TEXT("a call node"), Print.StartsWith(TEXT("K2Node_CallFunction#")));
			const TSharedPtr<FJsonObject> Node = Run(/* ref("@N@") | select Title X Y Graph.Name */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"select","fields":[{"name":"Title","expr":{"path":["Title"]}},{"name":"X","expr":{"path":["X"]}},{"name":"Y","expr":{"path":["Y"]}},{"name":"Graph.Name","expr":{"path":["Graph","Name"]}}]}]})RQL"), false, { { TEXT("N"), Print } });
			TestTrue(*FString::Printf(TEXT("title (%s)"), *UnrealMcpRqlSpecHelpers::CellText(Node, 0, 0)), UnrealMcpRqlSpecHelpers::CellText(Node, 0, 0).Replace(TEXT(" "), TEXT("")).Contains(TEXT("PrintString")));
			TestEqual(TEXT("placed"), UnrealMcpRqlSpecHelpers::CellText(Node, 0, 1), FString(TEXT("400")));
			Must(TEXT("qualified function"), /* unreal.asset("@BP@").EventGraph | call AddCallFunction("KismetMathLibrary.Add_IntInt") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"KismetMathLibrary.Add_IntInt"}]}]})RQL"));
			Refuses(TEXT("unknown function"), /* unreal.asset("@BP@").EventGraph | call AddCallFunction("NoSuchRqlFunction") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"NoSuchRqlFunction"}]}]})RQL"), TEXT("no Blueprint-callable function"));
			Refuses(TEXT("unknown class"), /* unreal.asset("@BP@").EventGraph | call AddCallFunction("NoSuchRqlClass.Foo") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"NoSuchRqlClass.Foo"}]}]})RQL"), TEXT("no class"));
			Must(TEXT("variable get"), /* unreal.asset("@BP@").EventGraph | call AddVariableGet("Health") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddVariableGet","args":[{"lit":"Health"}]}]})RQL"));
			Must(TEXT("variable set"), /* unreal.asset("@BP@").EventGraph | call AddVariableSet("bArmed") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddVariableSet","args":[{"lit":"bArmed"}]}]})RQL"));
			Refuses(TEXT("unknown variable"), /* unreal.asset("@BP@").EventGraph | call AddVariableGet("Mana") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddVariableGet","args":[{"lit":"Mana"}]}]})RQL"), TEXT("no Blueprint variable 'Mana'"));
			Must(TEXT("branch"), /* unreal.asset("@BP@").EventGraph | call AddNode("Branch") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddNode","args":[{"lit":"Branch"}]}]})RQL"));
			Must(TEXT("sequence"), /* unreal.asset("@BP@").EventGraph | call AddNode("Sequence") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddNode","args":[{"lit":"Sequence"}]}]})RQL"));
			const FString Loop = ResultRef(Must(TEXT("a standard macro"), /* unreal.asset("@BP@").EventGraph | call AddNode("ForLoop") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddNode","args":[{"lit":"ForLoop"}]}]})RQL")));
			TestTrue(TEXT("macro instance"), Loop.StartsWith(TEXT("K2Node_MacroInstance#")));
			Refuses(TEXT("unknown kind"), /* unreal.asset("@BP@").EventGraph | call AddNode("Teleport") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddNode","args":[{"lit":"Teleport"}]}]})RQL"), TEXT("ForEachLoop"));
			Must(TEXT("cast"), /* unreal.asset("@BP@").EventGraph | call AddCast("Pawn") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCast","args":[{"lit":"Pawn"}]}]})RQL"));
			Must(TEXT("custom event"), /* unreal.asset("@BP@").EventGraph | call AddCustomEvent("RqlFire") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCustomEvent","args":[{"lit":"RqlFire"}]}]})RQL"));
			Refuses(TEXT("the same custom event"), /* unreal.asset("@BP@").EventGraph | call AddCustomEvent("RqlFire") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCustomEvent","args":[{"lit":"RqlFire"}]}]})RQL"), TEXT("already has"));
			const TSharedPtr<FJsonObject> Count = Run(/* unreal.asset("@BP@").EventGraph | get Nodes | where Title contains "Print" | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"get","expr":{"path":["Nodes"]}},{"op":"where","pred":{"op":"contains","args":[{"path":["Title"]},{"lit":"Print"}]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("nodes are queryable"), UnrealMcpRqlSpecHelpers::Number(Count), 1.0);
			TestTrue(TEXT("the Blueprint is dirty"), Made->GetOutermost()->IsDirty());

			const FString Function = ResultRef(Must(TEXT("function graph"), /* unreal.asset("@BP@") | call AddFunction("RqlDoThing") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"call","method":"AddFunction","args":[{"lit":"RqlDoThing"}]}]})RQL")));
			const TSharedPtr<FJsonObject> Kind = Run(/* ref("@G@") | select Kind Nodes */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@G@"}]},"stages":[{"op":"select","fields":[{"name":"Kind","expr":{"path":["Kind"]}},{"name":"Nodes","expr":{"path":["Nodes"]}}]}]})RQL"), false, { { TEXT("G"), Function } });
			TestEqual(TEXT("a function graph"), UnrealMcpRqlSpecHelpers::CellText(Kind, 0, 0), FString(TEXT("Function")));
			Refuses(TEXT("events in a function"), /* ref("@G@") | call AddEvent("Tick") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@G@"}]},"stages":[{"op":"call","method":"AddEvent","args":[{"lit":"Tick"}]}]})RQL"), TEXT("events go into an event graph"), { { TEXT("G"), Function } });
			Refuses(TEXT("the function entry"), /* ref("@G@") | get Nodes | call Destroy() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@G@"}]},"stages":[{"op":"get","expr":{"path":["Nodes"]}},{"op":"call","method":"Destroy","args":[]}]})RQL"), TEXT("cannot be deleted"), { { TEXT("G"), Function } });
			Must(TEXT("a call of the new function"), /* unreal.asset("@BP@").EventGraph | call AddCallFunction("RqlDoThing") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"RqlDoThing"}]}]})RQL"));
		});

		It("reads pins, sets defaults, connects and disconnects them", [this]()
		{
			MakeBlueprint();
			const FString Event = ResultRef(Must(TEXT("event"), /* unreal.asset("@BP@").EventGraph | call AddEvent("BeginPlay") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddEvent","args":[{"lit":"BeginPlay"}]}]})RQL")));
			const FString Print = ResultRef(Must(TEXT("call"), /* unreal.asset("@BP@").EventGraph | call AddCallFunction("PrintString") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"PrintString"}]}]})RQL")));
			const FString Get = ResultRef(Must(TEXT("get"), /* unreal.asset("@BP@").EventGraph | call AddVariableGet("Health") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddVariableGet","args":[{"lit":"Health"}]}]})RQL")));
			const TSharedPtr<FJsonObject> Pins = Run(/* ref("@N@") | get Pins | select Name Direction Type IsExec */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"get","expr":{"path":["Pins"]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Direction","expr":{"path":["Direction"]}},{"name":"Type","expr":{"path":["Type"]}},{"name":"IsExec","expr":{"path":["IsExec"]}}]}]})RQL"), false, { { TEXT("N"), Print } });
			int32 Duration = INDEX_NONE;
			for (int32 Row = 0; Row < UnrealMcpRqlSpecHelpers::RowCount(Pins); ++Row)
			{
				if (UnrealMcpRqlSpecHelpers::CellText(Pins, Row, 0) == TEXT("Duration"))
					Duration = Row;
			}
			TestEqual(TEXT("first pin: the exec input"), UnrealMcpRqlSpecHelpers::CellText(Pins, 0, 0), FString(TEXT("execute")));
			TestEqual(TEXT("exec"), UnrealMcpRqlSpecHelpers::CellText(Pins, 0, 3), FString(TEXT("true")));
			TestTrue(TEXT("Duration pin"), Duration != INDEX_NONE);

			Must(TEXT("default by call"), /* ref("@N@") | call SetPinDefault("InString", "Hello RQL") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"call","method":"SetPinDefault","args":[{"lit":"InString"},{"lit":"Hello RQL"}]}]})RQL"), { { TEXT("N"), Print } });
			const TSharedPtr<FJsonObject> Default = Run(/* ref("@N@") | get Pins | where Name == "InString" | select DefaultValue */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"get","expr":{"path":["Pins"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"InString"}]}},{"op":"select","fields":[{"name":"DefaultValue","expr":{"path":["DefaultValue"]}}]}]})RQL"), false, { { TEXT("N"), Print } });
			TestEqual(TEXT("default reads"), UnrealMcpRqlSpecHelpers::CellText(Default), FString(TEXT("Hello RQL")));
			Must(TEXT("default by member"), /* ref("@N@") | get Pins | where Name == "Duration" | set DefaultValue = 5 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"get","expr":{"path":["Pins"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Duration"}]}},{"op":"set","assign":[{"target":{"path":["DefaultValue"]},"value":{"lit":5}}]}]})RQL"), { { TEXT("N"), Print } });
			const TSharedPtr<FJsonObject> Five = Run(/* ref("@N@") | get Pins | where Name == "Duration" | select DefaultValue */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"get","expr":{"path":["Pins"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Duration"}]}},{"op":"select","fields":[{"name":"DefaultValue","expr":{"path":["DefaultValue"]}}]}]})RQL"), false, { { TEXT("N"), Print } });
			TestTrue(TEXT("number default"), UnrealMcpRqlSpecHelpers::CellText(Five).StartsWith(TEXT("5")));
			Must(TEXT("a struct default in RQL form"), /* ref("@N@") | call SetPinDefault("TextColor", "(1, 0, 0, 1)") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"call","method":"SetPinDefault","args":[{"lit":"TextColor"},{"lit":"(1, 0, 0, 1)"}]}]})RQL"), { { TEXT("N"), Print } });
			Refuses(TEXT("an exec pin has no default"), /* ref("@N@") | call SetPinDefault("execute", "x") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"call","method":"SetPinDefault","args":[{"lit":"execute"},{"lit":"x"}]}]})RQL"), TEXT("execution pin"), { { TEXT("N"), Print } });
			Refuses(TEXT("an unknown pin"), /* ref("@N@") | call SetPinDefault("Nope", "x") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"call","method":"SetPinDefault","args":[{"lit":"Nope"},{"lit":"x"}]}]})RQL"), TEXT("has no pin 'Nope'"), { { TEXT("N"), Print } });

			Must(TEXT("connect exec"), /* ref("@E@") | call Connect("then", ref("@N@"), "execute") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@E@"}]},"stages":[{"op":"call","method":"Connect","args":[{"lit":"then"},{"call":"ref","args":[{"lit":"@N@"}]},{"lit":"execute"}]}]})RQL"), { { TEXT("E"), Event }, { TEXT("N"), Print } });
			const TSharedPtr<FJsonObject> Linked = Run(/* ref("@N@") | get Pins | where Name == "execute" | get LinkedTo | select Name Node.Title */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"get","expr":{"path":["Pins"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"execute"}]}},{"op":"get","expr":{"path":["LinkedTo"]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Node.Title","expr":{"path":["Node","Title"]}}]}]})RQL"), false, { { TEXT("N"), Print } });
			TestEqual(TEXT("linked to the event's then"), UnrealMcpRqlSpecHelpers::CellText(Linked, 0, 0), FString(TEXT("then")));
			Refuses(TEXT("exec into data"), /* ref("@E@") | call Connect("then", ref("@N@"), "InString") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@E@"}]},"stages":[{"op":"call","method":"Connect","args":[{"lit":"then"},{"call":"ref","args":[{"lit":"@N@"}]},{"lit":"InString"}]}]})RQL"), TEXT("cannot connect"), { { TEXT("E"), Event }, { TEXT("N"), Print } });
			Must(TEXT("pin to pin"), /* ref("@G@") | get Pins | where Name == "Health" | call Connect(ref("@N@").Pins.first(Name == "Duration")) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@G@"}]},"stages":[{"op":"get","expr":{"path":["Pins"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Health"}]}},{"op":"call","method":"Connect","args":[{"call":"first","on":{"path":["Pins"],"on":{"call":"ref","args":[{"lit":"@N@"}]}},"args":[{"op":"==","args":[{"path":["Name"]},{"lit":"Duration"}]}]}]}]})RQL"),
				{ { TEXT("G"), Get }, { TEXT("N"), Print } });
			const TSharedPtr<FJsonObject> Data = Run(/* ref("@N@") | get Pins | where Name == "Duration" | select LinkedTo */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"get","expr":{"path":["Pins"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Duration"}]}},{"op":"select","fields":[{"name":"LinkedTo","expr":{"path":["LinkedTo"]}}]}]})RQL"), false, { { TEXT("N"), Print } });
			TestTrue(TEXT("data connected"), UnrealMcpRqlSpecHelpers::CellText(Data).Contains(TEXT("Health")));
			Must(TEXT("disconnect"), /* ref("@E@") | call Disconnect("then", ref("@N@"), "execute") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@E@"}]},"stages":[{"op":"call","method":"Disconnect","args":[{"lit":"then"},{"call":"ref","args":[{"lit":"@N@"}]},{"lit":"execute"}]}]})RQL"), { { TEXT("E"), Event }, { TEXT("N"), Print } });
			Refuses(TEXT("not connected"), /* ref("@E@") | call Disconnect("then", ref("@N@"), "execute") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@E@"}]},"stages":[{"op":"call","method":"Disconnect","args":[{"lit":"then"},{"call":"ref","args":[{"lit":"@N@"}]},{"lit":"execute"}]}]})RQL"), TEXT("not connected"), { { TEXT("E"), Event }, { TEXT("N"), Print } });
			Must(TEXT("break all"), /* ref("@N@") | call BreakLinks() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"call","method":"BreakLinks","args":[]}]})RQL"), { { TEXT("N"), Print } });
			const TSharedPtr<FJsonObject> None = Run(/* ref("@N@") | get Pins | where LinkedTo.any() | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"get","expr":{"path":["Pins"]}},{"op":"where","pred":{"call":"any","on":{"path":["LinkedTo"]},"args":[]}},{"op":"count"}]})RQL"), false, { { TEXT("N"), Print } });
			TestEqual(TEXT("no links left"), UnrealMcpRqlSpecHelpers::Number(None), 0.0);
		});

		It("edits node members, removes nodes, compiles and undoes", [this]()
		{
			UBlueprint* Made = MakeBlueprint();
			const FString Print = ResultRef(Must(TEXT("call"), /* unreal.asset("@BP@").EventGraph | call AddCallFunction("PrintString") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"PrintString"}]}]})RQL")));
			Must(TEXT("node members"), /* ref("@N@") | set X = 640, Y = -32, Comment = "says hello" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["X"]},"value":{"lit":640}},{"target":{"path":["Y"]},"value":{"lit":-32}},{"target":{"path":["Comment"]},"value":{"lit":"says hello"}}]}]})RQL"), { { TEXT("N"), Print } });
			UEdGraphNode* PrintNode = nullptr;
			for (UEdGraphNode* Node : FBlueprintEditorUtils::FindEventGraph(Made)->Nodes)
			{
				// "Print String", or "PrintString" when the editor shows no friendly names (unattended runs)
				if (Node && Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT(" "), TEXT("")).Contains(TEXT("PrintString")))
					PrintNode = Node;
			}
			if (!TestNotNull(TEXT("print node"), PrintNode))
				return;
			TestTrue(TEXT("written"), PrintNode->NodePosX == 640 && PrintNode->NodePosY == -32 && PrintNode->NodeComment == TEXT("says hello"));

			const FString Event = ResultRef(Must(TEXT("event"), /* unreal.asset("@BP@").EventGraph | call AddEvent("BeginPlay") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddEvent","args":[{"lit":"BeginPlay"}]}]})RQL")));
			Must(TEXT("wired"), /* ref("@E@") | call Connect("then", ref("@N@"), "execute") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@E@"}]},"stages":[{"op":"call","method":"Connect","args":[{"lit":"then"},{"call":"ref","args":[{"lit":"@N@"}]},{"lit":"execute"}]}]})RQL"), { { TEXT("E"), Event }, { TEXT("N"), Print } });
			const TSharedPtr<FJsonObject> Good = Must(TEXT("compile"), /* unreal.asset("@BP@") | call Compile() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"call","method":"Compile","args":[]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("compiles (%s)"), *UnrealMcpRqlSpecHelpers::CellText(Good, 0, 2)), UnrealMcpRqlSpecHelpers::CellText(Good, 0, 2).Contains(TEXT("\"Succeeded\":true")));
			const TSharedPtr<FJsonObject> Status = Run(/* unreal.asset("@BP@") | select Status */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"select","fields":[{"name":"Status","expr":{"path":["Status"]}}]}]})RQL"));
			TestTrue(TEXT("status after compile"), UnrealMcpRqlSpecHelpers::CellText(Status).StartsWith(TEXT("UpToDate")));

			// A Pawn function on an Actor Blueprint: "self" is not a Pawn, a compile error on that node.
			const FString Move = ResultRef(Must(TEXT("pawn call"), /* unreal.asset("@BP@").EventGraph | call AddCallFunction("Pawn.AddMovementInput") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"Pawn.AddMovementInput"}]}]})RQL")));
			Must(TEXT("wired into the chain"), /* ref("@N@") | call Connect("then", ref("@M@"), "execute") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"call","method":"Connect","args":[{"lit":"then"},{"call":"ref","args":[{"lit":"@M@"}]},{"lit":"execute"}]}]})RQL"), { { TEXT("N"), Print }, { TEXT("M"), Move } });
			const TSharedPtr<FJsonObject> Bad = Must(TEXT("compile with an error"), /* unreal.asset("@BP@") | call Compile() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"call","method":"Compile","args":[]}]})RQL"));
			const FString Result = UnrealMcpRqlSpecHelpers::CellText(Bad, 0, 2);
			TestTrue(*FString::Printf(TEXT("a failed compile is a result (%s)"), *Result), Result.Contains(TEXT("\"Succeeded\":false")));
			TestTrue(TEXT("the message names the node"), Result.Contains(TEXT("K2Node_CallFunction#")));
			const TSharedPtr<FJsonObject> Error = Run(/* ref("@M@") | select Error */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@M@"}]},"stages":[{"op":"select","fields":[{"name":"Error","expr":{"path":["Error"]}}]}]})RQL"), false, { { TEXT("M"), Move } });
			TestFalse(TEXT("the node carries the error"), UnrealMcpRqlSpecHelpers::CellText(Error).IsEmpty());
			Must(TEXT("node removed"), /* ref("@M@") | call Destroy() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@M@"}]},"stages":[{"op":"call","method":"Destroy","args":[]}]})RQL"), { { TEXT("M"), Move } });
			const TSharedPtr<FJsonObject> Fixed = Must(TEXT("compile again"), /* unreal.asset("@BP@") | call Compile() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"call","method":"Compile","args":[]}]})RQL"));
			TestTrue(TEXT("compiles after the removal"), UnrealMcpRqlSpecHelpers::CellText(Fixed, 0, 2).Contains(TEXT("\"Succeeded\":true")));

			if (!GEditor)
				return;
			UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Made);
			const int32 Before = Graph->Nodes.Num();
			Must(TEXT("added for undo"), /* unreal.asset("@BP@").EventGraph | call AddNode("Branch") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddNode","args":[{"lit":"Branch"}]}]})RQL"));
			TestEqual(TEXT("one more node"), Graph->Nodes.Num(), Before + 1);
			GEditor->UndoTransaction();
			TestEqual(TEXT("undo removes it"), Graph->Nodes.Num(), Before);
			GEditor->RedoTransaction();
			TestEqual(TEXT("redo restores it"), Graph->Nodes.Num(), Before + 1);
			Must(TEXT("disabled"), /* ref("@N@") | set Enabled = false */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Enabled"]},"value":{"lit":false}}]}]})RQL"), { { TEXT("N"), Print } });
			TestFalse(TEXT("node disabled"), PrintNode->IsNodeEnabled());
		});
	});

	Describe("Blueprint components and variables", [this]()
	{
		It("edits component defaults: placed actors that keep the old value follow, overrides stay, undo restores", [this]()
		{
			UBlueprint* Made = MakeBlueprint();
			USCS_Node* Node = Made->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("RqlMesh"));
			Made->SimpleConstructionScript->AddNode(Node);
			FKismetEditorUtilities::CompileBlueprint(Made);
			const FString Name = FPackageName::GetShortName(Blueprint);

			const TSharedPtr<FJsonObject> Listed = Run(/* unreal.asset("@BP@") | get Components | select Name $type */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"get","expr":{"path":["Components"]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"$type","expr":{"path":[{"meta":"type"}]}}]}]})RQL"));
			TestEqual(TEXT("one component"), UnrealMcpRqlSpecHelpers::RowCount(Listed), 1);
			TestEqual(TEXT("read under its variable name"), UnrealMcpRqlSpecHelpers::CellText(Listed, 0, 0), FString(TEXT("RqlMesh")));
			TestEqual(TEXT("its class"), UnrealMcpRqlSpecHelpers::CellText(Listed, 0, 1), FString(TEXT("StaticMeshComponent")));

			Must(TEXT("spawned by Blueprint name"), /* editor | call SpawnActor("@BPNAME@", "RqlSeqLampBP") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"@BPNAME@"},{"lit":"RqlSeqLampBP"}]}]})RQL"), { { TEXT("BPNAME"), Name } });
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			AActor* Follower = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == TEXT("RqlSeqLampBP"))
					Follower = *It;
			}
			if (!TestNotNull(TEXT("placed"), Follower) || !TestTrue(TEXT("of the Blueprint's class"), Follower->GetClass() == Made->GeneratedClass))
				return;
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			AActor* Overrider = World->SpawnActor<AActor>(Made->GeneratedClass, FTransform::Identity, Params);
			Overrider->SetActorLabel(TEXT("RqlSeqLampBPOverride"));
			UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
			Overrider->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Cone);
			Made->GetOutermost()->SetDirtyFlag(false);

			Must(TEXT("template mesh"), /* unreal.asset("@BP@").Components | where Name == "RqlMesh" | set StaticMesh = "/Engine/BasicShapes/Sphere" */ TEXT(R"RQL({"rql":"0.1","source":{"path":["Components"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlMesh"}]}},{"op":"set","assign":[{"target":{"path":["StaticMesh"]},"value":{"lit":"/Engine/BasicShapes/Sphere"}}]}]})RQL"));
			UStaticMeshComponent* Template = Cast<UStaticMeshComponent>(Node->ComponentTemplate);
			TestTrue(TEXT("the template has the mesh"), Template && Template->GetStaticMesh() && Template->GetStaticMesh()->GetName() == TEXT("Sphere"));
			const UStaticMeshComponent* FollowerMesh = Follower->FindComponentByClass<UStaticMeshComponent>();
			TestTrue(TEXT("the placed actor follows"), FollowerMesh && FollowerMesh->GetStaticMesh() && FollowerMesh->GetStaticMesh()->GetName() == TEXT("Sphere"));
			TestTrue(TEXT("an override stays"), Overrider->FindComponentByClass<UStaticMeshComponent>()->GetStaticMesh() == Cone);
			TestTrue(TEXT("the Blueprint is modified"), Made->GetOutermost()->IsDirty());

			if (!GEditor)
				return;
			GEditor->UndoTransaction();
			TestTrue(TEXT("undo restores the template"), Template && Template->GetStaticMesh() == nullptr);
			// Read the component again: the placed actor's construction script may have rebuilt it.
			const UStaticMeshComponent* Restored = IsValid(Follower) ? Follower->FindComponentByClass<UStaticMeshComponent>() : nullptr;
			TestTrue(TEXT("and the placed actor"), Restored && Restored->GetStaticMesh() == nullptr);
		});

		It("writes several members of a placed actor's construction-script component in one set (each write rebuilds it)", [this]()
		{
			UBlueprint* Made = MakeBlueprint();
			USCS_Node* Node = Made->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("RqlMesh"));
			Made->SimpleConstructionScript->AddNode(Node);
			FKismetEditorUtilities::CompileBlueprint(Made);
			Must(TEXT("spawned"), /* editor | call SpawnActor("@BPNAME@", "RqlSeqLampBP", "(1000, 0, 0)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"@BPNAME@"},{"lit":"RqlSeqLampBP"},{"lit":"(1000, 0, 0)"}]}]})RQL"), { { TEXT("BPNAME"), FPackageName::GetShortName(Blueprint) } });
			const TSharedPtr<FJsonObject> Receipt = Must(TEXT("three members in one set"), /* unreal.find("RqlSeqLampBP") | get Components | where Name == "RqlMesh" | set RelativeLocation = "(0, 0, 500)", RelativeScale3D = "(2, 2, 2)", bVisible = false */ TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSeqLampBP"}]},"stages":[{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlMesh"}]}},{"op":"set","assign":[{"target":{"path":["RelativeLocation"]},"value":{"lit":"(0, 0, 500)"}},{"target":{"path":["RelativeScale3D"]},"value":{"lit":"(2, 2, 2)"}},{"target":{"path":["bVisible"]},"value":{"lit":false}}]}]})RQL"));
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			AActor* Placed = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == TEXT("RqlSeqLampBP"))
					Placed = *It;
			}
			if (!TestNotNull(TEXT("placed"), Placed))
				return;
			const UStaticMeshComponent* Mesh = Placed->FindComponentByClass<UStaticMeshComponent>();
			if (!TestNotNull(TEXT("its mesh component"), Mesh))
				return;
			TestEqual(TEXT("the first member stuck"), Mesh->GetRelativeLocation(), FVector(0, 0, 500));
			TestEqual(TEXT("the second member reached the live component"), Mesh->GetRelativeScale3D(), FVector(2, 2, 2));
			TestFalse(TEXT("and the third (each write rebuilt the component again)"), Mesh->IsVisible());
			// The receipt reads the values back from the live component, not from the discarded copy.
			TestEqual(TEXT("receipt: new location"), UnrealMcpRqlSpecHelpers::CellText(Receipt, 0, 3), FString(TEXT("(0, 0, 500)")));
			TestEqual(TEXT("receipt: new scale"), UnrealMcpRqlSpecHelpers::CellText(Receipt, 1, 3), FString(TEXT("(2, 2, 2)")));
		});

		It("refuses writing a variable that is not Instance Editable on a placed actor (it would be reset)", [this]()
		{
			UBlueprint* Made = MakeBlueprint();
			Must(TEXT("spawned"), /* editor | call SpawnActor("@BPNAME@", "RqlSeqLampBP") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"@BPNAME@"},{"lit":"RqlSeqLampBP"}]}]})RQL"), { { TEXT("BPNAME"), FPackageName::GetShortName(Blueprint) } });
			Refuses(TEXT("not Instance Editable"), /* unreal.find("RqlSeqLampBP") | set Health = 5 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSeqLampBP"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Health"]},"value":{"lit":5}}]}]})RQL"), TEXT("is not Instance Editable"));
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Made, TEXT("Health"), /*bNewFlag*/ false);
			FKismetEditorUtilities::CompileBlueprint(Made);
			Must(TEXT("instance editable now"), /* unreal.find("RqlSeqLampBP") | set Health = 5 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSeqLampBP"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Health"]},"value":{"lit":5}}]}]})RQL"));
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			bool bFound = false;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == TEXT("RqlSeqLampBP"))
				{
					bFound = true;
					const FDoubleProperty* Health = FindFProperty<FDoubleProperty>(It->GetClass(), TEXT("Health"));
					TestTrue(TEXT("the value stuck"), Health && Health->GetPropertyValue_InContainer(*It) == 5.0);
				}
			}
			TestTrue(TEXT("the placed actor is still there"), bFound);
		});

		It("matches a placed Blueprint actor by the Blueprint's name, with or without _C", [this]()
		{
			MakeBlueprint();
			const FString Name = FPackageName::GetShortName(Blueprint);
			Must(TEXT("spawned"), /* editor | call SpawnActor("@BPNAME@", "RqlSeqLampBP") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"@BPNAME@"},{"lit":"RqlSeqLampBP"}]}]})RQL"), { { TEXT("BPNAME"), Name } });
			for (const FString& TypeName : { Name, Name + TEXT("_C") })
			{
				const TSharedPtr<FJsonObject> Matched = Run(/* actors | where Label == "RqlSeqLampBP" and $ is @TYPE@ | select $type */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"and","args":[{"op":"==","args":[{"path":["Label"]},{"lit":"RqlSeqLampBP"}]},{"op":"is","args":[{"path":[]},{"lit":"@TYPE@"}]}]}},{"op":"select","fields":[{"name":"$type","expr":{"path":[{"meta":"type"}]}}]}]})RQL"), false, { { TEXT("TYPE"), TypeName } });
				TestEqual(*FString::Printf(TEXT("is %s (%s)"), *TypeName, *UnrealMcpRqlSpecHelpers::Codes(Matched)), UnrealMcpRqlSpecHelpers::RowCount(Matched), 1);
				TestEqual(TEXT("$type is the class name"), UnrealMcpRqlSpecHelpers::CellText(Matched, 0, 0), Name + TEXT("_C"));
			}
		});
	});

	Describe("Widget Blueprints", [this]()
	{
		It("builds a widget tree, edits widgets and slots, removes widgets, compiles and undoes", [this]()
		{
			UWidgetBlueprint* Made = MakeWidgetBlueprint();
			if (!TestNotNull(TEXT("widget blueprint"), Made))
				return;
			const TSharedPtr<FJsonObject> Empty = Run(/* unreal.asset("@WBP@") | select RootWidget Widgets */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"select","fields":[{"name":"RootWidget","expr":{"path":["RootWidget"]}},{"name":"Widgets","expr":{"path":["Widgets"]}}]}]})RQL"));
			TestEqual(TEXT("no root yet"), UnrealMcpRqlSpecHelpers::CellText(Empty, 0, 0), FString());
			const FString Root = ResultRef(Must(TEXT("root"), /* unreal.asset("@WBP@") | call AddWidget("CanvasPanel", "Root") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"call","method":"AddWidget","args":[{"lit":"CanvasPanel"},{"lit":"Root"}]}]})RQL")));
			TestTrue(TEXT("a canvas"), Root.StartsWith(TEXT("CanvasPanel#")));
			Refuses(TEXT("a second root"), /* unreal.asset("@WBP@") | call AddWidget("Border") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"call","method":"AddWidget","args":[{"lit":"Border"}]}]})RQL"), TEXT("already has a root widget"));
			const FString Title = ResultRef(Must(TEXT("child of the root"), /* ref("@R@") | call AddChild("TextBlock", "Title") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@R@"}]},"stages":[{"op":"call","method":"AddChild","args":[{"lit":"TextBlock"},{"lit":"Title"}]}]})RQL"), { { TEXT("R"), Root } }));
			Refuses(TEXT("a text block is no panel"), /* ref("@T@") | call AddChild("Button") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@T@"}]},"stages":[{"op":"call","method":"AddChild","args":[{"lit":"Button"}]}]})RQL"), TEXT("not a panel"), { { TEXT("T"), Title } });
			Refuses(TEXT("a taken name"), /* ref("@R@") | call AddChild("TextBlock", "Title") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@R@"}]},"stages":[{"op":"call","method":"AddChild","args":[{"lit":"TextBlock"},{"lit":"Title"}]}]})RQL"), TEXT("already has a widget named 'Title'"), { { TEXT("R"), Root } });
			Refuses(TEXT("not a widget class"), /* ref("@R@") | call AddChild("PointLight") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@R@"}]},"stages":[{"op":"call","method":"AddChild","args":[{"lit":"PointLight"}]}]})RQL"), TEXT("not a widget class"), { { TEXT("R"), Root } });
			const FString List = ResultRef(Must(TEXT("a panel by parent argument"), /* unreal.asset("@WBP@") | call AddWidget("VerticalBox", "List", ref("@R@")) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"call","method":"AddWidget","args":[{"lit":"VerticalBox"},{"lit":"List"},{"call":"ref","args":[{"lit":"@R@"}]}]}]})RQL"), { { TEXT("R"), Root } }));
			Must(TEXT("first button"), /* ref("@L@") | call AddChild("Button", "Ok") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@L@"}]},"stages":[{"op":"call","method":"AddChild","args":[{"lit":"Button"},{"lit":"Ok"}]}]})RQL"), { { TEXT("L"), List } });
			Must(TEXT("second button"), /* ref("@L@") | call AddChild("Button", "Cancel") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@L@"}]},"stages":[{"op":"call","method":"AddChild","args":[{"lit":"Button"},{"lit":"Cancel"}]}]})RQL"), { { TEXT("L"), List } });
			const TSharedPtr<FJsonObject> Count = Run(/* unreal.asset("@WBP@") | get Widgets | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"get","expr":{"path":["Widgets"]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("five widgets"), UnrealMcpRqlSpecHelpers::Number(Count), 5.0);
			const TSharedPtr<FJsonObject> Children = Run(/* ref("@L@") | get Children | select Name Parent.Name */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@L@"}]},"stages":[{"op":"get","expr":{"path":["Children"]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Parent.Name","expr":{"path":["Parent","Name"]}}]}]})RQL"), false, { { TEXT("L"), List } });
			TestEqual(TEXT("children in order"), UnrealMcpRqlSpecHelpers::CellText(Children, 1, 0), FString(TEXT("Cancel")));
			TestEqual(TEXT("their parent"), UnrealMcpRqlSpecHelpers::CellText(Children, 0, 1), FString(TEXT("List")));
			const TSharedPtr<FJsonObject> Tree = Run(/* unreal.asset("@WBP@") | get RootWidget | recurse Children | select $depth Name */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"get","expr":{"path":["RootWidget"]}},{"op":"recurse","via":[{"path":["Children"]}]},{"op":"select","fields":[{"name":"$depth","expr":{"path":[{"meta":"depth"}]}},{"name":"Name","expr":{"path":["Name"]}}]}]})RQL"));
			TestEqual(TEXT("recurse walks the tree"), UnrealMcpRqlSpecHelpers::RowCount(Tree), 5);

			Must(TEXT("a widget property"), /* ref("@T@") | set Text = "Hello RQL" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@T@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Text"]},"value":{"lit":"Hello RQL"}}]}]})RQL"), { { TEXT("T"), Title } });
			UTextBlock* Text = Cast<UTextBlock>(Made->WidgetTree->FindWidget(TEXT("Title")));
			TestTrue(TEXT("text stored"), Text && Text->GetText().ToString() == TEXT("Hello RQL"));
			Must(TEXT("its canvas slot"), /* ref("@T@") | set Slot.LayoutData.Offsets.Left = 24 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@T@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Slot","LayoutData","Offsets","Left"]},"value":{"lit":24}}]}]})RQL"), { { TEXT("T"), Title } });
			const UCanvasPanelSlot* Slot = Text ? Cast<UCanvasPanelSlot>(Text->Slot) : nullptr;
			TestTrue(TEXT("slot stored"), Slot && FMath::IsNearlyEqual(Slot->GetLayout().Offsets.Left, 24.0f));
			const TSharedPtr<FJsonObject> Status = Run(/* unreal.asset("@WBP@") | select Status */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"select","fields":[{"name":"Status","expr":{"path":["Status"]}}]}]})RQL"));
			TestEqual(TEXT("the Blueprint is marked modified"), UnrealMcpRqlSpecHelpers::CellText(Status), FString(TEXT("Dirty")));

			Must(TEXT("a widget removed"), /* unreal.asset("@WBP@") | get Widgets | where Name == "Cancel" | call Destroy() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"get","expr":{"path":["Widgets"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Cancel"}]}},{"op":"call","method":"Destroy","args":[]}]})RQL"));
			const TSharedPtr<FJsonObject> After = Run(/* ref("@L@") | get Children | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@L@"}]},"stages":[{"op":"get","expr":{"path":["Children"]}},{"op":"count"}]})RQL"), false, { { TEXT("L"), List } });
			TestEqual(TEXT("one button left"), UnrealMcpRqlSpecHelpers::Number(After), 1.0);
			const TSharedPtr<FJsonObject> Compiled = Must(TEXT("compile"), /* unreal.asset("@WBP@") | call Compile() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"call","method":"Compile","args":[]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("compiles (%s)"), *UnrealMcpRqlSpecHelpers::CellText(Compiled, 0, 2)), UnrealMcpRqlSpecHelpers::CellText(Compiled, 0, 2).Contains(TEXT("\"Succeeded\":true")));

			if (!GEditor)
				return;
			Must(TEXT("one more for undo"), /* ref("@L@") | call AddChild("Image", "Icon") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@L@"}]},"stages":[{"op":"call","method":"AddChild","args":[{"lit":"Image"},{"lit":"Icon"}]}]})RQL"), { { TEXT("L"), List } });
			TestNotNull(TEXT("added"), Made->WidgetTree->FindWidget(TEXT("Icon")));
			GEditor->UndoTransaction();
			const TSharedPtr<FJsonObject> Undone = Run(/* ref("@L@") | get Children | where Name == "Icon" | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@L@"}]},"stages":[{"op":"get","expr":{"path":["Children"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Icon"}]}},{"op":"count"}]})RQL"), false, { { TEXT("L"), List } });
			TestEqual(TEXT("undo takes it out of the panel"), UnrealMcpRqlSpecHelpers::Number(Undone), 0.0);
		});
	});

	Describe("widget variables", [this]()
	{
		It("makes a widget a variable (the Designer's Is Variable) so a graph can read it, undoably", [this]()
		{
			UWidgetBlueprint* Made = MakeWidgetBlueprint();
			if (!TestNotNull(TEXT("widget blueprint"), Made))
				return;
			const FString Root = ResultRef(Must(TEXT("root"), /* unreal.asset("@WBP@") | call AddWidget("CanvasPanel", "Root") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"call","method":"AddWidget","args":[{"lit":"CanvasPanel"},{"lit":"Root"}]}]})RQL")));
			Must(TEXT("a text block"), /* unreal.asset("@WBP@") | call AddWidget("TextBlock", "Label", ref("@R@")) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"call","method":"AddWidget","args":[{"lit":"TextBlock"},{"lit":"Label"},{"call":"ref","args":[{"lit":"@R@"}]}]}]})RQL"), { { TEXT("R"), Root } });
			const TSharedPtr<FJsonObject> Before = Run(/* unreal.asset("@WBP@").Widgets | where Name == "Label" | select IsVariable */ TEXT(R"RQL({"rql":"0.1","source":{"path":["Widgets"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]}},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Label"}]}},{"op":"select","fields":[{"name":"IsVariable","expr":{"path":["IsVariable"]}}]}]})RQL"));
			TestEqual(*FString::Printf(TEXT("a text block starts as no variable (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Before)), UnrealMcpRqlSpecHelpers::CellText(Before), FString(TEXT("false")));
			Refuses(TEXT("a graph cannot read it yet"), /* unreal.asset("@WBP@").EventGraph | call AddVariableGet("Label") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]}},"stages":[{"op":"call","method":"AddVariableGet","args":[{"lit":"Label"}]}]})RQL"), TEXT("set IsVariable = true"));

			Must(TEXT("made a variable"), /* unreal.asset("@WBP@").Widgets | where Name == "Label" | set IsVariable = true */ TEXT(R"RQL({"rql":"0.1","source":{"path":["Widgets"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]}},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Label"}]}},{"op":"set","assign":[{"target":{"path":["IsVariable"]},"value":{"lit":true}}]}]})RQL"));
			UWidget* Label = Made->WidgetTree->FindWidget(TEXT("Label"));
			TestTrue(TEXT("the widget is a variable"), Label && Label->bIsVariable);
			const FString Get = ResultRef(Must(TEXT("a graph reads it"), /* unreal.asset("@WBP@").EventGraph | call AddVariableGet("Label") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]}},"stages":[{"op":"call","method":"AddVariableGet","args":[{"lit":"Label"}]}]})RQL")));
			TestTrue(TEXT("a get node"), Get.StartsWith(TEXT("K2Node_VariableGet#")));
			const TSharedPtr<FJsonObject> Compiled = Must(TEXT("compile"), /* unreal.asset("@WBP@") | call Compile() */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@WBP@"}]},"stages":[{"op":"call","method":"Compile","args":[]}]})RQL"));
			const FString CompileResult = UnrealMcpRqlSpecHelpers::CellText(Compiled, 0, 2);
			TestTrue(*FString::Printf(TEXT("compiles (%s)"), *CompileResult), CompileResult.Contains(TEXT("\"Succeeded\":true")));
			TestNotNull(TEXT("the generated class has the member"), FindFProperty<FObjectProperty>(Made->GeneratedClass, TEXT("Label")));

			if (!GEditor)
				return;
			GEditor->UndoTransaction(); // the compile call's step
			GEditor->UndoTransaction(); // the get node
			GEditor->UndoTransaction(); // Is Variable
			Label = Made->WidgetTree->FindWidget(TEXT("Label"));
			TestTrue(TEXT("undo makes it no variable again"), Label && !Label->bIsVariable);
		});
	});

	Describe("levels", [this]()
	{
		It("copies a level to a file that opens at once", [this]()
		{
			const FString Level = FString::Printf(TEXT("/Game/RqlSpecData/LV_Spec%d_%d"), Serial, FMath::Rand());
			const FString Copy = Level + TEXT("_Copy");
			// The other specs expect the editor's own level back (a saved map): reopen it at the end.
			UWorld* Start = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const FString Original = Start ? Start->GetOutermost()->GetName() : FString();
			Must(TEXT("a saved level"), /* editor | call NewLevel("@LV@") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"NewLevel","args":[{"lit":"@LV@"}]}]})RQL"), { { TEXT("LV"), Level } });
			Must(TEXT("copied"), /* editor | call CopyAsset("@LV@", "@CP@") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CopyAsset","args":[{"lit":"@LV@"},{"lit":"@CP@"}]}]})RQL"), { { TEXT("LV"), Level }, { TEXT("CP"), Copy } });
			const FString File = FPackageName::LongPackageNameToFilename(Copy, FPackageName::GetMapPackageExtension());
			TestTrue(TEXT("the copy is saved"), IFileManager::Get().FileExists(*File));
			Must(TEXT("the copy opens"), /* editor | call OpenLevel("@CP@") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"OpenLevel","args":[{"lit":"@CP@"}]}]})RQL"), { { TEXT("CP"), Copy } });
			UWorld* Opened = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			TestTrue(TEXT("the editor shows the copy"), Opened && Opened->GetOutermost()->GetName() == Copy);

			if (!Original.IsEmpty() && FPackageName::DoesPackageExist(Original))
				UEditorLoadingAndSavingUtils::LoadMap(Original);
			else
				Must(TEXT("an unsaved level again"), /* editor | call NewLevel("") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"NewLevel","args":[{"lit":""}]}]})RQL"));
			Run(/* editor | call DeleteAsset("@LV@", true) */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"DeleteAsset","args":[{"lit":"@LV@"},{"lit":true}]}]})RQL"), true, { { TEXT("LV"), Level } });
			Run(/* editor | call DeleteAsset("@CP@", true) */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"DeleteAsset","args":[{"lit":"@CP@"},{"lit":true}]}]})RQL"), true, { { TEXT("CP"), Copy } });
		});
	});

	Describe("Level Sequences", [this]()
	{
		It("binds actors, adds tracks and sections, keys channels and edits ranges", [this]()
		{
			ULevelSequence* Sequence = MakeSequence();
			Must(TEXT("playback range"), /* unreal.asset("@LS@") | set Start = 0, End = 150 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Start"]},"value":{"lit":0}},{"target":{"path":["End"]},"value":{"lit":150}}]}]})RQL"));
			const TSharedPtr<FJsonObject> Range = Run(/* unreal.asset("@LS@") | select DisplayRate Start End */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"select","fields":[{"name":"DisplayRate","expr":{"path":["DisplayRate"]}},{"name":"Start","expr":{"path":["Start"]}},{"name":"End","expr":{"path":["End"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("30 fps (%s)"), *UnrealMcpRqlSpecHelpers::CellText(Range, 0, 0)), UnrealMcpRqlSpecHelpers::CellText(Range, 0, 0).Contains(TEXT("30")));
			TestEqual(TEXT("start"), UnrealMcpRqlSpecHelpers::CellText(Range, 0, 1), FString(TEXT("0")));
			TestEqual(TEXT("end"), UnrealMcpRqlSpecHelpers::CellText(Range, 0, 2), FString(TEXT("150")));
			Refuses(TEXT("start after end"), /* unreal.asset("@LS@") | set Start = 200 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Start"]},"value":{"lit":200}}]}]})RQL"), TEXT("Start must be before End"));

			Must(TEXT("an actor"), /* editor | call SpawnActor("PointLight", "RqlSeqLamp") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSeqLamp"}]}]})RQL"));
			const TSharedPtr<FJsonObject> Bound = Must(TEXT("bound"), /* unreal.asset("@LS@") | call AddActor(unreal.find("RqlSeqLamp")) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"call","method":"AddActor","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSeqLamp"}]}]}]})RQL"));
			TestTrue(TEXT("returns the binding"), UnrealMcpRqlSpecHelpers::CellText(Bound, 0, 2).Contains(TEXT("RqlSeqLamp")) || UnrealMcpRqlSpecHelpers::CellText(Bound, 0, 2).StartsWith(TEXT("SequenceBinding")));
			Refuses(TEXT("bound twice"), /* unreal.asset("@LS@") | call AddActor(unreal.find("RqlSeqLamp")) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"call","method":"AddActor","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSeqLamp"}]}]}]})RQL"), TEXT("already bound"));
			const TSharedPtr<FJsonObject> Bindings = Run(/* unreal.asset("@LS@") | get Bindings | select Name Kind Class */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Kind","expr":{"path":["Kind"]}},{"name":"Class","expr":{"path":["Class"]}}]}]})RQL"));
			TestEqual(TEXT("binding name"), UnrealMcpRqlSpecHelpers::CellText(Bindings, 0, 0), FString(TEXT("RqlSeqLamp")));
			TestEqual(TEXT("possessable"), UnrealMcpRqlSpecHelpers::CellText(Bindings, 0, 1), FString(TEXT("Possessable")));
			TestEqual(TEXT("class"), UnrealMcpRqlSpecHelpers::CellText(Bindings, 0, 2), FString(TEXT("PointLight")));

			Must(TEXT("transform track"), /* unreal.asset("@LS@") | get Bindings | call AddTrack("Transform") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"call","method":"AddTrack","args":[{"lit":"Transform"}]}]})RQL"));
			Refuses(TEXT("not a track class"), /* unreal.asset("@LS@") | get Bindings | call AddTrack("PointLight") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"call","method":"AddTrack","args":[{"lit":"PointLight"}]}]})RQL"), TEXT("not a track class"));
			const TSharedPtr<FJsonObject> Section = Run(/* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | select Start End */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"select","fields":[{"name":"Start","expr":{"path":["Start"]}},{"name":"End","expr":{"path":["End"]}}]}]})RQL"));
			TestEqual(TEXT("section over the playback range"), UnrealMcpRqlSpecHelpers::CellText(Section, 0, 0), FString(TEXT("0")));
			TestEqual(TEXT("section end"), UnrealMcpRqlSpecHelpers::CellText(Section, 0, 1), FString(TEXT("150")));

			Must(TEXT("a key"), /* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | call AddKey("Location.X", 30, 100) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"Location.X"},{"lit":30},{"lit":100}]}]})RQL"));
			Must(TEXT("another key"), /* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | call AddKey("location.x", 60, 250.5) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"location.x"},{"lit":60},{"lit":250.5}]}]})RQL"));
			const TSharedPtr<FJsonObject> Channels = Run(/* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | select Channels */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"select","fields":[{"name":"Channels","expr":{"path":["Channels"]}}]}]})RQL"));
			const FString ChannelText = UnrealMcpRqlSpecHelpers::CellText(Channels);
			TestTrue(*FString::Printf(TEXT("keys read back (%s)"), *ChannelText.Left(400)), ChannelText.Contains(TEXT("\"Frame\":30")) && ChannelText.Contains(TEXT("250.5")));
			Refuses(TEXT("unknown channel"), /* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | call AddKey("Nope", 1, 1) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"Nope"},{"lit":1},{"lit":1}]}]})RQL"), TEXT("Location.X"));
			Refuses(TEXT("a text value"), /* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | call AddKey("Location.X", 1, "far") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"Location.X"},{"lit":1},{"lit":"far"}]}]})RQL"), TEXT("a number"));
			Must(TEXT("an internal channel name keys the same channel"), /* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | call AddKey("Rotation.Z", 10, 45) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"Rotation.Z"},{"lit":10},{"lit":45}]}]})RQL"));
			const FString Yaw = UnrealMcpRqlSpecHelpers::CellText(Run(/* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | get Channels | where Name == "Rotation.Yaw" | select Keys */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"get","expr":{"path":["Channels"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Rotation.Yaw"}]}},{"op":"select","fields":[{"name":"Keys","expr":{"path":["Keys"]}}]}]})RQL")));
			TestTrue(*FString::Printf(TEXT("Rotation.Z is the yaw (%s)"), *Yaw), Yaw.Contains(TEXT("\"Frame\":10")) && Yaw.Contains(TEXT("\"Value\":45")));
			Must(TEXT("a key past the end grows the section"), /* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | call AddKey("Location.Y", 200, 5) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"Location.Y"},{"lit":200},{"lit":5}]}]})RQL"));
			const TSharedPtr<FJsonObject> Grown = Run(/* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | select End */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"select","fields":[{"name":"End","expr":{"path":["End"]}}]}]})RQL"));
			TestTrue(TEXT("section grown"), FCString::Atoi(*UnrealMcpRqlSpecHelpers::CellText(Grown)) >= 200);
			Must(TEXT("section range"), /* unreal.asset("@LS@") | get Bindings | get Tracks | get Sections | set Start = 10 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"set","assign":[{"target":{"path":["Start"]},"value":{"lit":10}}]}]})RQL"));

			Must(TEXT("visibility"), /* unreal.asset("@LS@") | get Bindings | call AddTrack("Visibility") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"call","method":"AddTrack","args":[{"lit":"Visibility"}]}]})RQL"));
			const TSharedPtr<FJsonObject> Bool = Run(/* unreal.asset("@LS@") | get Bindings | get Tracks | where $type == "MovieSceneVisibilityTrack" | get Sections | select Channels */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieSceneVisibilityTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"select","fields":[{"name":"Channels","expr":{"path":["Channels"]}}]}]})RQL"));
			const TSharedPtr<FJsonObject> BoolName = UnrealMcpRqlSpecHelpers::Parse(UnrealMcpRqlSpecHelpers::CellText(Bool).Mid(1).LeftChop(1));
			const FString Channel = BoolName.IsValid() ? BoolName->GetStringField(TEXT("Name")) : FString();
			Must(TEXT("a bool key"), /* unreal.asset("@LS@") | get Bindings | get Tracks | where $type == "MovieSceneVisibilityTrack" | get Sections | call AddKey("@C@", 15, false) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieSceneVisibilityTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"@C@"},{"lit":15},{"lit":false}]}]})RQL"), { { TEXT("C"), Channel } });
			const TSharedPtr<FJsonObject> BoolKeys = Run(/* unreal.asset("@LS@") | get Bindings | get Tracks | where $type == "MovieSceneVisibilityTrack" | get Sections | select Channels */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieSceneVisibilityTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"select","fields":[{"name":"Channels","expr":{"path":["Channels"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("bool key (%s)"), *UnrealMcpRqlSpecHelpers::CellText(BoolKeys)), UnrealMcpRqlSpecHelpers::CellText(BoolKeys).Contains(TEXT("\"Value\":false")));

			Must(TEXT("an unbound track"), /* unreal.asset("@LS@") | call AddTrack("CameraCut") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"call","method":"AddTrack","args":[{"lit":"CameraCut"}]}]})RQL"));
			const TSharedPtr<FJsonObject> Tracks = Run(/* unreal.asset("@LS@") | get Tracks | select $type */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Tracks"]}},{"op":"select","fields":[{"name":"$type","expr":{"path":[{"meta":"type"}]}}]}]})RQL"));
			TestEqual(TEXT("camera cut track"), UnrealMcpRqlSpecHelpers::CellText(Tracks), FString(TEXT("MovieSceneCameraCutTrack")));
			const TSharedPtr<FJsonObject> Listed = Run(/* levelSequences | where Path contains "@LS@" | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["levelSequences"]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Path"]},{"lit":"@LS@"}]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("levelSequences source"), UnrealMcpRqlSpecHelpers::Number(Listed), 1.0);
			TestTrue(TEXT("the sequence is dirty"), Sequence->GetOutermost()->IsDirty());

			if (!GEditor)
				return;
			Must(TEXT("a key to undo"), /* unreal.asset("@LS@") | get Bindings | get Tracks | where $type == "MovieScene3DTransformTrack" | get Sections | call AddKey("Location.Z", 90, 7) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieScene3DTransformTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"Location.Z"},{"lit":90},{"lit":7}]}]})RQL"));
			GEditor->UndoTransaction();
			const TSharedPtr<FJsonObject> Undone = Run(/* unreal.asset("@LS@") | get Bindings | get Tracks | where $type == "MovieScene3DTransformTrack" | get Sections | select Channels */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieScene3DTransformTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"select","fields":[{"name":"Channels","expr":{"path":["Channels"]}}]}]})RQL"));
			TestFalse(TEXT("undo removes the key"), UnrealMcpRqlSpecHelpers::CellText(Undone).Contains(TEXT("\"Frame\":90")));
		});

		It("animates a property: the property track of its type on the component's binding, keyed by the property name", [this]()
		{
			ULevelSequence* Sequence = MakeSequence();
			Must(TEXT("an actor"), /* editor | call SpawnActor("PointLight", "RqlSeqLamp") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlSeqLamp"}]}]})RQL"));
			Must(TEXT("bound"), /* unreal.asset("@LS@") | call AddActor(unreal.find("RqlSeqLamp")) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"call","method":"AddActor","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlSeqLamp"}]}]}]})RQL"));
			// A property track class alone animates nothing: refused on a binding and unbound, naming the property form.
			Refuses(TEXT("a bare property track class"), /* unreal.asset("@LS@") | get Bindings | where Name == "RqlSeqLamp" | call AddTrack("MovieSceneFloatTrack") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSeqLamp"}]}},{"op":"call","method":"AddTrack","args":[{"lit":"MovieSceneFloatTrack"}]}]})RQL"), TEXT("AddTrack(\"Intensity\")"));
			Refuses(TEXT("an unbound property track"), /* unreal.asset("@LS@") | call AddTrack("MovieSceneFloatTrack") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"call","method":"AddTrack","args":[{"lit":"MovieSceneFloatTrack"}]}]})RQL"), TEXT("bind the actor"));
			// An unknown name lists what can be animated (the light component's Interp properties).
			Refuses(TEXT("an unknown property"), /* unreal.asset("@LS@") | get Bindings | where Name == "RqlSeqLamp" | call AddTrack("NoSuchThing") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSeqLamp"}]}},{"op":"call","method":"AddTrack","args":[{"lit":"NoSuchThing"}]}]})RQL"), TEXT("animatable properties: "));

			const TSharedPtr<FJsonObject> Added = Must(TEXT("intensity"), /* unreal.asset("@LS@") | get Bindings | where Name == "RqlSeqLamp" | call AddTrack("Intensity") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSeqLamp"}]}},{"op":"call","method":"AddTrack","args":[{"lit":"Intensity"}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("returns a float track (%s)"), *UnrealMcpRqlSpecHelpers::CellText(Added, 0, 2)), UnrealMcpRqlSpecHelpers::CellText(Added, 0, 2).StartsWith(TEXT("MovieSceneFloatTrack")));
			Must(TEXT("a component property by path"), /* unreal.asset("@LS@") | get Bindings | where Name == "RqlSeqLamp" | call AddTrack("LightComponent0.LightColor") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSeqLamp"}]}},{"op":"call","method":"AddTrack","args":[{"lit":"LightComponent0.LightColor"}]}]})RQL"));
			Refuses(TEXT("the same property twice"), /* unreal.asset("@LS@") | get Bindings | where Name == "RqlSeqLamp" | call AddTrack("Intensity") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSeqLamp"}]}},{"op":"call","method":"AddTrack","args":[{"lit":"Intensity"}]}]})RQL"), TEXT("already has"));

			// The movie scene: one child possessable for the component, holding both property tracks, bound to their properties.
			UMovieScene* Scene = Sequence->GetMovieScene();
			const FMovieScenePossessable* Component = nullptr;
			for (int32 Index = 0; Index < Scene->GetPossessableCount(); ++Index)
				if (Scene->GetPossessable(Index).GetParent().IsValid())
					Component = &Scene->GetPossessable(Index);
			if (!TestNotNull(TEXT("a component binding under the actor"), Component))
				return;
			TestEqual(TEXT("named after the component"), Component->GetName(), FString(TEXT("LightComponent0")));
			const FMovieSceneBinding* Binding = Scene->FindBinding(Component->GetGuid());
			TArray<FString> Bound;
			for (UMovieSceneTrack* Track : Binding ? Binding->GetTracks() : TArray<UMovieSceneTrack*>())
				if (const UMovieScenePropertyTrack* Property = Cast<UMovieScenePropertyTrack>(Track))
					Bound.Add(Track->GetClass()->GetName() + TEXT(":") + Property->GetPropertyName().ToString());
			Bound.Sort();
			TestEqual(TEXT("tracks bound to their properties"), FString::Join(Bound, TEXT(",")), FString(TEXT("MovieSceneColorTrack:LightColor,MovieSceneFloatTrack:Intensity")));
			if (!GEditor)
				return;
			AActor* Lamp = nullptr;
			for (TActorIterator<AActor> It(GEditor->GetEditorWorldContext().World()); It; ++It)
				if (It->GetActorLabel() == TEXT("RqlSeqLamp"))
					Lamp = *It;
			UActorComponent* Light = Lamp ? Lamp->GetComponentByClass<UPointLightComponent>() : nullptr;
			TestTrue(TEXT("the component binding resolves to the light component"), Light && Sequence->FindBindingFromObject(Light, Lamp->GetWorld()) == Component->GetGuid());

			Must(TEXT("key at 0"), /* unreal.asset("@LS@") | get Bindings | where Name == "LightComponent0" | get Tracks | where $type == "MovieSceneFloatTrack" | get Sections | call AddKey("Intensity", 0, 0) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"LightComponent0"}]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieSceneFloatTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"Intensity"},{"lit":0},{"lit":0}]}]})RQL"));
			Must(TEXT("key at 60"), /* unreal.asset("@LS@") | get Bindings | where Name == "LightComponent0" | get Tracks | where $type == "MovieSceneFloatTrack" | get Sections | call AddKey("Intensity", 60, 5000) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"LightComponent0"}]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieSceneFloatTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"call","method":"AddKey","args":[{"lit":"Intensity"},{"lit":60},{"lit":5000}]}]})RQL"));
			const FString Channels = UnrealMcpRqlSpecHelpers::CellText(Run(/* unreal.asset("@LS@") | get Bindings | where Name == "LightComponent0" | get Tracks | where $type == "MovieSceneFloatTrack" | get Sections | select Channels */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"LightComponent0"}]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieSceneFloatTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"select","fields":[{"name":"Channels","expr":{"path":["Channels"]}}]}]})RQL")));
			TestTrue(*FString::Printf(TEXT("the float channel is named Intensity and holds both keys (%s)"), *Channels.Left(600)),
				Channels.Contains(TEXT("\"Name\":\"Intensity\"")) && Channels.Contains(TEXT("\"Frame\":60")) && Channels.Contains(TEXT("5000")));

			// Visibility keys the actor's bHidden, as the Sequencer's Visibility track does.
			Must(TEXT("visibility"), /* unreal.asset("@LS@") | get Bindings | where Name == "RqlSeqLamp" | call AddTrack("Visibility") */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSeqLamp"}]}},{"op":"call","method":"AddTrack","args":[{"lit":"Visibility"}]}]})RQL"));
			const FMovieSceneBinding* ActorBinding = Scene->FindBinding(Component->GetParent());
			const UMovieSceneVisibilityTrack* Visibility = nullptr;
			for (UMovieSceneTrack* Track : ActorBinding ? ActorBinding->GetTracks() : TArray<UMovieSceneTrack*>())
				if (const UMovieSceneVisibilityTrack* Found = Cast<UMovieSceneVisibilityTrack>(Track))
					Visibility = Found;
			TestTrue(TEXT("visibility bound to bHidden"), Visibility && Visibility->GetPropertyName() == FName(TEXT("bHidden")));
			// Its bool is inverted (true = visible), so the channel is named Visible, not after bHidden.
			const FString VisibleChannel = UnrealMcpRqlSpecHelpers::CellText(Run(/* unreal.asset("@LS@") | get Bindings | where Name == "RqlSeqLamp" | get Tracks | where $type == "MovieSceneVisibilityTrack" | get Sections | select Channels */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@LS@"}]},"stages":[{"op":"get","expr":{"path":["Bindings"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"RqlSeqLamp"}]}},{"op":"get","expr":{"path":["Tracks"]}},{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"MovieSceneVisibilityTrack"}]}},{"op":"get","expr":{"path":["Sections"]}},{"op":"select","fields":[{"name":"Channels","expr":{"path":["Channels"]}}]}]})RQL")));
			TestTrue(*FString::Printf(TEXT("the visibility channel is Visible (%s)"), *VisibleChannel), VisibleChannel.Contains(TEXT("\"Name\":\"Visible\"")));

			if (!GEditor)
				return;
			const int32 Before = Scene->GetPossessableCount();
			GEditor->UndoTransaction(); // visibility
			GEditor->UndoTransaction(); // key at 60
			GEditor->UndoTransaction(); // key at 0
			GEditor->UndoTransaction(); // light color
			GEditor->UndoTransaction(); // intensity (and the component binding)
			TestEqual(TEXT("undo removes the component binding"), Scene->GetPossessableCount(), Before - 1);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
