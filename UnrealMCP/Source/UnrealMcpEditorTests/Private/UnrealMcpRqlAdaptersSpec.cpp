// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRqlSpecFixtures.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UnrealMcpRqlTools.h"
#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"

#include "Components/PointLightComponent.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Font.h"
#include "Engine/Level.h"
#include "Engine/Light.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Features/IModularFeatures.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"

/**
 * The existing adapters of the RQL data layer (docs/rql.md): the nine math-struct adapters, the UObject references
 * adapter, TArray / TSet / TMap, scalars and enums, the asset and class-defaults lists, the material parameter maps,
 * the seven class hooks (Object, Actor, ActorComponent, Level, World, MaterialInterface, Editor) and every registration
 * of the extension seam (IUnrealMcpRqlModule). Plans come from Rql.Parser 0.2.0 (the query is the comment before each).
 * A fixture object (UUnrealMcpRqlSpecFixture, in a /Game/RqlSpecAdapters package) holds one property of every kind.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlAdaptersSpec, "UnrealMcp.Tools.Rql.Adapters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	UUnrealMcpRqlSpecFixture* Fixture = nullptr;

	/** The fixture's RQL id (its `Id` member), filled into the "@F@" placeholders of the plans. */
	FString FixtureId;

	TSharedPtr<FJsonObject> Run(const FString& Plan, bool bMutate = false)
	{
		return UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(Plan, { { TEXT("F"), FixtureId } }), bMutate);
	}

	/** Set + read back one member: the mutation succeeds and the member then reads as @p Expected. */
	void SetsTo(const TCHAR* What, const FString& SetPlan, const FString& ReadPlan, const FString& Expected)
	{
		const TSharedPtr<FJsonObject> Set = Run(SetPlan, true);
		TestTrue(*FString::Printf(TEXT("%s: set ok (%s)"), What, *UnrealMcpRqlSpecHelpers::Codes(Set)), UnrealMcpRqlSpecHelpers::IsOk(Set));
		const TSharedPtr<FJsonObject> Read = Run(ReadPlan);
		TestEqual(*FString::Printf(TEXT("%s: reads back"), What), UnrealMcpRqlSpecHelpers::CellText(Read), Expected);
	}

	/** A mutation that must succeed. */
	void Must(const FString& Plan)
	{
		const TSharedPtr<FJsonObject> Result = Run(Plan, true);
		TestTrue(*FString::Printf(TEXT("mutation ok (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Result)), UnrealMcpRqlSpecHelpers::IsOk(Result));
	}

	/** A mutation that must fail with a diagnostic containing @p Expected. */
	void Refuses(const TCHAR* What, const FString& Plan, const FString& Expected)
	{
		const TSharedPtr<FJsonObject> Result = Run(Plan, true);
		TestFalse(*FString::Printf(TEXT("%s: refused"), What), UnrealMcpRqlSpecHelpers::IsOk(Result));
		TestTrue(*FString::Printf(TEXT("%s: says '%s' (%s)"), What, *Expected, *UnrealMcpRqlSpecHelpers::Codes(Result)),
			UnrealMcpRqlSpecHelpers::Codes(Result).Contains(Expected));
	}

	static UWorld* World()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	static TArray<AActor*> Lamps()
	{
		TArray<AActor*> Out;
		if (UWorld* W = World())
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				if (IsValid(*It) && It->GetActorLabel().StartsWith(TEXT("RqlAdaptersLamp")))
					Out.Add(*It);
			}
		}
		return Out;
	}

	static AActor* Lamp(const FString& Label)
	{
		for (AActor* Actor : Lamps())
		{
			if (Actor->GetActorLabel() == Label)
				return Actor;
		}
		return nullptr;
	}

	static void CleanupWorld()
	{
		if (GEditor)
			GEditor->SelectNone(false, true);
		for (AActor* Actor : Lamps())
			Actor->GetWorld()->EditorDestroyActor(Actor, true);
	}

	void MakeFixture()
	{
		UPackage* Package = CreatePackage(TEXT("/Game/RqlSpecAdapters/Fixture"));
		Fixture = NewObject<UUnrealMcpRqlSpecFixture>(Package, TEXT("Fixture"), RF_Public | RF_Standalone | RF_Transactional);
		// The id of the fixture: rql-query reads `Id` of the object found by its path.
		const TSharedPtr<FJsonObject> Read = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.asset("/Game/RqlSpecAdapters/Fixture.Fixture") | select Id */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecAdapters/Fixture.Fixture"}]},"stages":[{"op":"select","fields":[{"name":"Id","expr":{"path":["Id"]}}]}]})RQL"));
		FixtureId = UnrealMcpRqlSpecHelpers::CellText(Read);
		TestFalse(TEXT("the fixture has an id"), FixtureId.IsEmpty());
	}

	void DropFixture()
	{
		UnrealMcpRqlSpecHelpers::DropObject(Fixture);
		Fixture = nullptr;
		FixtureId.Reset();
	}

END_DEFINE_SPEC(FUnrealMcpRqlAdaptersSpec)

/** The extension seam's registrations, exercised end to end by a module the spec registers. */
class FUnrealMcpRqlAdaptersSpecModule final : public IUnrealMcpRqlModule
{
public:
	TFunction<UObject*()> Fixture;

	virtual FString GetRqlModuleName() const override { return TEXT("RqlAdaptersSpec"); }
	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		TFunction<UObject*()> Get = Fixture;
		Context.AddClassDefaultsSource(TEXT("rqlSpecLights"), TEXT("light class defaults"), ALight::StaticClass());
		Context.AddObjectSource(TEXT("rqlSpecFixture"), TEXT("the fixture"), UUnrealMcpRqlSpecFixture::StaticClass(), Get);
		Context.AddObjectListSource(TEXT("rqlSpecFixtures"), TEXT("the fixture, as a list"), UUnrealMcpRqlSpecFixture::StaticClass(),
			[Get]() { TArray<UObject*> Out; if (UObject* Object = Get()) Out.Add(Object); return Out; });
		Context.AddAssetSource(TEXT("rqlSpecMaterials"), TEXT("project materials"), UMaterialInterface::StaticClass());
		Context.AddJsonMember(UUnrealMcpRqlSpecFixture::StaticClass(), TEXT("SpecJson"), TEXT("json"), [](UObject*) -> TSharedPtr<FJsonValue>
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("a"), 1);
			Object->SetArrayField(TEXT("b"), { MakeShared<FJsonValueString>(TEXT("x")), MakeShared<FJsonValueBoolean>(true) });
			return MakeShared<FJsonValueObject>(Object);
		});
		Context.AddObjectMember(UUnrealMcpRqlSpecFixture::StaticClass(), TEXT("SpecSelf"), TEXT("itself"), UUnrealMcpRqlSpecFixture::StaticClass(),
			[](UObject* Object) { return Object; });
		Context.AddObjectListMember(UUnrealMcpRqlSpecFixture::StaticClass(), TEXT("SpecList"), TEXT("itself, twice"), UObject::StaticClass(),
			[](UObject* Object) { return TArray<UObject*>{ Object, Object }; });
		Context.ExposeProperties(UUnrealMcpRqlSpecFixture::StaticClass(), { TEXT("HiddenValue") }, /*bWritable*/ true);
		Context.AddApiSource(TEXT("rqlSpecApi"), TEXT("spec operations"), UUnrealMcpRqlSpecApi::StaticClass(), TEXT("RqlSpecApi"));
		Context.AddGuide(TEXT("RqlAdaptersSpec guide"), TEXT("the rqlSpec guide body"));
	}
};

void FUnrealMcpRqlAdaptersSpec::Define()
{
	BeforeEach([this]() { CleanupWorld(); MakeFixture(); });
	AfterEach([this]() { CleanupWorld(); DropFixture(); });

	Describe("math structs", [this]()
	{
		It("reads and writes Vector as text, lists and object literals, and rejects malformed text", [this]()
		{
			SetsTo(TEXT("text"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector = "(1, 2.5, -3)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector"]},"value":{"lit":"(1, 2.5, -3)"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Vector */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Vector","expr":{"path":["Vector"]}}]}]})RQL"), TEXT("(1, 2.5, -3)"));
			TestTrue(TEXT("stored"), Fixture->Vector.Equals(FVector(1, 2.5, -3)));
			SetsTo(TEXT("list"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector = [4, 5, 6] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector"]},"value":{"list":[{"lit":4},{"lit":5},{"lit":6}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Vector */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Vector","expr":{"path":["Vector"]}}]}]})RQL"), TEXT("(4, 5, 6)"));
			// An object literal constructs a new value (RQL spec §4.5): the members it does not name are zero.
			SetsTo(TEXT("object literal"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector = {X: 7} */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector"]},"value":{"object":[{"key":"X","value":{"lit":7}}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Vector */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Vector","expr":{"path":["Vector"]}}]}]})RQL"), TEXT("(7, 0, 0)"));
			SetsTo(TEXT("a component from an expression"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector.Z = Vector.X * 2 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector","Z"]},"value":{"op":"*","args":[{"path":["Vector","X"]},{"lit":2}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Vector */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Vector","expr":{"path":["Vector"]}}]}]})RQL"), TEXT("(7, 0, 14)"));
			SetsTo(TEXT("Unreal's own text form"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector = "X=1 Y=2 Z=3" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector"]},"value":{"lit":"X=1 Y=2 Z=3"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Vector */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Vector","expr":{"path":["Vector"]}}]}]})RQL"), TEXT("(1, 2, 3)"));
			Refuses(TEXT("two numbers"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector = "(1, 2)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector"]},"value":{"lit":"(1, 2)"}}]}]})RQL"), TEXT("expects 3 numbers"));
			Refuses(TEXT("a text list element"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector = [1, "a", 3] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector"]},"value":{"list":[{"lit":1},{"lit":"a"},{"lit":3}]}}]}]})RQL"), TEXT("must be numbers"));
			Refuses(TEXT("an unknown literal member"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector = {W: 1} */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector"]},"value":{"object":[{"key":"W","value":{"lit":1}}]}}]}]})RQL"), TEXT("no member 'W'"));
			TestTrue(TEXT("a refused write keeps the value"), Fixture->Vector.Equals(FVector(1, 2, 3)));
			TestTrue(TEXT("writes notify the object (PostEditChangeProperty)"), Fixture->PostEditCount >= 5);
		});

		It("converts the other vector, rotation and integer structs", [this]()
		{
			SetsTo(TEXT("Vector2D"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector2D = "(1.5, -2)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector2D"]},"value":{"lit":"(1.5, -2)"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Vector2D */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Vector2D","expr":{"path":["Vector2D"]}}]}]})RQL"), TEXT("(1.5, -2)"));
			SetsTo(TEXT("Vector4"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector4 = [1, 2, 3, 4] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector4"]},"value":{"list":[{"lit":1},{"lit":2},{"lit":3},{"lit":4}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Vector4 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Vector4","expr":{"path":["Vector4"]}}]}]})RQL"), TEXT("(1, 2, 3, 4)"));
			SetsTo(TEXT("Rotator"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Rotator = "(10, 90, -45)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Rotator"]},"value":{"lit":"(10, 90, -45)"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Rotator */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Rotator","expr":{"path":["Rotator"]}}]}]})RQL"), TEXT("(10, 90, -45)"));
			TestTrue(TEXT("pitch yaw roll order"), FMath::IsNearlyEqual(Fixture->Rotator.Pitch, 10.0) && FMath::IsNearlyEqual(Fixture->Rotator.Yaw, 90.0)
				&& FMath::IsNearlyEqual(Fixture->Rotator.Roll, -45.0));
			SetsTo(TEXT("Rotator in Unreal's text form"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Rotator = "P=1 Y=2 R=3" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Rotator"]},"value":{"lit":"P=1 Y=2 R=3"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Rotator */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Rotator","expr":{"path":["Rotator"]}}]}]})RQL"), TEXT("(1, 2, 3)"));
			SetsTo(TEXT("Quat"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Quat = "(0, 0, 0.5, 0.5)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Quat"]},"value":{"lit":"(0, 0, 0.5, 0.5)"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Quat */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Quat","expr":{"path":["Quat"]}}]}]})RQL"), TEXT("(0, 0, 0.5, 0.5)"));
			SetsTo(TEXT("IntPoint rounds"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set IntPoint = "(1.6, -2.4)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["IntPoint"]},"value":{"lit":"(1.6, -2.4)"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select IntPoint */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"IntPoint","expr":{"path":["IntPoint"]}}]}]})RQL"), TEXT("(2, -2)"));
			SetsTo(TEXT("IntVector"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set IntVector = [3, 2, 1] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["IntVector"]},"value":{"list":[{"lit":3},{"lit":2},{"lit":1}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select IntVector */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"IntVector","expr":{"path":["IntVector"]}}]}]})RQL"), TEXT("(3, 2, 1)"));
			Refuses(TEXT("Vector4 with three numbers"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Vector4 = [1, 2, 3] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vector4"]},"value":{"list":[{"lit":1},{"lit":2},{"lit":3}]}}]}]})RQL"), TEXT("expects 4 numbers"));
		});

		It("converts colors: #rrggbb[aa], optional alpha, 0..255 clamping", [this]()
		{
			SetsTo(TEXT("LinearColor from hex (sRGB)"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set LinearColor = "#ff0000" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["LinearColor"]},"value":{"lit":"#ff0000"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select LinearColor */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"LinearColor","expr":{"path":["LinearColor"]}}]}]})RQL"), TEXT("(1, 0, 0, 1)"));
			SetsTo(TEXT("LinearColor alpha defaults to 1"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set LinearColor = "(0.5, 0.25, 0)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["LinearColor"]},"value":{"lit":"(0.5, 0.25, 0)"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select LinearColor */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"LinearColor","expr":{"path":["LinearColor"]}}]}]})RQL"), TEXT("(0.5, 0.25, 0, 1)"));
			SetsTo(TEXT("Color from hex with alpha"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Color = "#00ff0080" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Color"]},"value":{"lit":"#00ff0080"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Color */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Color","expr":{"path":["Color"]}}]}]})RQL"), TEXT("(0, 255, 0, 128)"));
			SetsTo(TEXT("Color alpha defaults to 255, values clamp"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Color = [10, 300, -5] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Color"]},"value":{"list":[{"lit":10},{"lit":300},{"lit":-5}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Color */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Color","expr":{"path":["Color"]}}]}]})RQL"), TEXT("(10, 255, 0, 255)"));
			Refuses(TEXT("non-hex"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set LinearColor = "#gg0000" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["LinearColor"]},"value":{"lit":"#gg0000"}}]}]})RQL"), TEXT("expects 4 numbers"));
		});

		It("compares math structs with their text forms", [this]()
		{
			Fixture->Vector = FVector(1, 2, 3);
			Fixture->LinearColor = FLinearColor(1, 0, 0, 1);
			const TSharedPtr<FJsonObject> ByText = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | where Vector == "(1, 2, 3)" | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Vector"]},{"lit":"(1, 2, 3)"}]}},{"op":"count"}]})RQL"));
			TestEqual(*FString::Printf(TEXT("Vector == text (%s)"), *UnrealMcpRqlSpecHelpers::Codes(ByText)), UnrealMcpRqlSpecHelpers::Number(ByText), 1.0);
			const TSharedPtr<FJsonObject> ByList = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | where Vector == [1, 2, 3] | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Vector"]},{"list":[{"lit":1},{"lit":2},{"lit":3}]}]}},{"op":"count"}]})RQL"));
			TestEqual(*FString::Printf(TEXT("Vector == list (%s)"), *UnrealMcpRqlSpecHelpers::Codes(ByList)), UnrealMcpRqlSpecHelpers::Number(ByList), 1.0);
			const TSharedPtr<FJsonObject> Different = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | where Vector == "(1, 2, 4)" | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Vector"]},{"lit":"(1, 2, 4)"}]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("a different vector"), UnrealMcpRqlSpecHelpers::Number(Different), 0.0);
			// ref("UnrealMcpRqlSpecFixture#@F@") | where Vector == {X: 1, Y: 2, Z: @Z@} | count
			const FString ByObject = TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Vector"]},{"object":[{"key":"X","value":{"lit":1}},{"key":"Y","value":{"lit":2}},{"key":"Z","value":{"lit":@Z@}}]}]}},{"op":"count"}]})RQL");
			const TSharedPtr<FJsonObject> Same = Run(ByObject.Replace(TEXT("@Z@"), TEXT("3")));
			TestEqual(*FString::Printf(TEXT("Vector == object literal (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Same)), UnrealMcpRqlSpecHelpers::Number(Same), 1.0);
			TestEqual(TEXT("a different object literal"), UnrealMcpRqlSpecHelpers::Number(Run(ByObject.Replace(TEXT("@Z@"), TEXT("4")))), 0.0);
			const TSharedPtr<FJsonObject> Color = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | where LinearColor == "#ff0000" | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["LinearColor"]},{"lit":"#ff0000"}]}},{"op":"count"}]})RQL"));
			TestEqual(*FString::Printf(TEXT("color == hex (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Color)), UnrealMcpRqlSpecHelpers::Number(Color), 1.0);
			const TSharedPtr<FJsonObject> Component = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | where Vector.Y > 1.5 | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"where","pred":{"op":">","args":[{"path":["Vector","Y"]},{"lit":1.5}]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("a component in a filter"), UnrealMcpRqlSpecHelpers::Number(Component), 1.0);
		});
	});

	Describe("references", [this]()
	{
		It("accepts asset paths, class names and null; reads soft references as paths", [this]()
		{
			SetsTo(TEXT("asset path"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Reference = "/Engine/EngineMaterials/DefaultMaterial" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Reference"]},"value":{"lit":"/Engine/EngineMaterials/DefaultMaterial"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Reference.Name */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Reference.Name","expr":{"path":["Reference","Name"]}}]}]})RQL"), TEXT("DefaultMaterial"));
			SetsTo(TEXT("class name"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set ActorClass = "PointLight" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["ActorClass"]},"value":{"lit":"PointLight"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select ActorClass.Name */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"ActorClass.Name","expr":{"path":["ActorClass","Name"]}}]}]})RQL"), TEXT("PointLight"));
			TestTrue(TEXT("the class is stored"), Fixture->ActorClass == APointLight::StaticClass());
			SetsTo(TEXT("soft reference"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Soft = "/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Soft"]},"value":{"lit":"/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Soft */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Soft","expr":{"path":["Soft"]}}]}]})RQL"), TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
			SetsTo(TEXT("null clears"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Reference = null, Soft = null */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Reference"]},"value":{"lit":null}},{"target":{"path":["Soft"]},"value":{"lit":null}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Reference */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Reference","expr":{"path":["Reference"]}}]}]})RQL"), TEXT(""));
			TestTrue(TEXT("both cleared"), Fixture->Reference == nullptr && Fixture->Soft.IsNull());
		});

		It("reads and writes soft object / class paths as text", [this]()
		{
			SetsTo(TEXT("a path"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set SoftPath = "/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["SoftPath"]},"value":{"lit":"/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select SoftPath */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"SoftPath","expr":{"path":["SoftPath"]}}]}]})RQL"), TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
			TestEqual(TEXT("stored"), Fixture->SoftPath.ToString(), FString(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
			const TSharedPtr<FJsonObject> Package = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | where SoftPath == "/Engine/EngineMaterials/DefaultMaterial" | select SoftPath.Package */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["SoftPath"]},{"lit":"/Engine/EngineMaterials/DefaultMaterial"}]}},{"op":"select","fields":[{"name":"SoftPath.Package","expr":{"path":["SoftPath","Package"]}}]}]})RQL"));
			TestEqual(TEXT("compares with the package path"), UnrealMcpRqlSpecHelpers::CellText(Package), FString(TEXT("/Engine/EngineMaterials/DefaultMaterial")));
			Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | set Reference = "/Engine/EngineMaterials/WorldGridMaterial" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Reference"]},"value":{"lit":"/Engine/EngineMaterials/WorldGridMaterial"}}]}]})RQL"), true);
			SetsTo(TEXT("an object"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set SoftPath = Reference */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["SoftPath"]},"value":{"path":["Reference"]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select SoftPath */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"SoftPath","expr":{"path":["SoftPath"]}}]}]})RQL"), TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
			SetsTo(TEXT("a class path"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set SoftClass = "/Script/Engine.PointLight" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["SoftClass"]},"value":{"lit":"/Script/Engine.PointLight"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select SoftClass */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"SoftClass","expr":{"path":["SoftClass"]}}]}]})RQL"), TEXT("/Script/Engine.PointLight"));
			Must(/* ref("UnrealMcpRqlSpecFixture#@F@") | set SoftPath = "", SoftClass = "" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["SoftPath"]},"value":{"lit":""}},{"target":{"path":["SoftClass"]},"value":{"lit":""}}]}]})RQL"));
			TestTrue(TEXT("cleared"), Fixture->SoftPath.IsNull() && Fixture->SoftClass.IsNull());
			Refuses(TEXT("raw members are not writable"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set SoftPath.Path = "x" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["SoftPath","Path"]},"value":{"lit":"x"}}]}]})RQL"), TEXT("readonly-member"));
		});

		It("refuses missing objects and classes of the wrong base", [this]()
		{
			Refuses(TEXT("missing asset"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Reference = "/Game/RqlSpecAdapters/NoSuchAsset" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Reference"]},"value":{"lit":"/Game/RqlSpecAdapters/NoSuchAsset"}}]}]})RQL"), TEXT("no Object"));
			Refuses(TEXT("not an actor class"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set ActorClass = "Material" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["ActorClass"]},"value":{"lit":"Material"}}]}]})RQL"), TEXT("not a subclass of Actor"));
			Refuses(TEXT("a number for an object"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Reference = 5 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Reference"]},"value":{"lit":5}}]}]})RQL"), TEXT("Object"));
			TestTrue(TEXT("nothing stored"), Fixture->Reference == nullptr && Fixture->ActorClass == nullptr);
		});
	});

	Describe("containers", [this]()
	{
		It("writes arrays whole and by element, and refuses bad elements and indexes", [this]()
		{
			SetsTo(TEXT("whole array"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Numbers = [1, 2, 3] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Numbers"]},"value":{"list":[{"lit":1},{"lit":2},{"lit":3}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Numbers */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Numbers","expr":{"path":["Numbers"]}}]}]})RQL"), TEXT("[1,2,3]"));
			const int32 Before = Fixture->PostEditCount;
			SetsTo(TEXT("one element"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Numbers[1] = 5 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Numbers",{"index":1}]},"value":{"lit":5}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Numbers */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Numbers","expr":{"path":["Numbers"]}}]}]})RQL"), TEXT("[1,5,3]"));
			TestTrue(TEXT("an element write notifies the owner"), Fixture->PostEditCount > Before);
			SetsTo(TEXT("append"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Numbers = append(Numbers, 9) */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Numbers"]},"value":{"call":"append","args":[{"path":["Numbers"]},{"lit":9}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Numbers */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Numbers","expr":{"path":["Numbers"]}}]}]})RQL"), TEXT("[1,5,3,9]"));
			const TSharedPtr<FJsonObject> Sum = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | get Numbers | sum $ */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"get","expr":{"path":["Numbers"]}},{"op":"sum","expr":{"path":[]}}]})RQL"));
			TestEqual(TEXT("elements stream"), UnrealMcpRqlSpecHelpers::Number(Sum), 18.0);
			Refuses(TEXT("index out of range"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Numbers[9] = 1 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Numbers",{"index":9}]},"value":{"lit":1}}]}]})RQL"), TEXT("out of range"));
			Refuses(TEXT("text element"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Numbers = ["x"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Numbers"]},"value":{"list":[{"lit":"x"}]}}]}]})RQL"), TEXT("number"));
			TestEqual(TEXT("refused writes keep the array"), Fixture->Numbers, TArray<int32>({ 1, 5, 3, 9 }));
			SetsTo(TEXT("struct elements in RQL forms"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Points = ["(1, 2, 3)", [4, 5, 6]] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Points"]},"value":{"list":[{"lit":"(1, 2, 3)"},{"list":[{"lit":4},{"lit":5},{"lit":6}]}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Points[1] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Points[1]","expr":{"path":["Points",{"index":1}]}}]}]})RQL"), TEXT("(4, 5, 6)"));
			SetsTo(TEXT("a struct element member"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Points[0].Z = 30 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Points",{"index":0},"Z"]},"value":{"lit":30}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Points[0] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Points[0]","expr":{"path":["Points",{"index":0}]}}]}]})RQL"), TEXT("(1, 2, 30)"));
		});

		It("writes sets (equal names collapse) and maps (keys added, values by key)", [this]()
		{
			Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | set Names = ["a", "A", "b"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Names"]},"value":{"list":[{"lit":"a"},{"lit":"A"},{"lit":"b"}]}}]}]})RQL"), true);
			TestEqual(TEXT("FName case collapses"), Fixture->Names.Num(), 2);
			SetsTo(TEXT("whole map"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Scores = {a: 1, b: 2} */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Scores"]},"value":{"object":[{"key":"a","value":{"lit":1}},{"key":"b","value":{"lit":2}}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Scores["b"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Scores[\"b\"]","expr":{"path":["Scores",{"key":"b"}]}}]}]})RQL"), TEXT("2"));
			SetsTo(TEXT("a new key"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Scores["c"] = 3 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Scores",{"key":"c"}]},"value":{"lit":3}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Scores["c"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Scores[\"c\"]","expr":{"path":["Scores",{"key":"c"}]}}]}]})RQL"), TEXT("3"));
			SetsTo(TEXT("an existing key"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Scores["a"] = Scores["a"] + 10 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Scores",{"key":"a"}]},"value":{"op":"+","args":[{"path":["Scores",{"key":"a"}]},{"lit":10}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Scores["a"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Scores[\"a\"]","expr":{"path":["Scores",{"key":"a"}]}}]}]})RQL"), TEXT("11"));
			TestEqual(TEXT("three entries"), Fixture->Scores.Num(), 3);
			SetsTo(TEXT("struct values"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Spots["home"] = "(1, 2, 3)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Spots",{"key":"home"}]},"value":{"lit":"(1, 2, 3)"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Spots["home"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Spots[\"home\"]","expr":{"path":["Spots",{"key":"home"}]}}]}]})RQL"), TEXT("(1, 2, 3)"));
			const TSharedPtr<FJsonObject> Keys = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | get Scores | where Value > 2 | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"get","expr":{"path":["Scores"]}},{"op":"where","pred":{"op":">","args":[{"path":["Value"]},{"lit":2}]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("map entries stream (Key / Value)"), UnrealMcpRqlSpecHelpers::Number(Keys), 2.0);
			Refuses(TEXT("a list for a map"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Scores = [1] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Scores"]},"value":{"list":[{"lit":1}]}}]}]})RQL"), TEXT("Dictionary"));
		});
	});

	Describe("scalars and enums", [this]()
	{
		It("checks integer ranges, enum names and read-only members", [this]()
		{
			SetsTo(TEXT("uint8 max"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Small = 255 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Small"]},"value":{"lit":255}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Small */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Small","expr":{"path":["Small"]}}]}]})RQL"), TEXT("255"));
			Refuses(TEXT("uint8 overflow"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Small = 256 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Small"]},"value":{"lit":256}}]}]})RQL"), TEXT("out of range"));
			Refuses(TEXT("unsigned below zero"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Small = -1 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Small"]},"value":{"lit":-1}}]}]})RQL"), TEXT("out of range"));
			SetsTo(TEXT("int64"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Big = 1099511627776 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Big"]},"value":{"lit":1099511627776}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Big */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Big","expr":{"path":["Big"]}}]}]})RQL"), TEXT("1099511627776"));
			SetsTo(TEXT("enum by name"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Mode = "Run" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Mode"]},"value":{"lit":"Run"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Mode */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Mode","expr":{"path":["Mode"]}}]}]})RQL"), TEXT("Run"));
			SetsTo(TEXT("enum by number"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Mode = 1 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Mode"]},"value":{"lit":1}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Mode */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Mode","expr":{"path":["Mode"]}}]}]})RQL"), TEXT("Walk"));
			// RQL a258ba9 enum aliases: the editor's display name (UMETA DisplayName) resolves; "enum by name" above shows
			// that Walk's display name "Run", which spells another entry, does not shadow the Run entry.
			SetsTo(TEXT("enum by display name"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Mode = "Sprint" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Mode"]},"value":{"lit":"Sprint"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Mode */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Mode","expr":{"path":["Mode"]}}]}]})RQL"), TEXT("Run"));
			Refuses(TEXT("unknown enum name"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Mode = "Fly" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Mode"]},"value":{"lit":"Fly"}}]}]})RQL"), TEXT("Fly"));
			SetsTo(TEXT("bool"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set bFlag = true */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["bFlag"]},"value":{"lit":true}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select bFlag */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"bFlag","expr":{"path":["bFlag"]}}]}]})RQL"), TEXT("true"));
			Refuses(TEXT("text for a bool (RQL is strict)"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set bFlag = "false" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["bFlag"]},"value":{"lit":"false"}}]}]})RQL"), TEXT("expected true or false"));
			SetsTo(TEXT("FText"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Caption = "Hello" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Caption"]},"value":{"lit":"Hello"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Caption */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Caption","expr":{"path":["Caption"]}}]}]})RQL"), TEXT("Hello"));
			SetsTo(TEXT("FName"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Key = "Name_1" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Key"]},"value":{"lit":"Name_1"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Key */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Key","expr":{"path":["Key"]}}]}]})RQL"), TEXT("Name_1"));
			Refuses(TEXT("VisibleAnywhere is read-only"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set ReadOnlyValue = 1 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["ReadOnlyValue"]},"value":{"lit":1}}]}]})RQL"), TEXT("readonly-member"));
			TestEqual(TEXT("kept"), Fixture->ReadOnlyValue, 7);
		});

		It("writes nested struct members and struct literals", [this]()
		{
			SetsTo(TEXT("nested member"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Row.Damage = 5, Row.Offset.Y = 2 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Row","Damage"]},"value":{"lit":5}},{"target":{"path":["Row","Offset","Y"]},"value":{"lit":2}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Row.Offset */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Row.Offset","expr":{"path":["Row","Offset"]}}]}]})RQL"), TEXT("(0, 2, 0)"));
			TestEqual(TEXT("damage"), Fixture->Row.Damage, 5);
			// A literal of a struct no query has read yet: its members are known before any value of it exists.
			SetsTo(TEXT("a struct literal"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Row = {Label: "x", Mode: "Run"} */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Row"]},"value":{"object":[{"key":"Label","value":{"lit":"x"}},{"key":"Mode","value":{"lit":"Run"}}]}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Row.Label */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Row.Label","expr":{"path":["Row","Label"]}}]}]})RQL"), TEXT("x"));
			TestTrue(TEXT("the members it does not name get their defaults"), Fixture->Row.Damage == 0 && Fixture->Row.Mode == EUnrealMcpRqlSpecMode::Run);
			const TSharedPtr<FJsonObject> Filter = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | where Row.Label == "x" and Row.Speed == 1 | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"where","pred":{"op":"and","args":[{"op":"==","args":[{"path":["Row","Label"]},{"lit":"x"}]},{"op":"==","args":[{"path":["Row","Speed"]},{"lit":1}]}]}},{"op":"count"}]})RQL"));
			TestEqual(*FString::Printf(TEXT("struct members in a filter (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Filter)), UnrealMcpRqlSpecHelpers::Number(Filter), 1.0);
		});

		// Plans from Rql.Parser 0.3.0-dev.602ce7c (RQL main 602ce7c: MutationPolicy::coerce_text, map-entry writes).
		It("imports UE's text form for structs without an adapter, and refuses text that does not import", [this]()
		{
			SetsTo(TEXT("ExportText form"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Row = "(Damage=5,Label=\"text form\",Offset=(X=1,Y=2,Z=3))" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Row"]},"value":{"lit":"(Damage=5,Label=\"text form\",Offset=(X=1,Y=2,Z=3))"}}]}]})RQL"),
				/* ref("UnrealMcpRqlSpecFixture#@F@") | select Row.Label */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Row.Label","expr":{"path":["Row","Label"]}}]}]})RQL"), TEXT("text form"));
			TestTrue(TEXT("every member imported"), Fixture->Row.Damage == 5 && Fixture->Row.Offset.Equals(FVector(1, 2, 3)));
			Refuses(TEXT("an unknown member"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Row = "(Nope=1)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Row"]},"value":{"lit":"(Nope=1)"}}]}]})RQL"), TEXT("Unreal's text form"));
			Refuses(TEXT("trailing text"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Row = "(Damage=5) trailing" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Row"]},"value":{"lit":"(Damage=5) trailing"}}]}]})RQL"), TEXT("trailing"));
			TestEqual(TEXT("refusals left the row"), Fixture->Row.Damage, 5);
			// The math structs too: members by name, not by position (an FColor copies as B, G, R, A).
			Must(/* ref("UnrealMcpRqlSpecFixture#@F@") | set Color = "(B=0,G=0,R=255,A=255)", Vector = "(X=1,Y=2,Z=3)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Color"]},"value":{"lit":"(B=0,G=0,R=255,A=255)"}},{"target":{"path":["Vector"]},"value":{"lit":"(X=1,Y=2,Z=3)"}}]}]})RQL"));
			TestTrue(TEXT("FColor by member name"), Fixture->Color == FColor(255, 0, 0, 255));
			TestTrue(TEXT("FVector by member name"), Fixture->Vector.Equals(FVector(1, 2, 3)));
		});

		It("writes map entries streamed by get through the owning map", [this]()
		{
			Must(/* ref("UnrealMcpRqlSpecFixture#@F@") | set Scores = {a: 1, b: 2, c: 3} */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Scores"]},"value":{"object":[{"key":"a","value":{"lit":1}},{"key":"b","value":{"lit":2}},{"key":"c","value":{"lit":3}}]}}]}]})RQL"));
			const int32 EditsBefore = Fixture->PostEditCount;
			const TSharedPtr<FJsonObject> Scaled = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | get Scores | where Value > 1 | set Value = Value * 10 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"get","expr":{"path":["Scores"]}},{"op":"where","pred":{"op":">","args":[{"path":["Value"]},{"lit":1}]}},{"op":"set","assign":[{"target":{"path":["Value"]},"value":{"op":"*","args":[{"path":["Value"]},{"lit":10}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("entry write ok (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Scaled)), UnrealMcpRqlSpecHelpers::IsOk(Scaled));
			TestEqual(TEXT("one receipt row per entry"), UnrealMcpRqlSpecHelpers::RowCount(Scaled), 2);
			TestTrue(TEXT("the receipt names the owner"), UnrealMcpRqlSpecHelpers::CellText(Scaled, 0, 0).StartsWith(TEXT("UnrealMcpRqlSpecFixture#")));
			const int32* A = Fixture->Scores.Find(TEXT("a"));
			const int32* B = Fixture->Scores.Find(TEXT("b"));
			const int32* C = Fixture->Scores.Find(TEXT("c"));
			TestTrue(TEXT("matching values scaled, the other kept"), A && B && C && *A == 1 && *B == 20 && *C == 30);
			TestTrue(TEXT("written through the owner's editor notification"), Fixture->PostEditCount > EditsBefore);

			Must(/* ref("UnrealMcpRqlSpecFixture#@F@") | set Spots = {home: "(1, 2, 3)", away: "(4, 5, 6)"} */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Spots"]},"value":{"object":[{"key":"home","value":{"lit":"(1, 2, 3)"}},{"key":"away","value":{"lit":"(4, 5, 6)"}}]}}]}]})RQL"));
			Must(/* ref("UnrealMcpRqlSpecFixture#@F@") | get Spots | where Key == "away" | set Value.Z = 60 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"get","expr":{"path":["Spots"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Key"]},{"lit":"away"}]}},{"op":"set","assign":[{"target":{"path":["Value","Z"]},"value":{"lit":60}}]}]})RQL"));
			const FVector* Away = Fixture->Spots.Find(TEXT("away"));
			const FVector* Home = Fixture->Spots.Find(TEXT("home"));
			TestTrue(TEXT("a struct member of an entry written back"), Away && Away->Equals(FVector(4, 5, 60)) && Home && Home->Equals(FVector(1, 2, 3)));
		});

		It("rolls a failing set back as a whole", [this]()
		{
			Fixture->Small = 1;
			Fixture->Big = 1;
			Refuses(TEXT("second write fails"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Big = 2, Small = 999 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Big"]},"value":{"lit":2}},{"target":{"path":["Small"]},"value":{"lit":999}}]}]})RQL"), TEXT("out of range"));
			TestTrue(TEXT("first write rolled back"), Fixture->Big == 1 && Fixture->Small == 1);
		});

		It("undoes a set with one editor transaction", [this]()
		{
			if (!GEditor)
				return;
			Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | set Big = 41, Vector = "(9, 9, 9)" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Big"]},"value":{"lit":41}},{"target":{"path":["Vector"]},"value":{"lit":"(9, 9, 9)"}}]}]})RQL"), true);
			TestEqual(TEXT("written"), Fixture->Big, static_cast<int64>(41));
			GEditor->UndoTransaction();
			TestEqual(TEXT("undone"), Fixture->Big, static_cast<int64>(0));
			TestTrue(TEXT("both members in one step"), Fixture->Vector.IsZero());
			GEditor->RedoTransaction();
			TestEqual(TEXT("redone"), Fixture->Big, static_cast<int64>(41));
			TestTrue(TEXT("the package is dirty"), Fixture->GetOutermost()->IsDirty());
		});
	});

	Describe("asset lists", [this]()
	{
		It("lists assets of a class lazily and reports unknown classes", [this]()
		{
			const TSharedPtr<FJsonObject> Count = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.assets("Material", "/Engine/EngineMaterials") | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"assets","on":{"path":["unreal"]},"args":[{"lit":"Material"},{"lit":"/Engine/EngineMaterials"}]},"stages":[{"op":"count"}]})RQL"));
			TestTrue(*FString::Printf(TEXT("engine materials (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Count)), UnrealMcpRqlSpecHelpers::Number(Count) >= 2.0);
			const TSharedPtr<FJsonObject> First = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.assets("Material", "/Engine/EngineMaterials") | first 2 | select Name Path */ TEXT(R"RQL({"rql":"0.1","source":{"call":"assets","on":{"path":["unreal"]},"args":[{"lit":"Material"},{"lit":"/Engine/EngineMaterials"}]},"stages":[{"op":"first","n":2},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Path","expr":{"path":["Path"]}}]}]})RQL"));
			TestEqual(TEXT("first 2"), UnrealMcpRqlSpecHelpers::RowCount(First), 2);
			TestTrue(TEXT("sorted by path"), UnrealMcpRqlSpecHelpers::CellText(First, 0, 1) <= UnrealMcpRqlSpecHelpers::CellText(First, 1, 1));
			const TSharedPtr<FJsonObject> Unknown = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.assets("NoSuchRqlSpecClass") | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"assets","on":{"path":["unreal"]},"args":[{"lit":"NoSuchRqlSpecClass"}]},"stages":[{"op":"count"}]})RQL"));
			TestFalse(TEXT("unknown class fails"), UnrealMcpRqlSpecHelpers::IsOk(Unknown));
			TestTrue(*FString::Printf(TEXT("not-found (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Unknown)), UnrealMcpRqlSpecHelpers::Codes(Unknown).Contains(TEXT("not-found")));
			const TSharedPtr<FJsonObject> Missing = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.asset("/Game/RqlSpecAdapters/Nothing") | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecAdapters/Nothing"}]},"stages":[{"op":"count"}]})RQL"));
			TestEqual(TEXT("a missing asset is null (no rows)"), UnrealMcpRqlSpecHelpers::Number(Missing), 0.0);
			const TSharedPtr<FJsonObject> Classes = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.classes("Light", "Point") | select Name Parent Abstract Blueprint */ TEXT(R"RQL({"rql":"0.1","source":{"call":"classes","on":{"path":["unreal"]},"args":[{"lit":"Light"},{"lit":"Point"}]},"stages":[{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Parent","expr":{"path":["Parent"]}},{"name":"Abstract","expr":{"path":["Abstract"]}},{"name":"Blueprint","expr":{"path":["Blueprint"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("unreal.classes (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Classes)), UnrealMcpRqlSpecHelpers::RowCount(Classes) >= 1);
			TestEqual(TEXT("PointLight listed"), UnrealMcpRqlSpecHelpers::CellText(Classes, 0, 0), FString(TEXT("PointLight")));
		});

		It("lists engine and plugin content when the project has no asset of the class and no path is given", [this]()
		{
			// Fonts ship in /Engine/EngineFonts; a project without one of its own gets those instead of an empty list.
			TArray<FAssetData> InGame;
			FARFilter Filter;
			Filter.ClassPaths.Add(UFont::StaticClass()->GetClassPathName());
			Filter.PackagePaths.Add(FName(TEXT("/Game")));
			Filter.bRecursivePaths = true;
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, InGame);
			if (InGame.Num() > 0)
			{
				AddInfo(TEXT("the host project has fonts of its own: the fallback does not apply"));
				return;
			}
			const TSharedPtr<FJsonObject> Fonts = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.assets("Font") | where Path starts-with "/Engine/" | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"assets","on":{"path":["unreal"]},"args":[{"lit":"Font"}]},"stages":[{"op":"where","pred":{"op":"starts-with","args":[{"path":["Path"]},{"lit":"/Engine/"}]}},{"op":"count"}]})RQL"));
			TestTrue(*FString::Printf(TEXT("engine fonts (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Fonts)), UnrealMcpRqlSpecHelpers::Number(Fonts) >= 1.0);
			const TSharedPtr<FJsonObject> GameOnly = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.assets("Font", "/Game") | count */ TEXT(R"RQL({"rql":"0.1","source":{"call":"assets","on":{"path":["unreal"]},"args":[{"lit":"Font"},{"lit":"/Game"}]},"stages":[{"op":"count"}]})RQL"));
			TestEqual(TEXT("an explicit path stays exact"), UnrealMcpRqlSpecHelpers::Number(GameOnly), 0.0);
			// A mutation never widens to engine content (dry run: a failure here changes nothing).
			bool bOk = false;
			const FString Json = UnrealMcpRqlTools::ExecutePlan(/* unreal.assets("Font") | set LegacyFontSize = 10 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"assets","on":{"path":["unreal"]},"args":[{"lit":"Font"}]},"stages":[{"op":"set","assign":[{"target":{"path":["LegacyFontSize"]},"value":{"lit":10}}]}]})RQL"),
				/*bAllowMutation*/ true, /*bDryRun*/ true, bOk);
			TestTrue(*FString::Printf(TEXT("a mutation reaches no engine font (%s)"), *Json.Left(300)), bOk && !Json.Contains(TEXT("Font#")));
		});

		It("reads the console newest first", [this]()
		{
			FUnrealMcpLogCollector& Collector = FUnrealMcpLogCollector::Get();
			const bool bWasRegistered = Collector.IsRegistered();
			Collector.Startup();
			UE_LOG(LogTemp, Display, TEXT("RqlAdaptersSpec marker one"));
			UE_LOG(LogTemp, Display, TEXT("RqlAdaptersSpec marker two"));
			GLog->Flush();
			const TSharedPtr<FJsonObject> Last = UnrealMcpRqlSpecHelpers::Exec(*this, /* console | where Message contains "RqlAdaptersSpec marker" | sort Sequence desc | first 1 | select Verbosity Category Message */ TEXT(R"RQL({"rql":"0.1","source":{"path":["console"]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Message"]},{"lit":"RqlAdaptersSpec marker"}]}},{"op":"sort","keys":[{"expr":{"path":["Sequence"]},"desc":true}]},{"op":"first","n":1},{"op":"select","fields":[{"name":"Verbosity","expr":{"path":["Verbosity"]}},{"name":"Category","expr":{"path":["Category"]}},{"name":"Message","expr":{"path":["Message"]}}]}]})RQL"));
			TestEqual(*FString::Printf(TEXT("one row (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Last)), UnrealMcpRqlSpecHelpers::RowCount(Last), 1);
			TestEqual(TEXT("verbosity"), UnrealMcpRqlSpecHelpers::CellText(Last, 0, 0), FString(TEXT("Display")));
			TestEqual(TEXT("category"), UnrealMcpRqlSpecHelpers::CellText(Last, 0, 1), FString(TEXT("LogTemp")));
			TestTrue(TEXT("newest first"), UnrealMcpRqlSpecHelpers::CellText(Last, 0, 2).Contains(TEXT("marker two")));
			const TSharedPtr<FJsonObject> Both = UnrealMcpRqlSpecHelpers::Exec(*this, /* console | where Message contains "RqlAdaptersSpec marker" | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["console"]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Message"]},{"lit":"RqlAdaptersSpec marker"}]}},{"op":"count"}]})RQL"));
			TestTrue(TEXT("both lines captured"), UnrealMcpRqlSpecHelpers::Number(Both) >= 2.0);
			if (!bWasRegistered)
				Collector.Shutdown();
		});
	});

	Describe("material parameters", [this]()
	{
		It("reads parameter maps of materials and writes them on material instances only", [this]()
		{
			const TSharedPtr<FJsonObject> Created = UnrealMcpRqlSpecHelpers::Exec(*this,
				/* editor | call CreateMaterialInstance("/Game/RqlSpecAdapters/MI_Spec", "/Engine/BasicShapes/BasicShapeMaterial") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"CreateMaterialInstance","args":[{"lit":"/Game/RqlSpecAdapters/MI_Spec"},{"lit":"/Engine/BasicShapes/BasicShapeMaterial"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("instance created (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Created)), UnrealMcpRqlSpecHelpers::IsOk(Created));
			UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(nullptr, TEXT("/Game/RqlSpecAdapters/MI_Spec.MI_Spec"));
			if (!TestNotNull(TEXT("instance"), Instance))
				return;
			const TSharedPtr<FJsonObject> Keys = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.asset("/Game/RqlSpecAdapters/MI_Spec") | get Vectors | select Key */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecAdapters/MI_Spec"}]},"stages":[{"op":"get","expr":{"path":["Vectors"]}},{"op":"select","fields":[{"name":"Key","expr":{"path":["Key"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("the base material has vector parameters (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Keys)), UnrealMcpRqlSpecHelpers::RowCount(Keys) >= 1);
			const FString Parameter = UnrealMcpRqlSpecHelpers::CellText(Keys, 0, 0);
			const TSharedPtr<FJsonObject> Set = UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(
				/* unreal.asset("/Game/RqlSpecAdapters/MI_Spec") | set Vectors["@P@"] = "#00ff00" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecAdapters/MI_Spec"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vectors",{"key":"@P@"}]},"value":{"lit":"#00ff00"}}]}]})RQL"), { { TEXT("P"), Parameter } }), true);
			TestTrue(*FString::Printf(TEXT("instance parameter set (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Set)), UnrealMcpRqlSpecHelpers::IsOk(Set));
			const TSharedPtr<FJsonObject> Read = UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(
				/* unreal.asset("/Game/RqlSpecAdapters/MI_Spec") | select Vectors["@P@"] */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecAdapters/MI_Spec"}]},"stages":[{"op":"select","fields":[{"name":"Vectors[\"@P@\"]","expr":{"path":["Vectors",{"key":"@P@"}]}}]}]})RQL"), { { TEXT("P"), Parameter } }));
			TestEqual(TEXT("reads back"), UnrealMcpRqlSpecHelpers::CellText(Read), FString(TEXT("(0, 1, 0, 1)")));
			FLinearColor Stored;
			TestTrue(TEXT("stored as an override"), Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(*Parameter)), Stored) && Stored.Equals(FLinearColor(0, 1, 0, 1)));

			const TSharedPtr<FJsonObject> Unknown = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.asset("/Game/RqlSpecAdapters/MI_Spec") | set Scalars["NoSuchRqlParameter"] = 1 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecAdapters/MI_Spec"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Scalars",{"key":"NoSuchRqlParameter"}]},"value":{"lit":1}}]}]})RQL"), true);
			TestFalse(TEXT("unknown parameter refused"), UnrealMcpRqlSpecHelpers::IsOk(Unknown));
			const TSharedPtr<FJsonObject> Base = UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(
				/* unreal.asset("/Engine/BasicShapes/BasicShapeMaterial") | set Vectors["@P@"] = "#ff0000" */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Engine/BasicShapes/BasicShapeMaterial"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Vectors",{"key":"@P@"}]},"value":{"lit":"#ff0000"}}]}]})RQL"), { { TEXT("P"), Parameter } }), true);
			TestFalse(TEXT("a base material is not writable"), UnrealMcpRqlSpecHelpers::IsOk(Base));
			UnrealMcpRqlSpecHelpers::DropObject(Instance);
		});
	});

	Describe("class hooks", [this]()
	{
		It("gives every object Id, Name and Path, and refs resolve by Id", [this]()
		{
			const TSharedPtr<FJsonObject> Read = Run(/* ref("UnrealMcpRqlSpecFixture#@F@") | select Id Name Path $ref */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Id","expr":{"path":["Id"]}},{"name":"Name","expr":{"path":["Name"]}},{"name":"Path","expr":{"path":["Path"]}},{"name":"$ref","expr":{"path":[{"meta":"ref"}]}}]}]})RQL"));
			TestEqual(TEXT("name"), UnrealMcpRqlSpecHelpers::CellText(Read, 0, 1), FString(TEXT("Fixture")));
			TestEqual(TEXT("path"), UnrealMcpRqlSpecHelpers::CellText(Read, 0, 2), FString(TEXT("/Game/RqlSpecAdapters/Fixture.Fixture")));
			TestTrue(TEXT("$ref is Type#Id"), UnrealMcpRqlSpecHelpers::CellText(Read, 0, 3).StartsWith(TEXT("UnrealMcpRqlSpecFixture#")));
			Refuses(TEXT("Id is read-only"), /* ref("UnrealMcpRqlSpecFixture#@F@") | set Id = 1 */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Id"]},"value":{"lit":1}}]}]})RQL"), TEXT("readonly-member"));
			const TSharedPtr<FJsonObject> WrongType = Run(/* ref("PointLight#@F@") | select Name */ TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"PointLight#@F@"}]},"stages":[{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}}]}]})RQL"));
			TestEqual(*FString::Printf(TEXT("a ref of another type finds nothing (%s)"), *UnrealMcpRqlSpecHelpers::Codes(WrongType)),
				UnrealMcpRqlSpecHelpers::RowCount(WrongType), 0);
		});

		It("writes actor rotation, scale, folder, label and attachment", [this]()
		{
			UnrealMcpRqlSpecHelpers::Exec(*this, /* editor | call SpawnActor("PointLight", "RqlAdaptersLamp", "(0, 0, 0)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlAdaptersLamp"},{"lit":"(0, 0, 0)"}]}]})RQL"), true);
			UnrealMcpRqlSpecHelpers::Exec(*this, /* editor | call SpawnActor("PointLight", "RqlAdaptersLamp2", "(100, 0, 0)") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlAdaptersLamp2"},{"lit":"(100, 0, 0)"}]}]})RQL"), true);
			AActor* A = Lamp(TEXT("RqlAdaptersLamp"));
			AActor* B = Lamp(TEXT("RqlAdaptersLamp2"));
			if (!TestTrue(TEXT("spawned"), A && B))
				return;
			const TSharedPtr<FJsonObject> Set = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp" | set Rotation = "(0, 90, 0)", Scale = "(2, 2, 2)", Folder = "RqlSpec/Lights" */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp"}]}},{"op":"set","assign":[{"target":{"path":["Rotation"]},"value":{"lit":"(0, 90, 0)"}},{"target":{"path":["Scale"]},"value":{"lit":"(2, 2, 2)"}},{"target":{"path":["Folder"]},"value":{"lit":"RqlSpec/Lights"}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("transform set (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Set)), UnrealMcpRqlSpecHelpers::IsOk(Set));
			TestTrue(TEXT("yaw 90"), FMath::IsNearlyEqual(A->GetActorRotation().Yaw, 90.0, 0.01));
			TestTrue(TEXT("scale 2"), A->GetActorScale3D().Equals(FVector(2, 2, 2)));
			TestEqual(TEXT("folder"), A->GetFolderPath().ToString(), FString(TEXT("RqlSpec/Lights")));

			const TSharedPtr<FJsonObject> Attach = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp2" | set Parent = unreal.find("RqlAdaptersLamp") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp2"}]}},{"op":"set","assign":[{"target":{"path":["Parent"]},"value":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlAdaptersLamp"}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("attached (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Attach)), UnrealMcpRqlSpecHelpers::IsOk(Attach) && B->GetAttachParentActor() == A);
			const TSharedPtr<FJsonObject> Children = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp" | get Children | select Label */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp"}]}},{"op":"get","expr":{"path":["Children"]}},{"op":"select","fields":[{"name":"Label","expr":{"path":["Label"]}}]}]})RQL"));
			TestEqual(TEXT("children"), UnrealMcpRqlSpecHelpers::CellText(Children, 0, 0), FString(TEXT("RqlAdaptersLamp2")));
			const TSharedPtr<FJsonObject> Cycle = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp" | set Parent = unreal.find("RqlAdaptersLamp2") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp"}]}},{"op":"set","assign":[{"target":{"path":["Parent"]},"value":{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"RqlAdaptersLamp2"}]}}]}]})RQL"), true);
			TestFalse(TEXT("a cycle is refused"), UnrealMcpRqlSpecHelpers::IsOk(Cycle));
			TestTrue(*FString::Printf(TEXT("says cycle (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Cycle)), UnrealMcpRqlSpecHelpers::Codes(Cycle).Contains(TEXT("cycle")));
			const TSharedPtr<FJsonObject> Detach = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp2" | set Parent = null */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp2"}]}},{"op":"set","assign":[{"target":{"path":["Parent"]},"value":{"lit":null}}]}]})RQL"), true);
			TestTrue(TEXT("detached"), UnrealMcpRqlSpecHelpers::IsOk(Detach) && B->GetAttachParentActor() == nullptr);
			TestTrue(TEXT("world transform kept on detach"), B->GetActorLocation().Equals(FVector(100, 0, 0), 0.1));

			const TSharedPtr<FJsonObject> Rename = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp2" | set Label = "RqlAdaptersLampRenamed" */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp2"}]}},{"op":"set","assign":[{"target":{"path":["Label"]},"value":{"lit":"RqlAdaptersLampRenamed"}}]}]})RQL"), true);
			TestTrue(TEXT("label set"), UnrealMcpRqlSpecHelpers::IsOk(Rename) && B->GetActorLabel() == TEXT("RqlAdaptersLampRenamed"));
			const TSharedPtr<FJsonObject> Level = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp" | select Level.IsPersistent Components */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp"}]}},{"op":"select","fields":[{"name":"Level.IsPersistent","expr":{"path":["Level","IsPersistent"]}},{"name":"Components","expr":{"path":["Components"]}}]}]})RQL"));
			TestEqual(TEXT("in the persistent level"), UnrealMcpRqlSpecHelpers::CellText(Level, 0, 0), FString(TEXT("true")));
		});

		It("gives components their owner and refuses destroying class components", [this]()
		{
			UnrealMcpRqlSpecHelpers::Exec(*this, /* editor | call SpawnActor("PointLight", "RqlAdaptersLamp") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"SpawnActor","args":[{"lit":"PointLight"},{"lit":"RqlAdaptersLamp"}]}]})RQL"), true);
			const TSharedPtr<FJsonObject> Owner = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp" | get Components | where $ is PointLightComponent | select Owner.Label */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"PointLightComponent"}]}},{"op":"select","fields":[{"name":"Owner.Label","expr":{"path":["Owner","Label"]}}]}]})RQL"));
			TestEqual(TEXT("owner"), UnrealMcpRqlSpecHelpers::CellText(Owner, 0, 0), FString(TEXT("RqlAdaptersLamp")));
			const TSharedPtr<FJsonObject> Destroy = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp" | get Components | where $ is PointLightComponent | call Destroy() */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"PointLightComponent"}]}},{"op":"call","method":"Destroy","args":[]}]})RQL"), true);
			TestFalse(TEXT("class component refused"), UnrealMcpRqlSpecHelpers::IsOk(Destroy));
			TestTrue(*FString::Printf(TEXT("says why (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Destroy)), UnrealMcpRqlSpecHelpers::Codes(Destroy).Contains(TEXT("only components added")));
			const TSharedPtr<FJsonObject> Bad = UnrealMcpRqlSpecHelpers::Exec(*this, /* actors | where Label == "RqlAdaptersLamp" | call AddComponent("PointLight") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlAdaptersLamp"}]}},{"op":"call","method":"AddComponent","args":[{"lit":"PointLight"}]}]})RQL"), true);
			TestFalse(TEXT("an actor class is not a component class"), UnrealMcpRqlSpecHelpers::IsOk(Bad));
		});

		It("reads levels and the world, and switches the current level", [this]()
		{
			const TSharedPtr<FJsonObject> Levels = UnrealMcpRqlSpecHelpers::Exec(*this, /* levels | where IsPersistent | select Map Package IsVisible IsCurrent Actors */ TEXT(R"RQL({"rql":"0.1","source":{"path":["levels"]},"stages":[{"op":"where","pred":{"path":["IsPersistent"]}},{"op":"select","fields":[{"name":"Map","expr":{"path":["Map"]}},{"name":"Package","expr":{"path":["Package"]}},{"name":"IsVisible","expr":{"path":["IsVisible"]}},{"name":"IsCurrent","expr":{"path":["IsCurrent"]}},{"name":"Actors","expr":{"path":["Actors"]}}]}]})RQL"));
			TestEqual(TEXT("one persistent level"), UnrealMcpRqlSpecHelpers::RowCount(Levels), 1);
			TestTrue(TEXT("a package path"), UnrealMcpRqlSpecHelpers::CellText(Levels, 0, 1).StartsWith(TEXT("/")));
			const TSharedPtr<FJsonObject> Refuse = UnrealMcpRqlSpecHelpers::Exec(*this, /* levels | where IsPersistent | set IsCurrent = false */ TEXT(R"RQL({"rql":"0.1","source":{"path":["levels"]},"stages":[{"op":"where","pred":{"path":["IsPersistent"]}},{"op":"set","assign":[{"target":{"path":["IsCurrent"]},"value":{"lit":false}}]}]})RQL"), true);
			TestFalse(TEXT("IsCurrent = false refused"), UnrealMcpRqlSpecHelpers::IsOk(Refuse));
			const TSharedPtr<FJsonObject> WorldRead = UnrealMcpRqlSpecHelpers::Exec(*this, /* world | select Map Levels PersistentLevel.IsPersistent CurrentLevel */ TEXT(R"RQL({"rql":"0.1","source":{"path":["world"]},"stages":[{"op":"select","fields":[{"name":"Map","expr":{"path":["Map"]}},{"name":"Levels","expr":{"path":["Levels"]}},{"name":"PersistentLevel.IsPersistent","expr":{"path":["PersistentLevel","IsPersistent"]}},{"name":"CurrentLevel","expr":{"path":["CurrentLevel"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("world ok (%s)"), *UnrealMcpRqlSpecHelpers::Codes(WorldRead)), UnrealMcpRqlSpecHelpers::IsOk(WorldRead));
			TestEqual(TEXT("persistent level"), UnrealMcpRqlSpecHelpers::CellText(WorldRead, 0, 2), FString(TEXT("true")));
			const TSharedPtr<FJsonObject> Current = UnrealMcpRqlSpecHelpers::Exec(*this, /* world | set CurrentLevel = PersistentLevel */ TEXT(R"RQL({"rql":"0.1","source":{"path":["world"]},"stages":[{"op":"set","assign":[{"target":{"path":["CurrentLevel"]},"value":{"path":["PersistentLevel"]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("current level set (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Current)), UnrealMcpRqlSpecHelpers::IsOk(Current));
			const TSharedPtr<FJsonObject> Wrong = UnrealMcpRqlSpecHelpers::Exec(*this, /* world | set CurrentLevel = null */ TEXT(R"RQL({"rql":"0.1","source":{"path":["world"]},"stages":[{"op":"set","assign":[{"target":{"path":["CurrentLevel"]},"value":{"lit":null}}]}]})RQL"), true);
			TestFalse(TEXT("null is not a level"), UnrealMcpRqlSpecHelpers::IsOk(Wrong));
		});

		It("reads the editor state and refuses pausing outside play", [this]()
		{
			const TSharedPtr<FJsonObject> State = UnrealMcpRqlSpecHelpers::Exec(*this, /* editor | select IsPlaying IsPaused World.Map CurrentLevel.IsPersistent */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"select","fields":[{"name":"IsPlaying","expr":{"path":["IsPlaying"]}},{"name":"IsPaused","expr":{"path":["IsPaused"]}},{"name":"World.Map","expr":{"path":["World","Map"]}},{"name":"CurrentLevel.IsPersistent","expr":{"path":["CurrentLevel","IsPersistent"]}}]}]})RQL"));
			TestEqual(TEXT("not playing"), UnrealMcpRqlSpecHelpers::CellText(State, 0, 0), FString(TEXT("false")));
			TestEqual(TEXT("not paused"), UnrealMcpRqlSpecHelpers::CellText(State, 0, 1), FString(TEXT("false")));
			const TSharedPtr<FJsonObject> Pause = UnrealMcpRqlSpecHelpers::Exec(*this, /* editor | set IsPaused = true */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"set","assign":[{"target":{"path":["IsPaused"]},"value":{"lit":true}}]}]})RQL"), true);
			TestFalse(TEXT("pause refused"), UnrealMcpRqlSpecHelpers::IsOk(Pause));
			TestTrue(*FString::Printf(TEXT("not playing (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Pause)), UnrealMcpRqlSpecHelpers::Codes(Pause).Contains(TEXT("not playing")));
			const TSharedPtr<FJsonObject> Selection = UnrealMcpRqlSpecHelpers::Exec(*this, /* editor | set Selection = [1] */ TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"set","assign":[{"target":{"path":["Selection"]},"value":{"list":[{"lit":1}]}}]}]})RQL"), true);
			TestFalse(TEXT("a number is not an actor"), UnrealMcpRqlSpecHelpers::IsOk(Selection));
		});
	});

	Describe("extension seam", [this]()
	{
		It("registers sources, members, operations and guides of an IUnrealMcpRqlModule", [this]()
		{
			FUnrealMcpRqlAdaptersSpecModule Module;
			UUnrealMcpRqlSpecFixture* Current = Fixture;
			Module.Fixture = [Current]() -> UObject* { return Current; };
			Fixture->HiddenValue = 3;
			IModularFeatures::Get().RegisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), &Module);
			ON_SCOPE_EXIT { IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), &Module); };

			const TSharedPtr<FJsonObject> Defaults = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecLights | where $type == "PointLight" | select Name */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecLights"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"PointLight"}]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}}]}]})RQL"));
			TestEqual(*FString::Printf(TEXT("class defaults source (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Defaults)), UnrealMcpRqlSpecHelpers::CellText(Defaults, 0, 0), FString(TEXT("Default__PointLight")));
			const TSharedPtr<FJsonObject> Abstract = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecLights | where $type == "Light" | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecLights"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"Light"}]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("abstract classes left out"), UnrealMcpRqlSpecHelpers::Number(Abstract), 0.0);
			const TSharedPtr<FJsonObject> WriteDefault = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecLights | where $type == "PointLight" | set bHidden = true */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecLights"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"PointLight"}]}},{"op":"set","assign":[{"target":{"path":["bHidden"]},"value":{"lit":true}}]}]})RQL"), true);
			TestFalse(TEXT("class defaults are read-only"), UnrealMcpRqlSpecHelpers::IsOk(WriteDefault));

			const TSharedPtr<FJsonObject> Members = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecFixture | select SpecJson.a SpecSelf.Name SpecList HiddenValue */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecFixture"]},"stages":[{"op":"select","fields":[{"name":"SpecJson.a","expr":{"path":["SpecJson","a"]}},{"name":"SpecSelf.Name","expr":{"path":["SpecSelf","Name"]}},{"name":"SpecList","expr":{"path":["SpecList"]}},{"name":"HiddenValue","expr":{"path":["HiddenValue"]}}]}]})RQL"));
			TestTrue(*FString::Printf(TEXT("members ok (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Members)), UnrealMcpRqlSpecHelpers::IsOk(Members));
			TestEqual(TEXT("json member"), UnrealMcpRqlSpecHelpers::CellText(Members, 0, 0), FString(TEXT("1")));
			TestEqual(TEXT("object member"), UnrealMcpRqlSpecHelpers::CellText(Members, 0, 1), FString(TEXT("Fixture")));
			TestEqual(TEXT("exposed hidden property"), UnrealMcpRqlSpecHelpers::CellText(Members, 0, 3), FString(TEXT("3")));
			const TSharedPtr<FJsonObject> ListCount = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecFixtures | get SpecList | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecFixtures"]},"stages":[{"op":"get","expr":{"path":["SpecList"]}},{"op":"count"}]})RQL"));
			TestEqual(TEXT("object list member"), UnrealMcpRqlSpecHelpers::Number(ListCount), 2.0);
			const TSharedPtr<FJsonObject> Exposed = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecFixture | set HiddenValue = 8 */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecFixture"]},"stages":[{"op":"set","assign":[{"target":{"path":["HiddenValue"]},"value":{"lit":8}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("exposed writable (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Exposed)), UnrealMcpRqlSpecHelpers::IsOk(Exposed) && Fixture->HiddenValue == 8);
			const TSharedPtr<FJsonObject> Json = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecFixture | set SpecJson = 1 */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecFixture"]},"stages":[{"op":"set","assign":[{"target":{"path":["SpecJson"]},"value":{"lit":1}}]}]})RQL"), true);
			TestFalse(TEXT("json members are read-only"), UnrealMcpRqlSpecHelpers::IsOk(Json));
			const TSharedPtr<FJsonObject> Assets = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecMaterials | count */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecMaterials"]},"stages":[{"op":"count"}]})RQL"));
			TestTrue(TEXT("asset source ok"), UnrealMcpRqlSpecHelpers::IsOk(Assets));

			const TSharedPtr<FJsonObject> Add = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecApi | call Add(40) */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecApi"]},"stages":[{"op":"call","method":"Add","args":[{"lit":40}]}]})RQL"), true);
			TestEqual(*FString::Printf(TEXT("operation with a C++ default (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Add)), UnrealMcpRqlSpecHelpers::CellText(Add, 0, 2), FString(TEXT("42")));
			const TSharedPtr<FJsonObject> Fail = UnrealMcpRqlSpecHelpers::Exec(*this, /* rqlSpecApi | call Boom("bad input") */ TEXT(R"RQL({"rql":"0.1","source":{"path":["rqlSpecApi"]},"stages":[{"op":"call","method":"Boom","args":[{"lit":"bad input"}]}]})RQL"), true);
			TestFalse(TEXT("UnrealMcpRql::Fail fails the call"), UnrealMcpRqlSpecHelpers::IsOk(Fail));
			TestTrue(*FString::Printf(TEXT("with its message (%s)"), *UnrealMcpRqlSpecHelpers::Codes(Fail)), UnrealMcpRqlSpecHelpers::Codes(Fail).Contains(TEXT("bad input")));

			FUnrealMcpToolRegistry Registry;
			UnrealMcpRqlTools::Register(Registry);
			const FUnrealMcpRegisteredTool* Query = Registry.Find(TEXT("rql-query"));
			const FString Skill = Query && Query->Skills.Num() == 1 ? Query->Skills[0].Markdown : FString();
			TestTrue(TEXT("guide rendered"), Skill.Contains(TEXT("## RqlAdaptersSpec guide")) && Skill.Contains(TEXT("the rqlSpec guide body")));
			TestTrue(TEXT("module listed"), Skill.Contains(TEXT("`RqlAdaptersSpec`")));
			TestTrue(TEXT("its sources listed"), Skill.Contains(TEXT("| `rqlSpecLights` |")));
			bool bOk = false;
			const TSharedPtr<FJsonObject> Schema = UnrealMcpRqlSpecHelpers::Parse(UnrealMcpRqlTools::Schema(TEXT("RqlSpecApi"), bOk));
			TestTrue(TEXT("the api type is in the schema under its name"), bOk && Schema.IsValid());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
