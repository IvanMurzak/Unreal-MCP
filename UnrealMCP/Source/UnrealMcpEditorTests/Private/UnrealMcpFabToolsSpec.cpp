// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/CustomVersion.h"
#include "UObject/ObjectVersion.h"
#include "UObject/UE5MainStreamObjectVersion.h"
#include "Misc/ScopeExit.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/App.h"
#include "UObject/Package.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpFabSignIn.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * The Fab family's offline logic: listing uid parsing, the account id inside a sign-in token, the distribution method
 * and version choice of an unreal-engine format, and the argument checks that answer before any network call. The
 * network and import paths (fab.com, the engine's Fab plugin) are live-tested, not here.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpFabToolsSpec, "UnrealMcp.Tools.Fab",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	static TSharedPtr<FJsonValue> FabSpecVersion(const FString& Uid, const TArray<FString>& Engines)
	{
		TSharedPtr<FJsonObject> Version = MakeShared<FJsonObject>();
		Version->SetStringField(TEXT("uid"), Uid);
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Engine : Engines)
			Values.Add(MakeShared<FJsonValueString>(Engine));
		Version->SetArrayField(TEXT("engineVersions"), Values);
		return MakeShared<FJsonValueObject>(Version);
	}

	static FString FabSpecEngine(int32 MinorOffset)
	{
		return FString::Printf(TEXT("UE_%d.%d"), FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor() + MinorOffset);
	}

	/**
	 * Reading a missing token logs errors of the Fab plugin and the EOS SDK; they are the expected path here.
	 * Any number of them, none included: Occurrences < 0 means "ignore silently" from UE 5.6 on. UE 5.5 refuses a
	 * negative count (and 0 there means "one or more"), so 5.5 adds no expectation; its Fab plugin logs none here.
	 */
	void ExpectFabTokenReadErrors()
	{
		const FEngineVersion& Engine = FEngineVersion::Current();
		if (Engine.GetMajor() > 5 || (Engine.GetMajor() == 5 && Engine.GetMinor() >= 6))
		{
			AddExpectedErrorPlain(TEXT("unable to get auth token"), EAutomationExpectedErrorFlags::Contains, -1);
			AddExpectedErrorPlain(TEXT("EOS_Auth_CopyUserAuthToken"), EAutomationExpectedErrorFlags::Contains, -1);
		}
	}

	/** Write a package header a newer engine saved (tag, legacy version, UE3 version, UE4 / UE5 file versions) to @p Filename. */
	static bool FabSpecWriteNewerPackage(const FString& Filename)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		int32 Tag = (int32)PACKAGE_FILE_TAG, Legacy = -8, LegacyUE3 = 864;
		int32 UE4 = GPackageFileUEVersion.FileVersionUE4, UE5 = GPackageFileUEVersion.FileVersionUE5 + 1;
		Writer << Tag << Legacy << LegacyUE3 << UE4 << UE5;
		Bytes.AddZeroed(256);
		return FFileHelper::SaveArrayToFile(Bytes, *Filename);
	}

	/** @p Tool's answer while UnrealMcpFabSignIn reports @p Service and no sign-in (the real hook is restored after). */
	static FUnrealMcpToolResult FabSpecRunWithService(EUnrealMcpFabSignInService Service, const TCHAR* Tool)
	{
		const FUnrealMcpFabSignIn Saved = UnrealMcpFabSignIn();
		ON_SCOPE_EXIT { UnrealMcpFabSignIn() = Saved; };
		UnrealMcpFabSignIn().Service = [Service]() { return Service; };
		UnrealMcpFabSignIn().Tokens = [](FString&, FString&) { return false; };
		FUnrealMcpToolRegistry Registry;
		UnrealMcpFabTools::Register(Registry);
		return Registry.Execute(Tool, FUnrealMcpToolCall(MakeShared<FJsonObject>()));
	}

END_DEFINE_SPEC(FUnrealMcpFabToolsSpec)

void FUnrealMcpFabToolsSpec::Define()
{
	It("parses a listing uid from a uid or a fab.com listing URL", [this]()
	{
		const FString Uid = TEXT("a8b53a5e-1804-49d2-adad-b9b171d38dc2");
		TestEqual(TEXT("plain uid"), UnrealMcpFabTools::ParseListingUid(Uid), Uid);
		TestEqual(TEXT("upper case + spaces"), UnrealMcpFabTools::ParseListingUid(TEXT("  ") + Uid.ToUpper() + TEXT(" ")), Uid);
		TestEqual(TEXT("listing URL"), UnrealMcpFabTools::ParseListingUid(TEXT("https://www.fab.com/listings/") + Uid), Uid);
		TestEqual(TEXT("URL with query"), UnrealMcpFabTools::ParseListingUid(TEXT("https://www.fab.com/listings/") + Uid + TEXT("?x=1")), Uid);
		TestEqual(TEXT("URL with path"), UnrealMcpFabTools::ParseListingUid(TEXT("https://www.fab.com/listings/") + Uid + TEXT("/reviews")), Uid);
		TestTrue(TEXT("a title is no uid"), UnrealMcpFabTools::ParseListingUid(TEXT("Houseplant Pack")).IsEmpty());
		TestTrue(TEXT("empty"), UnrealMcpFabTools::ParseListingUid(FString()).IsEmpty());
	});

	It("reads the account id from an EOS token and nothing from garbage", [this]()
	{
		FString Payload = FBase64::Encode(FString(TEXT("{\"sub\":\"0123abcd\",\"iss\":\"x\"}")));
		Payload = Payload.Replace(TEXT("="), TEXT("")).Replace(TEXT("+"), TEXT("-")).Replace(TEXT("/"), TEXT("_"));
		TestEqual(TEXT("eg1 token"), UnrealMcpFabTools::AccountIdFromToken(TEXT("eg1~eyJhbGciOiJ.") + Payload + TEXT(".sig")), FString(TEXT("0123abcd")));
		TestEqual(TEXT("bare JWT"), UnrealMcpFabTools::AccountIdFromToken(TEXT("h.") + Payload + TEXT(".s")), FString(TEXT("0123abcd")));
		TestTrue(TEXT("garbage"), UnrealMcpFabTools::AccountIdFromToken(TEXT("not-a-token")).IsEmpty());
		TestTrue(TEXT("empty"), UnrealMcpFabTools::AccountIdFromToken(FString()).IsEmpty());
	});

	It("reads the distribution method from the format's techDetails", [this]()
	{
		TSharedPtr<FJsonObject> Format = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetField(TEXT("threeDModel"), MakeShared<FJsonValueNull>());
		TSharedPtr<FJsonObject> Tutorials = MakeShared<FJsonObject>();
		Tutorials->SetStringField(TEXT("distributionMethod"), TEXT("COMPLETE_PROJECT"));
		Details->SetObjectField(TEXT("tutorialsExamples"), Tutorials);
		Format->SetObjectField(TEXT("techDetails"), Details);
		TestEqual(TEXT("techDetails"), UnrealMcpFabTools::DistributionMethod(*Format), FString(TEXT("complete_project")));
		Format->SetStringField(TEXT("distributionMethod"), TEXT("code_plugin"));
		TestEqual(TEXT("top-level field wins"), UnrealMcpFabTools::DistributionMethod(*Format), FString(TEXT("code_plugin")));
		TestTrue(TEXT("none"), UnrealMcpFabTools::DistributionMethod(FJsonObject()).IsEmpty());
	});

	It("picks this editor's version, else the newest older one, else the oldest newer one", [this]()
	{
		TArray<TSharedPtr<FJsonValue>> Versions = {
			FabSpecVersion(TEXT("older2"), { FabSpecEngine(-2) }),
			FabSpecVersion(TEXT("exact"), { FabSpecEngine(-1), FabSpecEngine(0) }),
			FabSpecVersion(TEXT("newer"), { FabSpecEngine(1) }) };
		TestEqual(TEXT("exact match"), UnrealMcpFabTools::PickUnrealVersion(Versions), FString(TEXT("exact")));
		Versions = { FabSpecVersion(TEXT("older2"), { FabSpecEngine(-2) }), FabSpecVersion(TEXT("older1"), { FabSpecEngine(-1) }),
			FabSpecVersion(TEXT("newer"), { FabSpecEngine(1) }) };
		TestEqual(TEXT("newest older"), UnrealMcpFabTools::PickUnrealVersion(Versions), FString(TEXT("older1")));
		Versions = { FabSpecVersion(TEXT("newer2"), { FabSpecEngine(2) }), FabSpecVersion(TEXT("newer1"), { FabSpecEngine(1) }) };
		TestEqual(TEXT("oldest newer"), UnrealMcpFabTools::PickUnrealVersion(Versions), FString(TEXT("newer1")));
		TestTrue(TEXT("no versions"), UnrealMcpFabTools::PickUnrealVersion({}).IsEmpty());
	});

	It("tells the agent to ask the user and use fab-sign-in when no Fab sign-in exists (DEV-273)", [this]()
	{
		const FString Text = UnrealMcpFabTools::SignInRequiredText(TEXT("fab-add"));
		TestTrue(TEXT("says nothing was opened"), Text.Contains(TEXT("no browser was opened")));
		TestTrue(TEXT("asks the user in the chat"), Text.Contains(TEXT("AskUserQuestion")));
		TestTrue(TEXT("names the sign-in tool"), Text.Contains(TEXT("fab-sign-in")));
		TestTrue(TEXT("names the tool to call again"), Text.Contains(TEXT("call fab-add again")));
		TestTrue(TEXT("covers a run nobody can answer"), Text.Contains(TEXT("If you cannot ask the user, do not call fab-sign-in")));
		TestTrue(TEXT("fab-library names itself"), UnrealMcpFabTools::SignInRequiredText(TEXT("fab-library")).Contains(TEXT("call fab-library again")));
	});

	It("says in fab-search whether adding needs a sign-in first (DEV-273)", [this]()
	{
		const FString SignedOut = UnrealMcpFabTools::SearchFooter(false);
		TestTrue(TEXT("signed out: says so"), SignedOut.Contains(TEXT("Not signed in to Fab")));
		TestTrue(TEXT("signed out: names fab-sign-in"), SignedOut.Contains(TEXT("fab-sign-in")));
		TestTrue(TEXT("signed out: asks the user"), SignedOut.Contains(TEXT("AskUserQuestion")));
		const FString SignedIn = UnrealMcpFabTools::SearchFooter(true);
		TestFalse(TEXT("signed in: no sign-in warning"), SignedIn.Contains(TEXT("Not signed in")));
		TestTrue(TEXT("signed in: ownership rule"), SignedIn.Contains(TEXT("fab-library lists them")));
	});

	It("answers fab-library at once without a sign-in instead of opening a browser (DEV-273)", [this]()
	{
		ExpectFabTokenReadErrors();
		FUnrealMcpToolRegistry Registry;
		UnrealMcpFabTools::Register(Registry);
		const FUnrealMcpToolResult Result = Registry.Execute(TEXT("fab-library"), FUnrealMcpToolCall(MakeShared<FJsonObject>()));
		// A host without the Fab plugin answers that first; a signed-in editor (or a sign-in in progress) reads the library.
		if (Result.Message.Contains(TEXT("Fab plugin is not loaded")) || Result.bRetryLater || Result.bSuccess)
			return;
		TestTrue(TEXT("says the sign-in is missing"), Result.Message.Contains(TEXT("Not signed in to Fab")));
		TestTrue(TEXT("opened nothing"), Result.Message.Contains(TEXT("no browser was opened")));
	});

	It("fails at once, opening nothing, when Epic's sign-in service never started (DEV-281)", [this]()
	{
		ExpectFabTokenReadErrors();
		const FUnrealMcpToolResult SignIn = FabSpecRunWithService(EUnrealMcpFabSignInService::NotStarted, TEXT("fab-sign-in"));
		const FUnrealMcpToolResult Library = FabSpecRunWithService(EUnrealMcpFabSignInService::NotStarted, TEXT("fab-library"));
		// A host without the Fab plugin answers that first; one signed in through the Fab plugin itself is signed in.
		if (SignIn.Message.Contains(TEXT("Fab plugin is not loaded")) || SignIn.bSuccess)
			return;
		TestFalse(TEXT("fab-sign-in does not wait"), SignIn.bRetryLater);
		TestTrue(TEXT("fab-sign-in says why"), SignIn.Message.Contains(TEXT("sign-in service did not start")));
		TestTrue(TEXT("fab-sign-in opened nothing"), SignIn.Message.Contains(TEXT("no browser was opened")));
		TestFalse(TEXT("fab-library does not wait"), Library.bRetryLater || Library.bSuccess);
		TestTrue(TEXT("fab-library says why"), Library.Message.Contains(TEXT("sign-in service did not start")));
	});

	It("waits while the remembered sign-in is being restored (DEV-281)", [this]()
	{
		ExpectFabTokenReadErrors();
		const FUnrealMcpToolResult SignIn = FabSpecRunWithService(EUnrealMcpFabSignInService::Restoring, TEXT("fab-sign-in"));
		if (SignIn.Message.Contains(TEXT("Fab plugin is not loaded")) || SignIn.bSuccess)
			return;
		TestTrue(TEXT("waits for the restore"), SignIn.bRetryLater);
		TestTrue(TEXT("says what it waits for"), SignIn.Message.Contains(TEXT("Restoring")));
	});

	It("fab-search waits for a restore in flight instead of reporting signedIn:false (DEV-293)", [this]()
	{
		ExpectFabTokenReadErrors();
		const FUnrealMcpToolResult Search = FabSpecRunWithService(EUnrealMcpFabSignInService::Restoring, TEXT("fab-search"));
		TestTrue(TEXT("waits, no answer yet"), Search.bRetryLater);
		// An editor signed in through the Fab plugin itself does not wait: it starts the search ("Searching Fab.").
		if (!Search.Message.Contains(TEXT("Searching Fab")))
			TestTrue(TEXT("says what it waits for"), Search.Message.Contains(TEXT("remembered Fab sign-in")));
		const FString Footer = UnrealMcpFabTools::SearchFooter(false, EUnrealMcpFabSignInService::Restoring);
		TestTrue(TEXT("time out while restoring: call again"), Footer.Contains(TEXT("call fab-search again")));
		TestFalse(TEXT("time out while restoring: no sign-in prompt"), Footer.Contains(TEXT("AskUserQuestion whether to sign in")));
		TestTrue(TEXT("signed in: the restore is no news"),
			UnrealMcpFabTools::SearchFooter(true, EUnrealMcpFabSignInService::Restoring).Contains(TEXT("fab-library lists them")));
		TestTrue(TEXT("service down: plan without Fab"),
			UnrealMcpFabTools::SearchFooter(false, EUnrealMcpFabSignInService::NotStarted).Contains(TEXT("plan without Fab assets")));
	});

	It("rejects bad arguments before any network call", [this]()
	{
		FUnrealMcpToolRegistry Registry;
		UnrealMcpFabTools::Register(Registry);
		auto Run = [&Registry](const TCHAR* Tool, TFunction<void(FJsonObject&)> Configure)
		{
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Configure(*Args);
			return Registry.Execute(Tool, FUnrealMcpToolCall(Args));
		};
		FUnrealMcpToolResult Result = Run(TEXT("fab-search"), [](FJsonObject& A) { A.SetStringField(TEXT("sort"), TEXT("cheapest")); });
		TestFalse(TEXT("bad sort fails"), Result.bSuccess || Result.bRetryLater);
		TestTrue(TEXT("bad sort names the options"), Result.Message.Contains(TEXT("relevance")));

		Result = Run(TEXT("fab-add"), [](FJsonObject& A) { A.SetStringField(TEXT("listing"), TEXT("Houseplant Pack")); });
		if (Result.Message.Contains(TEXT("Fab plugin is not loaded")))
			return; // a host without the Fab plugin answers that first
		TestFalse(TEXT("a title is no listing"), Result.bSuccess || Result.bRetryLater);
		TestTrue(TEXT("says what a listing is"), Result.Message.Contains(TEXT("listing uid")));

		Result = Run(TEXT("fab-add"), [](FJsonObject& A)
		{
			A.SetStringField(TEXT("listing"), TEXT("a8b53a5e-1804-49d2-adad-b9b171d38dc2"));
			A.SetStringField(TEXT("quality"), TEXT("ultra"));
		});
		TestFalse(TEXT("bad quality fails"), Result.bSuccess || Result.bRetryLater);
		TestTrue(TEXT("bad quality names the options"), Result.Message.Contains(TEXT("low, medium, high, raw")));
	});
	It("reads the engine versions a listing supports (DEV-290)", [this]()
	{
		const TSharedPtr<FJsonObject> Listing = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Formats;
		{
			TSharedPtr<FJsonObject> Specs = MakeShared<FJsonObject>();
			Specs->SetArrayField(TEXT("unrealEngineEngineVersions"), { MakeShared<FJsonValueString>(TEXT("UE_5.8")),
				MakeShared<FJsonValueString>(TEXT("UE_5.10")), MakeShared<FJsonValueString>(TEXT("UE_5.7")), MakeShared<FJsonValueString>(TEXT("junk")) });
			TSharedPtr<FJsonObject> Format = MakeShared<FJsonObject>();
			Format->SetObjectField(TEXT("technicalSpecs"), Specs);
			Formats.Add(MakeShared<FJsonValueObject>(Format));
			TSharedPtr<FJsonObject> Gltf = MakeShared<FJsonObject>();
			Gltf->SetObjectField(TEXT("technicalSpecs"), MakeShared<FJsonObject>());
			Formats.Add(MakeShared<FJsonValueObject>(Gltf));
		}
		Listing->SetArrayField(TEXT("assetFormats"), Formats);
		TestEqual(TEXT("sorted by version, junk dropped"), UnrealMcpFabTools::ListingEngines(*Listing), TArray<FString>{ TEXT("5.7"), TEXT("5.8"), TEXT("5.10") });
		TestTrue(TEXT("no formats, no engines"), UnrealMcpFabTools::ListingEngines(FJsonObject()).IsEmpty());
	});

	It("tells which package files this engine cannot load (DEV-290)", [this]()
	{
		// An engine package loads.
		FString EngineFile;
		TestTrue(TEXT("engine package has a file"), FPackageName::TryConvertLongPackageNameToFilename(TEXT("/Engine/EngineMaterials/DefaultMaterial"),
			EngineFile, FPackageName::GetAssetPackageExtension()));
		bool bFatal = true;
		TestEqual(TEXT("an engine package loads"), UnrealMcpFabTools::PackageFileProblem(EngineFile, bFatal), FString());
		TestFalse(TEXT("nothing fatal"), bFatal);

		const FString Dir = FPaths::ProjectSavedDir() / TEXT("UnrealMcpFabToolsSpec");
		ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, false, true); };
		const FString NotPackage = Dir / TEXT("NotAPackage.uasset");
		TestTrue(TEXT("wrote a text file"), FFileHelper::SaveStringToFile(TEXT("this is not a package file at all"), *NotPackage));
		TestTrue(TEXT("a text file is no package"), UnrealMcpFabTools::PackageFileProblem(NotPackage, bFatal).Contains(TEXT("not a package file")));
		TestTrue(TEXT("fatal"), bFatal);

		const FString Newer = Dir / TEXT("Newer.uasset");
		TestTrue(TEXT("wrote the header"), FabSpecWriteNewerPackage(Newer));
		const FString Problem = UnrealMcpFabTools::PackageFileProblem(Newer, bFatal);
		TestTrue(TEXT("a newer package is named so"), Problem.Contains(TEXT("newer engine")));
		TestTrue(TEXT("fatal"), bFatal);
	});

	It("names custom versions newer than this engine's and unknown ones (DEV-290)", [this]()
	{
		const TOptional<FCustomVersion> Latest = FCurrentCustomVersions::Get(FUE5MainStreamObjectVersion::GUID);
		if (!TestTrue(TEXT("UE5-Main is registered"), Latest.IsSet()))
			return;
		FCustomVersionArray Versions;
		Versions.Add(FCustomVersion(FUE5MainStreamObjectVersion::GUID, Latest->Version + 2, TEXT("UE5-Main")));
		Versions.Add(FCustomVersion(FGuid(0x1234, 0x5678, 0x9abc, 0xdef0), 3, TEXT("NotRegistered")));
		TArray<FString> Unknown;
		const TArray<FString> TooNew = UnrealMcpFabTools::CustomVersionProblems(Versions, Unknown);
		if (TestEqual(TEXT("one too new"), TooNew.Num(), 1))
			TestTrue(TEXT("says saved and current"), TooNew[0].Contains(FString::Printf(TEXT("%d (this engine: %d)"), Latest->Version + 2, Latest->Version)));
		TestEqual(TEXT("one unknown"), Unknown.Num(), 1);

		Versions = { FCustomVersion(FUE5MainStreamObjectVersion::GUID, Latest->Version, TEXT("UE5-Main")) };
		Unknown.Reset();
		TestTrue(TEXT("the current version is fine"), UnrealMcpFabTools::CustomVersionProblems(Versions, Unknown).IsEmpty() && Unknown.IsEmpty());
	});

	It("maps a package dependency to the plugin it needs (DEV-290)", [this]()
	{
		TestEqual(TEXT("a plugin module"), UnrealMcpFabTools::PluginOfDependency(TEXT("/Script/UnrealMcpEditor")), FString(TEXT("UnrealMCP")));
		TestEqual(TEXT("an engine module"), UnrealMcpFabTools::PluginOfDependency(TEXT("/Script/Engine")), FString());
		TestEqual(TEXT("project content"), UnrealMcpFabTools::PluginOfDependency(TEXT("/Game/Foo/Bar")), FString());
		TestEqual(TEXT("engine content"), UnrealMcpFabTools::PluginOfDependency(TEXT("/Engine/EngineMaterials/DefaultMaterial")), FString());
		if (IPluginManager::Get().FindPlugin(TEXT("Fab")).IsValid())
			TestEqual(TEXT("plugin content"), UnrealMcpFabTools::PluginOfDependency(TEXT("/Fab/Materials/Standard/M_MS_Base")), FString(TEXT("Fab")));
	});
	It("checks an imported folder and moves its instances off a read-only engine base material (DEV-290, DEV-291)", [this]()
	{
		UMaterial* EngineBase = LoadObject<UMaterial>(nullptr, TEXT("/Fab/Materials/Standard/M_MS_Base.M_MS_Base"));
		const FString CopyPackage = TEXT("/Game/Fab/Materials/Standard/M_MS_Base");
		if (!EngineBase || !FApp::IsEngineInstalled() || FPackageName::DoesPackageExist(CopyPackage))
			return; // needs the Fab plugin's content in an installed engine, and a project without that copy yet
		const bool bBaseWasDirty = EngineBase->GetOutermost()->IsDirty();
		const FString Folder = TEXT("/Game/UnrealMcpFabSpec");
		const FString Dir = FPaths::ProjectContentDir() / TEXT("UnrealMcpFabSpec");
		const FString FabDir = FPaths::ProjectContentDir() / TEXT("Fab");
		const bool bHadFabDir = IFileManager::Get().DirectoryExists(*FabDir);
		ON_SCOPE_EXIT
		{
			UEditorAssetLibrary::DeleteDirectory(Folder);
			if (UEditorAssetLibrary::DoesAssetExist(CopyPackage))
				UEditorAssetLibrary::DeleteAsset(CopyPackage);
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
			if (!bHadFabDir)
				IFileManager::Get().DeleteDirectory(*FabDir, false, true); // only the folders this spec made
		};

		// An "import": one instance of the engine's M_MS_Base, saved, and one package file a newer engine wrote.
		UPackage* Package = CreatePackage(*(Folder / TEXT("MI_FabSpec")));
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_FabSpec"), RF_Public | RF_Standalone);
		Instance->SetParentEditorOnly(EngineBase);
		FAssetRegistryModule::AssetCreated(Instance);
		if (!TestTrue(TEXT("saved the instance"), UEditorLoadingAndSavingUtils::SavePackages({ Package }, false)))
			return;
		FString InstanceFile;
		FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), InstanceFile, FPackageName::GetAssetPackageExtension());
		IAssetRegistry::Get()->ScanFilesSynchronous({ InstanceFile }, /*bForceRescan*/ true);
		TestTrue(TEXT("wrote the newer package"), FabSpecWriteNewerPackage(Dir / TEXT("SM_FromNewerEngine.uasset")));

		TArray<FString> Lines;
		const TSharedPtr<FJsonObject> Requirements = UnrealMcpFabTools::CheckImport(Folder, Lines);
		const TArray<TSharedPtr<FJsonValue>>* Unloadable = nullptr;
		if (TestTrue(TEXT("lists unloadable packages"), Requirements->TryGetArrayField(TEXT("unloadablePackages"), Unloadable)) && TestEqual(TEXT("one"), Unloadable->Num(), 1))
			TestTrue(TEXT("the newer one"), (*Unloadable)[0]->AsString().Contains(TEXT("SM_FromNewerEngine")));
		TestTrue(TEXT("the text says it cannot be loaded"), FString::Join(Lines, TEXT("\n")).Contains(TEXT("cannot be loaded by this editor")));
		bool bFab = false;
		const TArray<TSharedPtr<FJsonValue>>* Plugins = nullptr;
		if (Requirements->TryGetArrayField(TEXT("plugins"), Plugins))
		{
			for (const TSharedPtr<FJsonValue>& Plugin : *Plugins)
				bFab |= Plugin->AsObject()->GetStringField(TEXT("name")) == TEXT("Fab") && Plugin->AsObject()->GetBoolField(TEXT("enabled"));
		}
		TestTrue(TEXT("the Fab plugin is needed (its base material), and enabled"), bFab);
		const TArray<TSharedPtr<FJsonValue>>* Unresolved = nullptr;
		TestTrue(TEXT("nothing unresolved"), Requirements->TryGetArrayField(TEXT("unresolvedReferences"), Unresolved) && Unresolved->IsEmpty());

		// An instance with unsaved changes (the user's) is never reparented or saved by fab-add.
		UPackage* DirtyPackage = CreatePackage(*(Folder / TEXT("MI_FabSpecUnsaved")));
		UMaterialInstanceConstant* Unsaved = NewObject<UMaterialInstanceConstant>(DirtyPackage, TEXT("MI_FabSpecUnsaved"), RF_Public | RF_Standalone);
		Unsaved->SetParentEditorOnly(EngineBase);
		FAssetRegistryModule::AssetCreated(Unsaved);
		DirtyPackage->MarkPackageDirty();
		const TArray<FString> Copies = UnrealMcpFabTools::LocalizeBaseMaterials(Folder);
		if (TestEqual(TEXT("one copy used"), Copies.Num(), 1))
			TestTrue(TEXT("named with its instance count"), Copies[0].Contains(CopyPackage) && Copies[0].Contains(TEXT("1 instance")));
		const UMaterial* Copy = Cast<UMaterial>(Instance->Parent);
		if (TestNotNull(TEXT("reparented to a material"), Copy))
		{
			TestEqual(TEXT("the project copy"), Copy->GetOutermost()->GetName(), CopyPackage);
			TestTrue(TEXT("Nanite and instanced usages set"), Copy->bUsedWithNanite && Copy->bUsedWithInstancedStaticMeshes);
			TestFalse(TEXT("the copy is saved"), Copy->GetOutermost()->IsDirty());
		}
		TestEqual(TEXT("the engine material is untouched"), EngineBase->GetOutermost()->IsDirty(), bBaseWasDirty);
		TestTrue(TEXT("the unsaved instance keeps its parent"), Unsaved->Parent == EngineBase);
		TestTrue(TEXT("and stays unsaved"), DirtyPackage->IsDirty());
		TestTrue(TEXT("a second pass changes nothing"), UnrealMcpFabTools::LocalizeBaseMaterials(Folder).IsEmpty());
		DirtyPackage->SetDirtyFlag(false); // no save prompt for the spec's package in a local editor
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
