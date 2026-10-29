// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"

#include "ControlRigBlueprintLegacy.h"
#include "Rigs/RigHierarchy.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Highlevel/Hierarchy/RigUnit_TwoBoneIKSimple.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions").
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::RunPlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it.
//
//  Run via:  Automation RunTests UnrealAIControlRig
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIControlRigSpec, "UnrealAIControlRig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIControlRigSpec)

void FUnrealAIControlRigSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-control-rig"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("ControlRig")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `controlRigs` |"), Skill.Contains(TEXT("| `controlRigs` |")));
			TestTrue(TEXT("## Control Rig"), Skill.Contains(TEXT("## Control Rig")));
		});
	});

	Describe("source: controlRigs", [this]()
	{
		It("answers a query (count of the project's Control Rig blueprints)", [this]()
		{
			// controlRigs | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRigs"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("reads the hierarchy members of every rig", [this]()
		{
			// controlRigs | select Name Elements Controls Bones
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRigs"]},"stages":[{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}},{"name":"Elements","expr":{"path":["Elements"]}},{"name":"Controls","expr":{"path":["Controls"]}},{"name":"Bones","expr":{"path":["Bones"]}}]}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});
	});

	Describe("operations", [this]()
	{
		It("lists the Control Rig operations", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("ControlRigOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Method : { TEXT("CreateRig"), TEXT("AddBone"), TEXT("AddControl"), TEXT("AddTwoBoneIK") })
				TestTrue(*FString::Printf(TEXT("lists %s"), Method), Schema.Contains(FString(Method) + TEXT("(")));
		});

		It("builds a two-bone arm with an IK target control; refuses unknown elements and a taken path", [this]()
		{
			const FString Name = TEXT("CR_RqlSpecArm_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Path = TEXT("/Game/RqlSpec/") + Name;
			auto Run = [&Path](const TCHAR* Plan, TSharedPtr<FJsonObject>* Out = nullptr)
			{
				return UnrealMcpRqlTools::RunPlan(FString(Plan).Replace(TEXT("@RIG@"), *Path), true, Out);
			};
			auto Says = [this, &Run](const TCHAR* What, const TCHAR* Plan, const TCHAR* Expected)
			{
				TSharedPtr<FJsonObject> Result;
				TestFalse(*FString::Printf(TEXT("%s: refused"), What), Run(Plan, &Result));
				FString Text;
				if (Result.IsValid())
					FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
				TestTrue(*FString::Printf(TEXT("%s: says '%s' (%s)"), What, Expected, *Text.Left(400)), Text.Contains(Expected));
			};

			// controlRig | call CreateRig("@RIG@")
			TestTrue(TEXT("rig created"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"CreateRig","args":[{"lit":"@RIG@"}]}]})RQL")));
			UControlRigBlueprint* Rig = FindObject<UControlRigBlueprint>(nullptr, *(Path + TEXT(".") + Name));
			if (!TestNotNull(TEXT("the rig asset exists"), Rig))
				return;
			// controlRig | call AddBone(unreal.asset("@RIG@"), "Shoulder", "", [0, 0, 150])
			TestTrue(TEXT("Shoulder added"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddBone","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"Shoulder"},{"lit":""},{"list":[{"lit":0},{"lit":0},{"lit":150}]}]}]})RQL")));
			// controlRig | call AddBone(unreal.asset("@RIG@"), "Elbow", "Shoulder", [30, 0, 0])
			TestTrue(TEXT("Elbow added"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddBone","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"Elbow"},{"lit":"Shoulder"},{"list":[{"lit":30},{"lit":0},{"lit":0}]}]}]})RQL")));
			// controlRig | call AddBone(unreal.asset("@RIG@"), "Hand", "Elbow", [30, 0, 0])
			TestTrue(TEXT("Hand added"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddBone","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"Hand"},{"lit":"Elbow"},{"list":[{"lit":30},{"lit":0},{"lit":0}]}]}]})RQL")));
			// controlRig | call AddControl(unreal.asset("@RIG@"), "IK_Target", [60, 0, 150])
			TestTrue(TEXT("IK_Target added"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddControl","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"IK_Target"},{"list":[{"lit":60},{"lit":0},{"lit":150}]}]}]})RQL")));
			// controlRig | call AddTwoBoneIK(unreal.asset("@RIG@"), "Shoulder", "Elbow", "Hand", "IK_Target")
			TestTrue(TEXT("Basic IK added"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddTwoBoneIK","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"Shoulder"},{"lit":"Elbow"},{"lit":"Hand"},{"lit":"IK_Target"}]}]})RQL")));

			URigHierarchy* Hierarchy = Rig->GetHierarchy();
			if (TestNotNull(TEXT("hierarchy"), Hierarchy))
			{
				TestEqual(TEXT("three bones"), Hierarchy->Num(ERigElementType::Bone), 3);
				TestEqual(TEXT("one control"), Hierarchy->Num(ERigElementType::Control), 1);
				const FRigElementKey Hand(TEXT("Hand"), ERigElementType::Bone);
				TestEqual(TEXT("Hand's parent is Elbow"), Hierarchy->GetFirstParent(Hand).Name, FName(TEXT("Elbow")));
				const FVector HandAt = Hierarchy->GetGlobalTransform(Hand).GetLocation();
				TestTrue(*FString::Printf(TEXT("Hand at (60, 0, 150) (%s)"), *HandAt.ToString()), HandAt.Equals(FVector(60.0, 0.0, 150.0), 0.1));
			}
			const URigVMUnitNode* IK = nullptr;
			const URigVMUnitNode* Solve = nullptr;
			if (const URigVMGraph* Graph = Rig->GetModel())
			{
				for (const URigVMNode* Node : Graph->GetNodes())
				{
					const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node);
					IK = Unit && Unit->GetScriptStruct() == FRigUnit_TwoBoneIKSimplePerItem::StaticStruct() ? Unit : IK;
					Solve = Unit && Unit->GetScriptStruct() == FRigUnit_BeginExecution::StaticStruct() ? Unit : Solve;
				}
			}
			if (TestNotNull(TEXT("a Basic IK node"), IK) && TestNotNull(TEXT("a Forwards Solve node"), Solve))
			{
				TestTrue(TEXT("ItemA is Shoulder"), IK->FindPin(TEXT("ItemA"))->GetDefaultValue().Contains(TEXT("Shoulder")));
				TestTrue(TEXT("EffectorItem is Hand"), IK->FindPin(TEXT("EffectorItem"))->GetDefaultValue().Contains(TEXT("Hand")));
				TestTrue(TEXT("Effector is driven by a link"), IK->FindPin(TEXT("Effector"))->GetLinkedSourcePins().Num() == 1);
				TestTrue(TEXT("runs on Forwards Solve"), IK->FindPin(TEXT("ExecuteContext"))->GetLinkedSourcePins().ContainsByPredicate(
					[Solve](const URigVMPin* Pin) { return Pin->GetNode() == Solve; }));
			}

			// controlRig | call AddBone(unreal.asset("@RIG@"), "Finger", "Wrist", [5, 0, 0])
			Says(TEXT("an unknown parent bone"), TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddBone","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"Finger"},{"lit":"Wrist"},{"list":[{"lit":5},{"lit":0},{"lit":0}]}]}]})RQL"), TEXT("no parent named 'Wrist'"));
			// controlRig | call AddTwoBoneIK(unreal.asset("@RIG@"), "Shoulder", "Elbow", "Hand", "Nope")
			Says(TEXT("an unknown target"), TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddTwoBoneIK","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"Shoulder"},{"lit":"Elbow"},{"lit":"Hand"},{"lit":"Nope"}]}]})RQL"), TEXT("named 'Nope'"));
			// controlRig | call AddTwoBoneIK(unreal.asset("@RIG@"), "Elbow", "Shoulder", "Hand", "IK_Target")
			Says(TEXT("bones out of chain order"), TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddTwoBoneIK","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"Elbow"},{"lit":"Shoulder"},{"lit":"Hand"},{"lit":"IK_Target"}]}]})RQL"), TEXT("is not a bone chain"));
			// controlRig | call AddBone(unreal.asset("@RIG@"), "Hand", "Elbow", [1, 0, 0])
			Says(TEXT("a taken bone name"), TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"AddBone","args":[{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@RIG@"}]},{"lit":"Hand"},{"lit":"Elbow"},{"list":[{"lit":1},{"lit":0},{"lit":0}]}]}]})RQL"), TEXT("already has an element named 'Hand'"));
			// controlRig | call CreateRig("@RIG@")
			Says(TEXT("a taken path"), TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRig"]},"stages":[{"op":"call","method":"CreateRig","args":[{"lit":"@RIG@"}]}]})RQL"), TEXT("already exists"));
			if (Hierarchy)
				TestEqual(TEXT("the refused calls added nothing"), Hierarchy->Num(ERigElementType::Bone), 3);
		});
	});

	Describe("members", [this]()
	{
		It("describes ControlRigBlueprint with its hierarchy members", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("ControlRigBlueprint"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Elements"), Schema.Contains(TEXT("\"name\":\"Elements\"")));
			TestTrue(TEXT("lists name:Hierarchy"), Schema.Contains(TEXT("\"name\":\"Hierarchy\"")));
			TestTrue(TEXT("lists name:Controls"), Schema.Contains(TEXT("\"name\":\"Controls\"")));
			TestTrue(TEXT("lists name:Bones"), Schema.Contains(TEXT("\"name\":\"Bones\"")));
		});

		It("describes ControlRigComponent with its BoundRig member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("ControlRigComponent"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:BoundRig"), Schema.Contains(TEXT("\"name\":\"BoundRig\"")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
