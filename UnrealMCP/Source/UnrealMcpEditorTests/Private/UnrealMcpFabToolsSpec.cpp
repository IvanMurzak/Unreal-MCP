// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "UnrealMcpCoreTools.h"
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
}

#endif // WITH_DEV_AUTOMATION_TESTS
