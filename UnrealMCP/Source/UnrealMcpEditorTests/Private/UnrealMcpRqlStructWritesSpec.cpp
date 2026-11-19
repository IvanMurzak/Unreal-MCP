// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/CollisionProfile.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/Package.h"
#include "UnrealMcpRqlSpecFixtures.h"
#include "UnrealMcpRqlSpecHelpers.h"

/**
 * `set` through a struct member path (docs/rql.md "Structs are values"): the write reaches the owner, the editor is told
 * which member changed (as the Details panel tells it: the leaf in a chain under the owner's member), engine setters
 * keep derived state (a body's collision profile and physics) in step, and a write the editor resets is refused instead
 * of reported. Plans come from Rql.Parser 0.2.0 (the query is the comment before each).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlStructWritesSpec, "UnrealMcp.Tools.Rql.StructWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	UUnrealMcpRqlSpecFixture* Fixture = nullptr;
	FString FixtureId;

	TSharedPtr<FJsonObject> Run(const FString& Plan, bool bMutate = false)
	{
		return UnrealMcpRqlSpecHelpers::Exec(*this, UnrealMcpRqlSpecHelpers::Fill(Plan, { { TEXT("F"), FixtureId } }), bMutate);
	}

	static UWorld* World()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/** A cube far from everything else, labelled "RqlStructMesh", with the StaticMeshActor's own BlockAll profile. */
	static AStaticMeshActor* SpawnCube()
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		UWorld* W = World();
		if (!Cube || !W)
			return nullptr;
		AStaticMeshActor* Actor = W->SpawnActor<AStaticMeshActor>(FVector(0.0, 0.0, 80000.0), FRotator::ZeroRotator);
		if (!Actor)
			return nullptr;
		Actor->SetActorLabel(TEXT("RqlStructMesh"));
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		return Actor;
	}

	/** Whether a downward trace through the cube's centre hits it (the physics scene, not the property). */
	static bool TraceHits(const AStaticMeshActor* Actor)
	{
		FHitResult Hit;
		const FVector Centre = Actor->GetActorLocation() + FVector(0.0, 0.0, 50.0);
		const bool bHit = World()->LineTraceSingleByChannel(Hit, Centre + FVector(0.0, 0.0, 500.0), Centre - FVector(0.0, 0.0, 500.0),
			ECC_Visibility);
		return bHit && Hit.GetActor() == Actor;
	}

	static void CleanupWorld()
	{
		if (!World())
			return;
		for (TActorIterator<AStaticMeshActor> It(World()); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == TEXT("RqlStructMesh"))
				World()->EditorDestroyActor(*It, true);
		}
		if (GEditor)
			GEditor->ResetTransaction(FText::FromString(TEXT("RQL struct write specs")));
	}

	void MakeFixture()
	{
		UPackage* Package = CreatePackage(TEXT("/Game/RqlSpecStructWrites/Fixture"));
		Fixture = NewObject<UUnrealMcpRqlSpecFixture>(Package, TEXT("Fixture"), RF_Public | RF_Standalone | RF_Transactional);
		const TSharedPtr<FJsonObject> Read = UnrealMcpRqlSpecHelpers::Exec(*this, /* unreal.asset("/Game/RqlSpecStructWrites/Fixture.Fixture") | select Id */ TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"/Game/RqlSpecStructWrites/Fixture.Fixture"}]},"stages":[{"op":"select","fields":[{"name":"Id","expr":{"path":["Id"]}}]}]})RQL"));
		FixtureId = UnrealMcpRqlSpecHelpers::CellText(Read);
		TestFalse(TEXT("the fixture has an id"), FixtureId.IsEmpty());
	}

END_DEFINE_SPEC(FUnrealMcpRqlStructWritesSpec)

void FUnrealMcpRqlStructWritesSpec::Define()
{
	using namespace UnrealMcpRqlSpecHelpers;

	Describe("a body's collision", [this]()
	{
		BeforeEach([this]() { CleanupWorld(); });
		AfterEach([this]() { CleanupWorld(); });

		It("turns off through BodyInstance.CollisionEnabled: receipt, component and physics agree", [this]()
		{
			AStaticMeshActor* Actor = SpawnCube();
			if (!TestNotNull(TEXT("cube spawned"), Actor))
				return;
			UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			TestTrue(TEXT("the trace hits the cube before"), TraceHits(Actor));

			/* actors | where Label == "RqlStructMesh" | get StaticMeshComponent | set BodyInstance.CollisionEnabled = "NoCollision" */
			const TSharedPtr<FJsonObject> Set = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlStructMesh"}]}},{"op":"get","expr":{"path":["StaticMeshComponent"]}},{"op":"set","assign":[{"target":{"path":["BodyInstance","CollisionEnabled"]},"value":{"lit":"NoCollision"}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set ok (%s)"), *Codes(Set)), IsOk(Set));
			TestEqual(TEXT("one change"), RowCount(Set), 1);
			TestTrue(*FString::Printf(TEXT("old is QueryAndPhysics (%s)"), *CellText(Set, 0, 2)), CellText(Set, 0, 2).Contains(TEXT("QueryAndPhysics")));
			TestTrue(*FString::Printf(TEXT("new is NoCollision (%s)"), *CellText(Set, 0, 3)), CellText(Set, 0, 3).Contains(TEXT("NoCollision")));
			TestEqual(TEXT("the component has no collision"), static_cast<int32>(Mesh->GetCollisionEnabled()), static_cast<int32>(ECollisionEnabled::NoCollision));
			TestEqual(TEXT("its profile is Custom, as SetCollisionEnabled leaves it"), Mesh->GetCollisionProfileName(), UCollisionProfile::CustomCollisionProfileName);
			TestFalse(TEXT("the trace no longer hits the cube"), TraceHits(Actor));
		});

		It("turns off through BodyInstance.CollisionProfileName: the profile's settings and physics follow", [this]()
		{
			AStaticMeshActor* Actor = SpawnCube();
			if (!TestNotNull(TEXT("cube spawned"), Actor))
				return;
			UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			TestTrue(TEXT("the trace hits the cube before"), TraceHits(Actor));

			/* actors | where Label == "RqlStructMesh" | get StaticMeshComponent | set BodyInstance.CollisionProfileName = "NoCollision" */
			const TSharedPtr<FJsonObject> Set = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlStructMesh"}]}},{"op":"get","expr":{"path":["StaticMeshComponent"]}},{"op":"set","assign":[{"target":{"path":["BodyInstance","CollisionProfileName"]},"value":{"lit":"NoCollision"}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set ok (%s)"), *Codes(Set)), IsOk(Set));
			TestEqual(TEXT("new is NoCollision"), CellText(Set, 0, 3), FString(TEXT("NoCollision")));
			TestEqual(TEXT("the profile is NoCollision"), Mesh->GetCollisionProfileName(), FName(TEXT("NoCollision")));
			TestEqual(TEXT("the profile's CollisionEnabled came along"), static_cast<int32>(Mesh->GetCollisionEnabled()), static_cast<int32>(ECollisionEnabled::NoCollision));
			TestFalse(TEXT("the trace no longer hits the cube"), TraceHits(Actor));

			/* actors | where Label == "RqlStructMesh" | get StaticMeshComponent | set BodyInstance.CollisionProfileName = "BlockAll" */
			const TSharedPtr<FJsonObject> Back = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlStructMesh"}]}},{"op":"get","expr":{"path":["StaticMeshComponent"]}},{"op":"set","assign":[{"target":{"path":["BodyInstance","CollisionProfileName"]},"value":{"lit":"BlockAll"}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set back ok (%s)"), *Codes(Back)), IsOk(Back));
			TestTrue(TEXT("the trace hits the cube again"), TraceHits(Actor));
		});

		It("comes back whole when the mutation fails after it (profile, the mesh's default collision, physics)", [this]()
		{
			AStaticMeshActor* Actor = SpawnCube();
			if (!TestNotNull(TEXT("cube spawned"), Actor))
				return;
			UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			const FName Profile = Mesh->GetCollisionProfileName();
			TestTrue(TEXT("a StaticMeshActor takes its collision from the mesh"), Mesh->bUseDefaultCollision);

			// OverriddenLightMapRes is rounded up to a multiple of 4 by the component: 62 comes back as the 64 it has, a reset.
			/* actors | where Label == "RqlStructMesh" | get StaticMeshComponent | set BodyInstance.CollisionEnabled = "NoCollision", OverriddenLightMapRes = 62 */
			const TSharedPtr<FJsonObject> Set = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlStructMesh"}]}},{"op":"get","expr":{"path":["StaticMeshComponent"]}},{"op":"set","assign":[{"target":{"path":["BodyInstance","CollisionEnabled"]},"value":{"lit":"NoCollision"}},{"target":{"path":["OverriddenLightMapRes"]},"value":{"lit":62}}]}]})RQL"), true);
			TestFalse(TEXT("the mutation fails"), IsOk(Set));
			TestTrue(*FString::Printf(TEXT("on the reset member (%s)"), *Codes(Set)), Codes(Set).Contains(TEXT("OverriddenLightMapRes")));
			TestEqual(TEXT("the profile is back"), Mesh->GetCollisionProfileName(), Profile);
			TestTrue(TEXT("so is the mesh's default collision"), Mesh->bUseDefaultCollision);
			TestEqual(TEXT("and collision"), static_cast<int32>(Mesh->GetCollisionEnabled()), static_cast<int32>(ECollisionEnabled::QueryAndPhysics));
			TestTrue(TEXT("the trace hits the cube"), TraceHits(Actor));
		});

		It("runs the editor's follow-up of BodyInstance.bSimulatePhysics (Movable, PhysicsActor)", [this]()
		{
			AStaticMeshActor* Actor = SpawnCube();
			if (!TestNotNull(TEXT("cube spawned"), Actor))
				return;
			UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			Mesh->SetMobility(EComponentMobility::Static);

			/* actors | where Label == "RqlStructMesh" | get StaticMeshComponent | set BodyInstance.bSimulatePhysics = true */
			const TSharedPtr<FJsonObject> Set = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"RqlStructMesh"}]}},{"op":"get","expr":{"path":["StaticMeshComponent"]}},{"op":"set","assign":[{"target":{"path":["BodyInstance","bSimulatePhysics"]},"value":{"lit":true}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set ok (%s)"), *Codes(Set)), IsOk(Set));
			TestTrue(TEXT("simulates physics"), Mesh->BodyInstance.bSimulatePhysics);
			TestEqual(TEXT("made Movable, as the Details panel does"), static_cast<int32>(Mesh->Mobility.GetValue()), static_cast<int32>(EComponentMobility::Movable));
			TestEqual(TEXT("the BlockAll profile became PhysicsActor"), Mesh->GetCollisionProfileName(), UCollisionProfile::PhysicsActor_ProfileName);
		});
	});

	Describe("a nested struct member", [this]()
	{
		BeforeEach([this]() { MakeFixture(); });
		AfterEach([this]()
		{
			DropObject(Fixture);
			Fixture = nullptr;
			FixtureId.Reset();
		});

		It("two structs deep is written and notified as that member, under the owner's member", [this]()
		{
			/* ref("UnrealMcpRqlSpecFixture#@F@") | set Nest.Inner.Damage = 5 */
			const TSharedPtr<FJsonObject> Set = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Nest","Inner","Damage"]},"value":{"lit":5}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set ok (%s)"), *Codes(Set)), IsOk(Set));
			TestEqual(TEXT("receipt new"), CellText(Set, 0, 3), FString(TEXT("5")));
			TestEqual(TEXT("written"), Fixture->Nest.Inner.Damage, 5);
			TestEqual(TEXT("the editor heard of Damage"), Fixture->LastEditProperty, FName(TEXT("Damage")));
			TestEqual(TEXT("as a member of Nest"), Fixture->LastEditMember, FName(TEXT("Nest")));
		});

		It("a list of structs takes object literals that name their struct with $type (DEV-114)", [this]()
		{
			// A wave agent's `set WeightChannels = [{$type: "SplineModifierWeightEntry", ...}]` failed: "no concrete type named
			// ... derives from SplineModifierWeightEntry". The struct's own name, with or without its F prefix, now names it.
			/* ref("UnrealMcpRqlSpecFixture#@F@") | set Bag.Rows = [{$type: "UnrealMcpRqlSpecRow", Damage: 3}, {$type: "FUnrealMcpRqlSpecRow", Damage: 4}] */
			const TSharedPtr<FJsonObject> Set = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Bag","Rows"]},"value":{"list":[{"object":[{"key":"$type","value":{"lit":"UnrealMcpRqlSpecRow"}},{"key":"Damage","value":{"lit":3}}]},{"object":[{"key":"$type","value":{"lit":"FUnrealMcpRqlSpecRow"}},{"key":"Damage","value":{"lit":4}}]}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("set ok (%s)"), *Codes(Set)), IsOk(Set));
			TestTrue(TEXT("both rows written"), Fixture->Bag.Rows.Num() == 2 && Fixture->Bag.Rows[0].Damage == 3 && Fixture->Bag.Rows[1].Damage == 4);

			// Another struct's name is no subtype of a struct: refused, naming the one it takes.
			/* ref("UnrealMcpRqlSpecFixture#@F@") | set Bag.Rows = [{$type: "UnrealMcpRqlSpecNest"}] */
			const TSharedPtr<FJsonObject> Wrong = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Bag","Rows"]},"value":{"list":[{"object":[{"key":"$type","value":{"lit":"UnrealMcpRqlSpecNest"}}]}]}}]}]})RQL"), true);
			TestFalse(TEXT("another struct is refused"), IsOk(Wrong));
			TestTrue(*FString::Printf(TEXT("naming the struct the list takes (%s)"), *Codes(Wrong)),
				Codes(Wrong).Contains(TEXT("UnrealMcpRqlSpecNest")) && Codes(Wrong).Contains(TEXT("UnrealMcpRqlSpecRow")));
			TestEqual(TEXT("the rows are unchanged"), Fixture->Bag.Rows.Num(), 2);
		});

		It("that the editor resets is refused with the reason, never reported as a change", [this]()
		{
			/* ref("UnrealMcpRqlSpecFixture#@F@") | set Nest.Locked = 3 */
			const TSharedPtr<FJsonObject> Nested = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Nest","Locked"]},"value":{"lit":3}}]}]})RQL"), true);
			TestFalse(TEXT("nested: refused"), IsOk(Nested));
			TestTrue(*FString::Printf(TEXT("nested: names the member and the reset (%s)"), *Codes(Nested)),
				Codes(Nested).Contains(TEXT("Nest.Locked")) && Codes(Nested).Contains(TEXT("reset")));
			TestEqual(TEXT("nested: unchanged"), Fixture->Nest.Locked, 0);

			/* ref("UnrealMcpRqlSpecFixture#@F@") | set Pinned = 5 */
			const TSharedPtr<FJsonObject> Top = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Pinned"]},"value":{"lit":5}}]}]})RQL"), true);
			TestFalse(TEXT("top level: refused"), IsOk(Top));
			TestTrue(*FString::Printf(TEXT("top level: names the member (%s)"), *Codes(Top)), Codes(Top).Contains(TEXT("Pinned")) && Codes(Top).Contains(TEXT("reset")));
			TestEqual(TEXT("top level: unchanged"), Fixture->Pinned, 1);

			/* ref("UnrealMcpRqlSpecFixture#@F@") | set Pinned = 1 */
			const TSharedPtr<FJsonObject> Same = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"UnrealMcpRqlSpecFixture#@F@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Pinned"]},"value":{"lit":1}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("the value it already has: ok, no change (%s)"), *Codes(Same)), IsOk(Same) && RowCount(Same) == 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
