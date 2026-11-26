// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Misc/App.h"
#include "UObject/Package.h"

/**
 * Material compile feedback (docs/rql.md "Materials"): CompileErrors, Compile(), UsageFlags, MissingUsages and the
 * diagnostics row of a material that fails to compile. Shaders compile only in an editor that renders: under -nullrhi
 * Compile() says so, and the cases that need real errors check that instead. Works on unsaved
 * /Game/RqlSpecMaterials/M_<n> materials and an actor labelled RqlSpecMaterialUser, and removes them afterwards.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlMaterialsSpec, "UnrealMcp.Tools.Rql.Materials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	int32 Serial = 0;
	UMaterial* Material = nullptr;
	FString MaterialPath; // the object path: /Game/RqlSpecMaterials/M_<n>.M_<n>
	TWeakObjectPtr<AActor> User;

	TSharedPtr<FJsonObject> Run(const FString& Plan, bool bMutate = false)
	{
		return UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(Plan, { { TEXT("M"), MaterialPath } }), bMutate);
	}

	/** unreal.asset("@M@") | call Compile(): the result record as JSON text. */
	FString Compile()
	{
		const TSharedPtr<FJsonObject> Result = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@M@"}]},"stages":[{"op":"call","method":"Compile","args":[]}]})RQL"), true);
		TestTrue(*FString::Printf(TEXT("Compile() runs (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Result)), UnrealMcpRqlSpecHelpers::IsOk(Result));
		return UnrealMcpRqlSpecHelpers::CellText(Result, 0, 2);
	}

	/** unreal.asset("@M@") | select <Member>: the cell as text. */
	FString Read(const TCHAR* Member)
	{
		const FString Plan = FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@M@"}]},"stages":[{"op":"select","fields":[{"name":"%s","expr":{"path":["%s"]}}]}]})RQL"), Member, Member);
		const TSharedPtr<FJsonObject> Result = Run(Plan);
		TestTrue(*FString::Printf(TEXT("%s reads (%s)"), Member, *UnrealMcpRqlSpecHelpers::Codes(Result)), UnrealMcpRqlSpecHelpers::IsOk(Result));
		return UnrealMcpRqlSpecHelpers::CellText(Result);
	}

	/** diagnostics | where Message contains "@M@" (the material's package): how many rows. */
	int32 DiagnosticRows()
	{
		const TSharedPtr<FJsonObject> Result = UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(
			TEXT(R"RQL({"rql":"0.1","source":{"path":["diagnostics"]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Message"]},{"lit":"@P@"}]}}]})RQL"),
			{ { TEXT("P"), Material->GetOutermost()->GetName() } }));
		return UnrealMcpRqlSpecHelpers::RowCount(Result);
	}

	/** BaseColor = ComponentMask(B) of a two-component constant: "not enough components", a translation error. */
	void Break()
	{
		UMaterialExpressionConstant2Vector* Constant = NewObject<UMaterialExpressionConstant2Vector>(Material);
		UMaterialExpressionComponentMask* Mask = NewObject<UMaterialExpressionComponentMask>(Material);
		Material->GetExpressionCollection().AddExpression(Constant);
		Material->GetExpressionCollection().AddExpression(Mask);
		Mask->R = 0;
		Mask->B = 1;
		Mask->Input.Connect(0, Constant);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Mask);
	}

	void Fix()
	{
		Material->GetEditorOnlyData()->BaseColor.Expression = nullptr;
	}

END_DEFINE_SPEC(FUnrealMcpRqlMaterialsSpec)

void FUnrealMcpRqlMaterialsSpec::Define()
{
	BeforeEach([this]()
	{
		const FString Name = FString::Printf(TEXT("M_%d"), ++Serial);
		UPackage* Package = CreatePackage(*(TEXT("/Game/RqlSpecMaterials/") + Name));
		Material = NewObject<UMaterial>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		MaterialPath = Material->GetPathName();
	});

	AfterEach([this]()
	{
		if (User.IsValid())
			User->Destroy();
		if (Material)
		{
			Material->ClearFlags(RF_Public | RF_Standalone);
			Material->MarkAsGarbage();
			Material = nullptr;
		}
	});

	It("returns the compile errors per feature level and platform, and none once it compiles", [this]()
	{
		Break();
		const FString Broken = Compile();
		if (!FApp::CanEverRender())
		{
			TestTrue(*FString::Printf(TEXT("without a renderer it says so (%s)"), *Broken), Broken.Contains(TEXT("-nullrhi")) && Broken.Contains(TEXT("\"Succeeded\":false")));
			TestEqual(TEXT("and has no errors to read"), Read(TEXT("CompileErrors")), FString(TEXT("[]")));
			return;
		}
		TestTrue(*FString::Printf(TEXT("a failed compile is a result (%s)"), *Broken), Broken.Contains(TEXT("\"Succeeded\":false")));
		TestTrue(TEXT("each error names its feature level and platform"), Broken.Contains(TEXT("\"FeatureLevel\":")) && Broken.Contains(TEXT("\"Platform\":")));
		const FString Errors = Read(TEXT("CompileErrors"));
		TestTrue(*FString::Printf(TEXT("CompileErrors reads them (%s)"), *Errors), Errors.Contains(TEXT("\"Message\":")));
		TestEqual(TEXT("the diagnostics source lists the material"), DiagnosticRows(), 1);

		Fix();
		const FString Fixed = Compile();
		TestTrue(*FString::Printf(TEXT("compiles once fixed (%s)"), *Fixed), Fixed.Contains(TEXT("\"Succeeded\":true")) && Fixed.Contains(TEXT("\"Errors\":[]")));
		TestEqual(TEXT("no errors to read"), Read(TEXT("CompileErrors")), FString(TEXT("[]")));
		TestEqual(TEXT("and no diagnostics row"), DiagnosticRows(), 0);
	});

	It("names the usage flags a component using it needs and the material lacks or has not saved", [this]()
	{
		UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!TestNotNull(TEXT("editor world"), World) || !TestNotNull(TEXT("cube mesh"), Cube))
			return;
		const FString Flags = Read(TEXT("UsageFlags"));
		TestFalse(*FString::Printf(TEXT("a new material has no instanced-mesh usage (%s)"), *Flags), Flags.Contains(TEXT("bUsedWithInstancedStaticMeshes")));
		TestEqual(TEXT("nothing uses it yet"), Read(TEXT("MissingUsages")), FString(TEXT("[]")));

		AActor* Actor = World->SpawnActor<AActor>();
		User = Actor;
		Actor->SetActorLabel(TEXT("RqlSpecMaterialUser"));
		UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(Actor, TEXT("Instances"));
		Actor->SetRootComponent(Instances);
		Instances->SetStaticMesh(Cube);
		Instances->SetMaterial(0, Material);
		Instances->RegisterComponent();
		Instances->AddInstance(FTransform::Identity);

		// The editor may set the flag itself when the component renders; unsaved, so it is reported either way.
		const FString Missing = Read(TEXT("MissingUsages"));
		TestTrue(*FString::Printf(TEXT("the instanced mesh needs bUsedWithInstancedStaticMeshes (%s)"), *Missing),
			Missing.Contains(TEXT("\"Usage\":\"bUsedWithInstancedStaticMeshes\"")) && Missing.Contains(TEXT("RqlSpecMaterialUser")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
