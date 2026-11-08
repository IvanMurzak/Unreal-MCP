// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

/**
 * The lab case (2026-11-07) of a container inside a struct, on a real Input Mapping Context: its mappings live in
 * DefaultKeyMappings.Mappings, and `get DefaultKeyMappings.Mappings[0] | set Key = "C"` (even as a dry run) or a select of
 * the mappings read freed memory and crashed the editor; `set DefaultKeyMappings.Mappings[0].Key = "C"` reported success
 * without writing. Reached by reflection (classes loaded by path), so the core test
 * module needs no Enhanced Input dependency; skipped when the plugin is disabled.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlEnhancedInputSpec, "UnrealMcp.Tools.Rql.EnhancedInputMappings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	UObject* Context = nullptr;
	UObject* Action = nullptr;
	FProperty* KeyProperty = nullptr;
	bool bHasMapping = false;

	/** The context's first mapping, looked up anew: a write reassigns the array (its memory moves). */
	void* FirstMapping() const
	{
		const FStructProperty* Data = CastField<FStructProperty>(Context->GetClass()->FindPropertyByName(TEXT("DefaultKeyMappings")));
		const FArrayProperty* Mappings = CastField<FArrayProperty>(Data->Struct->FindPropertyByName(TEXT("Mappings")));
		FScriptArrayHelper Helper(Mappings, Mappings->ContainerPtrToValuePtr<void>(Data->ContainerPtrToValuePtr<void>(Context)));
		return Helper.Num() > 0 ? Helper.GetRawPtr(0) : nullptr;
	}

	void MakeContext()
	{
		UClass* ContextClass = LoadClass<UObject>(nullptr, TEXT("/Script/EnhancedInput.InputMappingContext"));
		UClass* ActionClass = LoadClass<UObject>(nullptr, TEXT("/Script/EnhancedInput.InputAction"));
		if (!ContextClass || !ActionClass)
			return;
		Action = NewObject<UObject>(CreatePackage(TEXT("/Game/RqlSpecInput/IA_Spec")), ActionClass, TEXT("IA_Spec"), RF_Public | RF_Standalone | RF_Transactional);
		Context = NewObject<UObject>(CreatePackage(TEXT("/Game/RqlSpecInput/IMC_Spec")), ContextClass, TEXT("IMC_Spec"), RF_Public | RF_Standalone | RF_Transactional);
		const FStructProperty* Data = CastField<FStructProperty>(ContextClass->FindPropertyByName(TEXT("DefaultKeyMappings")));
		const FArrayProperty* Mappings = Data ? CastField<FArrayProperty>(Data->Struct->FindPropertyByName(TEXT("Mappings"))) : nullptr;
		const FStructProperty* Mapping = Mappings ? CastField<FStructProperty>(Mappings->Inner) : nullptr;
		if (!Mapping)
			return;
		FScriptArrayHelper Helper(Mappings, Mappings->ContainerPtrToValuePtr<void>(Data->ContainerPtrToValuePtr<void>(Context)));
		void* First = Helper.GetRawPtr(Helper.AddValue());
		CastField<FObjectPropertyBase>(Mapping->Struct->FindPropertyByName(TEXT("Action")))->SetObjectPropertyValue_InContainer(First, Action);
		KeyProperty = Mapping->Struct->FindPropertyByName(TEXT("Key"));
		KeyProperty->ImportText_Direct(TEXT("SpaceBar"), KeyProperty->ContainerPtrToValuePtr<void>(First), nullptr, PPF_None);
		bHasMapping = true;
	}

	FString FirstKey() const
	{
		FString Text;
		if (void* First = FirstMapping())
			KeyProperty->ExportTextItem_Direct(Text, KeyProperty->ContainerPtrToValuePtr<void>(First), nullptr, nullptr, PPF_None);
		return Text;
	}

END_DEFINE_SPEC(FUnrealMcpRqlEnhancedInputSpec)

void FUnrealMcpRqlEnhancedInputSpec::Define()
{
	BeforeEach([this]() { MakeContext(); });
	AfterEach([this]()
	{
		UnrealMcpRqlSpecHelpers::DropObject(Context);
		UnrealMcpRqlSpecHelpers::DropObject(Action);
		Context = Action = nullptr;
		bHasMapping = false;
	});

	It("reads the mappings and sets a mapping key through DefaultKeyMappings, dry run first", [this]()
	{
		if (!Context || !bHasMapping)
		{
			AddInfo(TEXT("Enhanced Input is not available: skipped."));
			return;
		}
		const TSharedPtr<FJsonObject> Rows = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.asset("/Game/RqlSpecInput/IMC_Spec.IMC_Spec") | get DefaultKeyMappings.Mappings | select Action Key SettingBehavior */
			TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecInput/IMC_Spec.IMC_Spec"}]},"stages":[{"op":"get","expr":{"path":["DefaultKeyMappings","Mappings"]}},{"op":"select","fields":[{"name":"Action","expr":{"path":["Action"]}},{"name":"Key","expr":{"path":["Key"]}},{"name":"SettingBehavior","expr":{"path":["SettingBehavior"]}}]}]})RQL"));
		TestEqual(*FString::Printf(TEXT("one mapping (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Rows)), UnrealMcpRqlSpecHelpers::RowCount(Rows), 1);
		TestTrue(TEXT("its key"), UnrealMcpRqlSpecHelpers::CellText(Rows, 0, 1).Contains(TEXT("SpaceBar")));

		// The lab's form: the streamed mapping is a copy, so the runtime refuses it with the reason (it used to crash first).
		const TSharedPtr<FJsonObject> Streamed = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.asset("/Game/RqlSpecInput/IMC_Spec.IMC_Spec") | get DefaultKeyMappings.Mappings[0] | set Key = "C" */
			TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecInput/IMC_Spec.IMC_Spec"}]},"stages":[{"op":"get","expr":{"path":["DefaultKeyMappings","Mappings",{"index":0}]}},{"op":"set","assign":[{"target":{"path":["Key"]},"value":{"lit":"C"}}]}]})RQL"), true, true);
		TestTrue(*FString::Printf(TEXT("a streamed mapping is refused with the reason (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Streamed)),
			!UnrealMcpRqlSpecHelpers::IsOk(Streamed) && UnrealMcpRqlSpecHelpers::Codes(Streamed).Contains(TEXT("value copies")));

		const FString SetKey = /* unreal.asset("/Game/RqlSpecInput/IMC_Spec.IMC_Spec") | set DefaultKeyMappings.Mappings[0].Key = {KeyName: "C"} */
			TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecInput/IMC_Spec.IMC_Spec"}]},"stages":[{"op":"set","assign":[{"target":{"path":["DefaultKeyMappings","Mappings",{"index":0},"Key"]},"value":{"object":[{"key":"KeyName","value":{"lit":"C"}}]}}]}]})RQL");
		const TSharedPtr<FJsonObject> Dry = UnrealMcpRqlSpecHelpers::Exec(*this, SetKey, true, true);
		TestTrue(*FString::Printf(TEXT("dry run ok (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Dry)), UnrealMcpRqlSpecHelpers::IsOk(Dry));
		TestEqual(TEXT("a dry run writes nothing"), FirstKey(), FString(TEXT("SpaceBar")));
		const TSharedPtr<FJsonObject> Set = UnrealMcpRqlSpecHelpers::Exec(*this, SetKey, true);
		TestTrue(*FString::Printf(TEXT("set ok (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Set)), UnrealMcpRqlSpecHelpers::IsOk(Set));
		TestEqual(TEXT("the key is written"), FirstKey(), FString(TEXT("C")));
	});

	It("refuses MapKey (not an RQL operation) with a clear message instead of calling it", [this]()
	{
		if (!Context)
		{
			AddInfo(TEXT("Enhanced Input is not available: skipped."));
			return;
		}
		const TSharedPtr<FJsonObject> MapKey = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.asset("/Game/RqlSpecInput/IMC_Spec.IMC_Spec") | call MapKey(unreal.asset("/Game/RqlSpecInput/IA_Spec.IA_Spec"), "C") */
			TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecInput/IMC_Spec.IMC_Spec"}]},"stages":[{"op":"call","method":"MapKey","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecInput/IA_Spec.IA_Spec"}]},{"lit":"C"}]}]})RQL"), true, true);
		TestFalse(TEXT("refused"), UnrealMcpRqlSpecHelpers::IsOk(MapKey));
		TestTrue(*FString::Printf(TEXT("says why (%s)"), *UnrealMcpRqlSpecHelpers::Codes(MapKey)), UnrealMcpRqlSpecHelpers::Codes(MapKey).Contains(TEXT("MapKey")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
