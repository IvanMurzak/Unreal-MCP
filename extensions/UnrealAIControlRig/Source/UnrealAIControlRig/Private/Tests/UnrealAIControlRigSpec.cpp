// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealAIControlRigAnim.h"
#include "UnrealAIControlRigGraph.h"
#include "UnrealAIControlRigUnits.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ControlRigComponent.h"

#include "Animation/AnimBlueprint.h"
#include "ControlRigBlueprintLegacy.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Rigs/RigHierarchy.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Highlevel/Hierarchy/RigUnit_AimBone.h"
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

namespace UnrealAIControlRigSpecLocal
{
	FString SpecJson(const TSharedPtr<FJsonValue>& Value)
	{
		FString Out;
		if (Value.IsValid())
			FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		return Out;
	}

	FString SpecJson(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		if (Object.IsValid())
			FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		return Out;
	}

	/** {"lit":"..."}: a string argument of a plan. */
	FString SpecLit(const FString& Text)
	{
		return FString::Printf(TEXT(R"({"lit":"%s"})"), *Text.ReplaceCharWithEscapedChar());
	}

	/** {"lit":N}: a number argument of a plan. */
	FString SpecNumber(int32 Number)
	{
		return FString::Printf(TEXT(R"({"lit":%d})"), Number);
	}

	/**
	 * The plan Rql.Parser produces for `<source> | call <Method>(<Args>)`: the source is a named one (`controlRig`,
	 * `editor`) or, for an asset path, `unreal.asset("<path>")`.
	 */
	FString SpecCall(const FString& Source, const FString& Method, const TArray<FString>& Args)
	{
		const FString From = Source.StartsWith(TEXT("/"))
			? FString::Printf(TEXT(R"({"call":"asset","on":{"path":["unreal"]},"args":[%s]})"), *SpecLit(Source))
			: FString::Printf(TEXT(R"({"path":["%s"]})"), *Source);
		return FString::Printf(TEXT(R"({"rql":"0.1","source":%s,"stages":[{"op":"call","method":"%s","args":[%s]}]})"),
			*From, *Method, *FString::Join(Args, TEXT(",")));
	}

	URigVMNode* SpecNode(UControlRigBlueprint* Rig, const FString& Name)
	{
		return Rig && Rig->GetModel() ? Rig->GetModel()->FindNodeByName(*Name) : nullptr;
	}

	template <typename T>
	T* SpecAsset(const FString& Path)
	{
		return FindObject<T>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	}
}

BEGIN_DEFINE_SPEC(FUnrealAIControlRigSpec, "UnrealAIControlRig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIControlRigSpec)

void FUnrealAIControlRigSpec::Define()
{
	using namespace UnrealAIControlRigSpecLocal;

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
			for (const TCHAR* Text : { TEXT("| `controlRigUnits` |"), TEXT("AddLookAt("), TEXT("AddToAnimBlueprint("), TEXT("TrackNearestActor("), TEXT("AddUnit(") })
				TestTrue(*FString::Printf(TEXT("skill shows %s"), Text), Skill.Contains(Text));
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
			for (const TCHAR* Method : { TEXT("CreateRig"), TEXT("AddBone"), TEXT("AddControl"), TEXT("AddTwoBoneIK"), TEXT("AddVariable"), TEXT("AddUnit"), TEXT("AddSetVariable"), TEXT("RigValues"),
				TEXT("SetPin"), TEXT("Connect"), TEXT("RemoveNode"), TEXT("AddLookAt"), TEXT("AddToAnimBlueprint"), TEXT("SetRigInput"), TEXT("TrackNearestActor") })
				TestTrue(*FString::Printf(TEXT("lists %s"), Method), Schema.Contains(FString(Method) + TEXT("(")));
		});

		It("creates a rig at exactly the path asked for when its name ends in many digits", [this]()
		{
			// The engine's unique-name step reads trailing digits as an int32: ...F9672969617062 would become ...F0002147483647.
			const FString Path = TEXT("/Game/RqlSpec/CR_RqlSpecDigits_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8) + TEXT("F9672969617062");
			auto Run = [](const FString& Plan) { return UnrealMcpRqlTools::RunPlan(Plan, true); };

			// controlRig | call CreateRig("@RIG@")
			TestTrue(TEXT("rig created"), Run(SpecCall(TEXT("controlRig"), TEXT("CreateRig"), { SpecLit(Path) })));
			if (TestNotNull(TEXT("the rig is at the path asked for"), SpecAsset<UControlRigBlueprint>(Path)))
			{
				// editor | call DeleteAsset("@RIG@", true)
				TestTrue(TEXT("rig deleted"), Run(SpecCall(TEXT("editor"), TEXT("DeleteAsset"), { SpecLit(Path), TEXT(R"({"lit":true})") })));
				// The deleted rig's path is free again: the same call creates it there.
				TSharedPtr<FJsonObject> Again;
				const bool bAgain = UnrealMcpRqlTools::RunPlan(SpecCall(TEXT("controlRig"), TEXT("CreateRig"), { SpecLit(Path) }), true, &Again);
				TestTrue(*FString::Printf(TEXT("rig created again (%s)"), *SpecJson(Again).Left(400)), bAgain);
				if (TestNotNull(TEXT("the new rig is at the same path"), SpecAsset<UControlRigBlueprint>(Path)))
					TestTrue(TEXT("new rig deleted"), Run(SpecCall(TEXT("editor"), TEXT("DeleteAsset"), { SpecLit(Path), TEXT(R"({"lit":true})") })));
			}
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

	Describe("source: controlRigUnits", [this]()
	{
		It("lists the units a rig can run, with their pins, and resolves them by name or title", [this]()
		{
			const TArray<UObject*> Rows = UnrealAIControlRigUnits::List();
			const UUnrealAIControlRigUnit* Aim = nullptr;
			const UUnrealAIControlRigUnit* FromWorld = nullptr;
			for (const UObject* Row : Rows)
			{
				const UUnrealAIControlRigUnit* Unit = CastChecked<UUnrealAIControlRigUnit>(Row);
				Aim = Unit->Struct == TEXT("RigUnit_AimItem") ? Unit : Aim;
				FromWorld = Unit->Struct == TEXT("RigUnit_ToRigSpace_Location") ? Unit : FromWorld;
			}
			TestTrue(*FString::Printf(TEXT("a catalog (%d units)"), Rows.Num()), Rows.Num() > 100);
			if (TestNotNull(TEXT("the Aim unit"), Aim))
			{
				TestEqual(TEXT("its name"), Aim->Name, FString(TEXT("AimItem")));
				TestEqual(TEXT("its title"), Aim->DisplayName, FString(TEXT("Aim")));
				TestTrue(TEXT("it executes"), Aim->Execute);
				TestTrue(*FString::Printf(TEXT("its Primary pin lists Target (%s)"), *FString::Join(Aim->Pins, TEXT(" | "))),
					Aim->Pins.ContainsByPredicate([](const FString& Pin) { return Pin.StartsWith(TEXT("Primary: ")) && Pin.Contains(TEXT("Target Vector")); }));
			}
			if (TestNotNull(TEXT("the From World (location) unit"), FromWorld))
				TestFalse(TEXT("a pure unit"), FromWorld->Execute);
			TestTrue(TEXT("AimItem resolves"), UnrealAIControlRigUnits::Resolve(TEXT("AimItem")) == FRigUnit_AimItem::StaticStruct());
			TestTrue(TEXT("FRigUnit_AimItem resolves"), UnrealAIControlRigUnits::Resolve(TEXT("FRigUnit_AimItem")) == FRigUnit_AimItem::StaticStruct());
			TestTrue(TEXT("Basic IK resolves by title"), UnrealAIControlRigUnits::Resolve(TEXT("Basic IK")) != nullptr);

			// controlRigUnits | where Name == "AimItem" | count
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("query ok"), UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["controlRigUnits"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"AimItem"}]}},{"op":"count"}]})RQL"), false, &Result));
			TestTrue(TEXT("found by name"), Result.IsValid() && Result->GetNumberField(TEXT("value")) == 1);
		});
	});

	Describe("rig graph authoring", [this]()
	{
		It("adds variables and units with settings, links, pins and the execution order; refuses bad names before changing", [this]()
		{
			const FString Path = TEXT("/Game/RqlSpec/CR_RqlSpecGraph_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			auto Run = [](const FString& Plan, TSharedPtr<FJsonObject>* Out = nullptr) { return UnrealMcpRqlTools::RunPlan(Plan, true, Out); };
			auto Refused = [this, &Run](const TCHAR* What, const FString& Plan, const TCHAR* Expected)
			{
				TSharedPtr<FJsonObject> Result;
				TestFalse(*FString::Printf(TEXT("%s: refused"), What), Run(Plan, &Result));
				TestTrue(*FString::Printf(TEXT("%s: says '%s' (%s)"), What, Expected, *SpecJson(Result).Left(500)), SpecJson(Result).Contains(Expected));
			};

			// controlRig | call CreateRig("@RIG@")      AddBone("@RIG@", "Root", "", [0, 0, 0])      AddControl("@RIG@", "Goal", [0, 100, 150])
			TestTrue(TEXT("rig"), Run(SpecCall(TEXT("controlRig"), TEXT("CreateRig"), { SpecLit(Path) })));
			UControlRigBlueprint* Rig = SpecAsset<UControlRigBlueprint>(Path);
			if (!TestNotNull(TEXT("the rig asset"), Rig))
				return;
			TestTrue(TEXT("bone"), Run(SpecCall(TEXT("controlRig"), TEXT("AddBone"), { SpecLit(Path), SpecLit(TEXT("Root")), SpecLit(TEXT("")),
				FString::Printf(TEXT(R"({"list":[%s,%s,%s]})"), *SpecNumber(0), *SpecNumber(0), *SpecNumber(0)) })));
			TestTrue(TEXT("control"), Run(SpecCall(TEXT("controlRig"), TEXT("AddControl"), { SpecLit(Path), SpecLit(TEXT("Goal")),
				FString::Printf(TEXT(R"({"list":[%s,%s,%s]})"), *SpecNumber(0), *SpecNumber(100), *SpecNumber(150)) })));

			// controlRig | call AddVariable("@RIG@", "Target", "Vector", "(0, 50, 100)")      AddVariable("@RIG@", "Amount", "float", "0.5")
			TestTrue(TEXT("vector variable"), Run(SpecCall(TEXT("controlRig"), TEXT("AddVariable"), { SpecLit(Path), SpecLit(TEXT("Target")), SpecLit(TEXT("Vector")), SpecLit(TEXT("(0, 50, 100)")) })));
			TestTrue(TEXT("float variable"), Run(SpecCall(TEXT("controlRig"), TEXT("AddVariable"), { SpecLit(Path), SpecLit(TEXT("Amount")), SpecLit(TEXT("float")), SpecLit(TEXT("0.5")) })));
			const FString Variables = SpecJson(UnrealAIControlRigGraph::VariablesJson(Rig));
			TestTrue(*FString::Printf(TEXT("Variables lists both, public (%s)"), *Variables),
				Variables.Contains(TEXT("\"Name\":\"Target\"")) && Variables.Contains(TEXT("\"Name\":\"Amount\"")) && Variables.Contains(TEXT("\"Public\":true")));
			Refused(TEXT("a taken variable"), SpecCall(TEXT("controlRig"), TEXT("AddVariable"), { SpecLit(Path), SpecLit(TEXT("amount")), SpecLit(TEXT("float")) }), TEXT("already has a variable Amount"));
			Refused(TEXT("an unknown type"), SpecCall(TEXT("controlRig"), TEXT("AddVariable"), { SpecLit(Path), SpecLit(TEXT("Odd")), SpecLit(TEXT("Banana")) }), TEXT("is not a rig variable type"));

			// controlRig | call AddUnit("@RIG@", "RigUnit_ToRigSpace_Location", "Value=@Target", "", "ToRig")      a pure unit
			TestTrue(TEXT("pure unit"), Run(SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("RigUnit_ToRigSpace_Location")), SpecLit(TEXT("Value=@Target")), SpecLit(TEXT("")), SpecLit(TEXT("ToRig")) })));
			// controlRig | call AddUnit("@RIG@", "Aim", "Item=Root, Primary.Axis=(0, 1, 0), Primary.Target=@ToRig.Global, Weight=@Amount", "", "AimRoot")
			TSharedPtr<FJsonObject> Added;
			TestTrue(TEXT("executing unit"), Run(SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("Aim")),
				SpecLit(TEXT("Item=Root, Primary.Axis=(0, 1, 0), Primary.Target=@ToRig.Global, Weight=@Amount")), SpecLit(TEXT("")), SpecLit(TEXT("AimRoot")) }), &Added));
			TestTrue(TEXT("returns the node name"), SpecJson(Added).Contains(TEXT("AimRoot")));
			if (URigVMNode* Aim = SpecNode(Rig, TEXT("AimRoot")))
			{
				TestTrue(TEXT("Item is the Root bone"), Aim->FindPin(TEXT("Item"))->GetDefaultValue().Contains(TEXT("Root")));
				TestTrue(*FString::Printf(TEXT("Primary.Axis is +Y (%s)"), *Aim->FindPin(TEXT("Primary.Axis"))->GetDefaultValue()), Aim->FindPin(TEXT("Primary.Axis"))->GetDefaultValue().Contains(TEXT("Y=1")));
				const TArray<URigVMPin*> TargetSources = Aim->FindPin(TEXT("Primary.Target"))->GetLinkedSourcePins();
				TestTrue(TEXT("Primary.Target from ToRig.Global"), TargetSources.Num() == 1 && TargetSources[0]->GetPinPath() == TEXT("ToRig.Global"));
				const TArray<URigVMPin*> WeightSources = Aim->FindPin(TEXT("Weight"))->GetLinkedSourcePins();
				const URigVMVariableNode* WeightVariable = WeightSources.Num() == 1 ? Cast<URigVMVariableNode>(WeightSources[0]->GetNode()) : nullptr;
				TestTrue(TEXT("Weight read from the Amount variable"), WeightVariable && WeightVariable->GetVariableName() == TEXT("Amount"));
				TestEqual(TEXT("runs on Forwards Solve"), UnrealAIControlRigGraph::ExecuteInput(Aim)->GetLinkedSourcePins().Num(), 1);
			}
			else
			{
				AddError(TEXT("no AimRoot node"));
			}

			// controlRig | call AddUnit("@RIG@", "AimItem", "Item=Root", "", "Second")      AddUnit("@RIG@", "AimItem", "Item=Control:Goal", "Forwards Solve", "First")
			TestTrue(TEXT("second at the end"), Run(SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("AimItem")), SpecLit(TEXT("Item=Root")), SpecLit(TEXT("")), SpecLit(TEXT("Second")) })));
			TestTrue(TEXT("first inserted"), Run(SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("AimItem")), SpecLit(TEXT("Item=Control:Goal")), SpecLit(TEXT("Forwards Solve")), SpecLit(TEXT("First")) })));
			const FString Nodes = SpecJson(UnrealAIControlRigGraph::NodesJson(Rig));
			TestTrue(*FString::Printf(TEXT("Forwards Solve runs First, then AimRoot, then Second (%s)"), *Nodes.Left(2000)),
				Nodes.Contains(TEXT("\"Next\":[\"First\"]")) && Nodes.Contains(TEXT("\"Next\":[\"AimRoot\"]")) && Nodes.Contains(TEXT("\"Next\":[\"Second\"]")));
			TestTrue(TEXT("Nodes shows links as @Node.Pin"), Nodes.Contains(TEXT("\"Primary.Target\":\"@ToRig.Global\"")));

			// controlRig | call SetPin("@RIG@", "AimRoot.Primary.Kind", "Direction")      Connect("@RIG@", "ToRig.Global", "Second.Primary.Target")
			TSharedPtr<FJsonObject> Set;
			TestTrue(TEXT("pin set"), Run(SpecCall(TEXT("controlRig"), TEXT("SetPin"), { SpecLit(Path), SpecLit(TEXT("AimRoot.Primary.Kind")), SpecLit(TEXT("Direction")) }), &Set));
			TestTrue(*FString::Printf(TEXT("Kind is Direction (%s)"), *SpecJson(Set)), SpecJson(Set).Contains(TEXT("Direction")));
			TestTrue(TEXT("connected"), Run(SpecCall(TEXT("controlRig"), TEXT("Connect"), { SpecLit(Path), SpecLit(TEXT("ToRig.Global")), SpecLit(TEXT("Second.Primary.Target")) })));
			if (URigVMNode* Second = SpecNode(Rig, TEXT("Second")))
				TestEqual(TEXT("Second aims at ToRig"), Second->FindPin(TEXT("Primary.Target"))->GetLinkedSourcePins().Num(), 1);

			const int32 NodeCount = Rig->GetModel()->GetNodes().Num();
			Refused(TEXT("an unknown unit"), SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("Aimm")) }), TEXT("no rig unit named 'Aimm'"));
			Refused(TEXT("an unknown pin"), SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("AimItem")), SpecLit(TEXT("Itemz=Root")) }), TEXT("has no pin 'Itemz'"));
			Refused(TEXT("an unknown variable"), SpecCall(TEXT("controlRig"), TEXT("SetPin"), { SpecLit(Path), SpecLit(TEXT("AimRoot.Weight")), SpecLit(TEXT("@Nope")) }), TEXT("has no variable 'Nope'"));
			Refused(TEXT("an unknown element"), SpecCall(TEXT("controlRig"), TEXT("SetPin"), { SpecLit(Path), SpecLit(TEXT("AimRoot.Item")), SpecLit(TEXT("Wrist")) }), TEXT("no element named 'Wrist'"));
			Refused(TEXT("an output pin"), SpecCall(TEXT("controlRig"), TEXT("SetPin"), { SpecLit(Path), SpecLit(TEXT("ToRig.Global")), SpecLit(TEXT("(1, 2, 3)")) }), TEXT("is an output"));
			Refused(TEXT("After on a pure unit"), SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("RigUnit_ToRigSpace_Location")), SpecLit(TEXT("")), SpecLit(TEXT("AimRoot")) }), TEXT("is a pure value node"));
			Refused(TEXT("a value that does not fit"), SpecCall(TEXT("controlRig"), TEXT("SetPin"), { SpecLit(Path), SpecLit(TEXT("AimRoot.Weight")), SpecLit(TEXT("abc")) }), TEXT("is not a value of"));
			if (URigVMNode* Aim = SpecNode(Rig, TEXT("AimRoot")))
				TestEqual(TEXT("the refused value kept the Weight link"), Aim->FindPin(TEXT("Weight"))->GetLinkedSourcePins().Num(), 1);
			TestEqual(TEXT("the refused calls left no node"), Rig->GetModel()->GetNodes().Num(), NodeCount);

			// controlRig | call RemoveNode("@RIG@", "AimRoot")      First now runs Second
			TestTrue(TEXT("removed"), Run(SpecCall(TEXT("controlRig"), TEXT("RemoveNode"), { SpecLit(Path), SpecLit(TEXT("AimRoot")) })));
			TestNull(TEXT("no AimRoot"), SpecNode(Rig, TEXT("AimRoot")));
			const FString After = SpecJson(UnrealAIControlRigGraph::NodesJson(Rig));
			TestTrue(*FString::Printf(TEXT("the chain is reconnected (%s)"), *After.Left(1200)), After.Contains(TEXT("\"Next\":[\"Second\"]")));

			// unreal.asset("@RIG@") | call Compile()      editor | call DeleteAsset("@RIG@", true)
			TSharedPtr<FJsonObject> Compiled;
			TestTrue(TEXT("compile ran"), Run(SpecCall(Path, TEXT("Compile"), {}), &Compiled));
			TestTrue(*FString::Printf(TEXT("compiles (%s)"), *SpecJson(Compiled).Left(600)), SpecJson(Compiled).Contains(TEXT("Succeeded\":true")));
			TestTrue(TEXT("test rig deleted"), Run(SpecCall(TEXT("editor"), TEXT("DeleteAsset"), { SpecLit(Path), TEXT(R"({"lit":true})") })));
		});
	});

	Describe("reading a running rig", [this]()
	{
		It("stores any pin in a variable from the execution chain; refuses an unknown variable and a curve the rig lacks", [this]()
		{
			const FString Path = TEXT("/Game/RqlSpec/CR_RqlSpecSetVar_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			auto Run = [](const FString& Plan, TSharedPtr<FJsonObject>* Out = nullptr) { return UnrealMcpRqlTools::RunPlan(Plan, true, Out); };

			// controlRig | call CreateRig("@RIG@")      AddVariable("@RIG@", "HitZ", "float", "", false)
			TestTrue(TEXT("rig"), Run(SpecCall(TEXT("controlRig"), TEXT("CreateRig"), { SpecLit(Path) })));
			UControlRigBlueprint* Rig = SpecAsset<UControlRigBlueprint>(Path);
			if (!TestNotNull(TEXT("the rig asset"), Rig))
				return;
			TestTrue(TEXT("variable"), Run(SpecCall(TEXT("controlRig"), TEXT("AddVariable"), { SpecLit(Path), SpecLit(TEXT("HitZ")), SpecLit(TEXT("float")), SpecLit(TEXT("")), TEXT(R"({"lit":false})") })));
			// controlRig | call AddUnit("@RIG@", "SphereTraceByTraceChannel", "Start=(0, 0, 30), End=(0, 0, -50), Radius=5", "", "Trace")
			TestTrue(TEXT("trace"), Run(SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("SphereTraceByTraceChannel")), SpecLit(TEXT("Start=(0, 0, 30), End=(0, 0, -50), Radius=5")), SpecLit(TEXT("")), SpecLit(TEXT("Trace")) })));

			// controlRig | call AddSetVariable("@RIG@", "HitZ", "@Trace.HitLocation.Z", "", "StoreHit")
			TestTrue(TEXT("setter"), Run(SpecCall(TEXT("controlRig"), TEXT("AddSetVariable"), { SpecLit(Path), SpecLit(TEXT("HitZ")), SpecLit(TEXT("@Trace.HitLocation.Z")), SpecLit(TEXT("")), SpecLit(TEXT("StoreHit")) })));
			const URigVMVariableNode* Setter = Cast<URigVMVariableNode>(SpecNode(Rig, TEXT("StoreHit")));
			if (TestNotNull(TEXT("the setter node"), Setter))
			{
				TestFalse(TEXT("it sets, not gets"), Setter->IsGetter());
				const URigVMPin* Value = Setter->FindPin(TEXT("Value"));
				TestTrue(TEXT("its value comes from the trace"), Value && Value->GetLinkedSourcePins().Num() == 1);
				URigVMNode* Solve = UnrealAIControlRigGraph::FindForwardsSolve(Rig->GetModel());
				const URigVMPin* End = Solve ? UnrealAIControlRigGraph::EndOfChain(Solve) : nullptr;
				TestTrue(TEXT("it ends the execution chain"), End && End->GetNode() == Setter);
			}

			// controlRig | call AddSetVariable("@RIG@", "Nope", "1")
			TSharedPtr<FJsonObject> NoVariable;
			TestFalse(TEXT("an unknown variable"), Run(SpecCall(TEXT("controlRig"), TEXT("AddSetVariable"), { SpecLit(Path), SpecLit(TEXT("Nope")), SpecLit(TEXT("1")) }), &NoVariable));
			TestTrue(*FString::Printf(TEXT("names the variables (%s)"), *SpecJson(NoVariable).Left(400)), SpecJson(NoVariable).Contains(TEXT("variables: HitZ")));

			// controlRig | call AddSetVariable("@RIG@", "HitZ", "@Nope.Pin", "", "StoreBad")      leaves no setter behind
			TestFalse(TEXT("an unknown source"), Run(SpecCall(TEXT("controlRig"), TEXT("AddSetVariable"), { SpecLit(Path), SpecLit(TEXT("HitZ")), SpecLit(TEXT("@Nope.Pin")), SpecLit(TEXT("")), SpecLit(TEXT("StoreBad")) })));
			TestNull(TEXT("no setter left"), SpecNode(Rig, TEXT("StoreBad")));

			// controlRig | call AddUnit("@RIG@", "SetCurveValue", "Curve=DebugHit, Value=1", "", "SetDebug")      the engine would skip it in silence
			TSharedPtr<FJsonObject> NoCurve;
			TestFalse(TEXT("a curve the rig lacks"), Run(SpecCall(TEXT("controlRig"), TEXT("AddUnit"), { SpecLit(Path), SpecLit(TEXT("SetCurveValue")), SpecLit(TEXT("Curve=DebugHit, Value=1")), SpecLit(TEXT("")), SpecLit(TEXT("SetDebug")) }), &NoCurve));
			TestTrue(*FString::Printf(TEXT("says what works (%s)"), *SpecJson(NoCurve).Left(500)), SpecJson(NoCurve).Contains(TEXT("no curve 'DebugHit'")) && SpecJson(NoCurve).Contains(TEXT("AddSetVariable")));
			TestNull(TEXT("no curve node left"), SpecNode(Rig, TEXT("SetDebug")));

			TestTrue(TEXT("rig deleted"), Run(SpecCall(TEXT("editor"), TEXT("DeleteAsset"), { SpecLit(Path), TEXT(R"({"lit":true})") })));
		});

		It("reads rigs only while playing, and says how to start", [this]()
		{
			// controlRig | call RigValues()
			TSharedPtr<FJsonObject> Result;
			TestFalse(TEXT("refused without a play session"), UnrealMcpRqlTools::RunPlan(SpecCall(TEXT("controlRig"), TEXT("RigValues"), {}), true, &Result));
			TestTrue(*FString::Printf(TEXT("names StartPlay (%s)"), *SpecJson(Result).Left(400)), SpecJson(Result).Contains(TEXT("no Play In Editor session")) && SpecJson(Result).Contains(TEXT("StartPlay")));
		});
	});

	Describe("Anim Blueprint integration", [this]()
	{
		It("builds a head look-at rig from a mesh and runs it in an Anim Blueprint, its target set from the nearest actor", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			const FString RigPath = TEXT("/Game/RqlSpec/CR_RqlSpecLookAt_") + Id;
			const FString AbpPath = TEXT("/Game/RqlSpec/ABP_RqlSpecLookAt_") + Id;
			auto Run = [](const FString& Plan, TSharedPtr<FJsonObject>* Out = nullptr) { return UnrealMcpRqlTools::RunPlan(Plan, true, Out); };

			// controlRig | call CreateRig("@RIG@", "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP")      the engine's UE4 mannequin
			TestTrue(TEXT("rig from the mesh"), Run(SpecCall(TEXT("controlRig"), TEXT("CreateRig"), { SpecLit(RigPath), SpecLit(TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP")) })));
			UControlRigBlueprint* Rig = SpecAsset<UControlRigBlueprint>(RigPath);
			if (!TestNotNull(TEXT("the rig"), Rig))
				return;
			TestTrue(TEXT("the mesh's head bone imported"), Rig->GetHierarchy()->Contains(FRigElementKey(TEXT("head"), ERigElementType::Bone)));
			// controlRig | call AddVariable("@RIG@", "LookAtTarget", "Vector")      AddVariable("@RIG@", "LookAtWeight", "float", "0")
			TestTrue(TEXT("target variable"), Run(SpecCall(TEXT("controlRig"), TEXT("AddVariable"), { SpecLit(RigPath), SpecLit(TEXT("LookAtTarget")), SpecLit(TEXT("Vector")) })));
			TestTrue(TEXT("weight variable"), Run(SpecCall(TEXT("controlRig"), TEXT("AddVariable"), { SpecLit(RigPath), SpecLit(TEXT("LookAtWeight")), SpecLit(TEXT("float")), SpecLit(TEXT("0")) })));
			// controlRig | call AddLookAt("@RIG@", "head", "@LookAtTarget", "@LookAtWeight")
			TSharedPtr<FJsonObject> LookAt;
			TestTrue(TEXT("look-at"), Run(SpecCall(TEXT("controlRig"), TEXT("AddLookAt"), { SpecLit(RigPath), SpecLit(TEXT("head")), SpecLit(TEXT("@LookAtTarget")), SpecLit(TEXT("@LookAtWeight")) }), &LookAt));
			TestTrue(*FString::Printf(TEXT("returns LookAt_head (%s)"), *SpecJson(LookAt)), SpecJson(LookAt).Contains(TEXT("LookAt_head")));
			if (URigVMNode* Aim = SpecNode(Rig, TEXT("LookAt_head")))
			{
				const TArray<URigVMPin*> Target = Aim->FindPin(TEXT("Primary.Target"))->GetLinkedSourcePins();
				TestTrue(TEXT("aims at the target in rig space"), Target.Num() == 1 && Target[0]->GetPinPath() == TEXT("LookAt_head_Target.Global"));
				TestTrue(TEXT("Item is head"), Aim->FindPin(TEXT("Item"))->GetDefaultValue().Contains(TEXT("head")));
				TestEqual(TEXT("Secondary keeps it upright"), Aim->FindPin(TEXT("Secondary.Kind"))->GetDefaultValue(), FString(TEXT("Direction")));
			}
			else
			{
				AddError(TEXT("no LookAt_head node"));
			}
			// unreal.asset("@RIG@") | call Compile()
			TSharedPtr<FJsonObject> RigCompiled;
			TestTrue(TEXT("rig compile ran"), Run(SpecCall(RigPath, TEXT("Compile"), {}), &RigCompiled));
			TestTrue(*FString::Printf(TEXT("rig compiles (%s)"), *SpecJson(RigCompiled).Left(600)), SpecJson(RigCompiled).Contains(TEXT("Succeeded\":true")));

			// editor | call CopyAsset("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_AnimBlueprint", "@ABP@")
			TestTrue(TEXT("anim blueprint copied"), Run(SpecCall(TEXT("editor"), TEXT("CopyAsset"), { SpecLit(TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_AnimBlueprint")), SpecLit(AbpPath) })));
			UAnimBlueprint* Abp = SpecAsset<UAnimBlueprint>(AbpPath);
			if (!TestNotNull(TEXT("the Anim Blueprint"), Abp))
				return;
			// controlRig | call AddToAnimBlueprint("@ABP@", "@RIG@")
			TSharedPtr<FJsonObject> Wired;
			TestTrue(TEXT("rig added to the AnimGraph"), Run(SpecCall(TEXT("controlRig"), TEXT("AddToAnimBlueprint"), { SpecLit(AbpPath), SpecLit(RigPath) }), &Wired));
			TestTrue(*FString::Printf(TEXT("says what it bound (%s)"), *SpecJson(Wired).Left(600)), SpecJson(Wired).Contains(TEXT("LookAtTarget <- LookAtTarget")));
			const FString Rigs = SpecJson(UnrealAIControlRigAnim::ControlRigsJson(Abp));
			TestTrue(*FString::Printf(TEXT("ControlRigs: before Output Pose, inputs bound (%s)"), *Rigs),
				Rigs.Contains(TEXT("\"Before\":\"Output Pose\"")) && Rigs.Contains(TEXT("\"LookAtTarget\":\"@LookAtTarget\"")) && Rigs.Contains(TEXT("\"LookAtWeight\":\"@LookAtWeight\"")));
			TestTrue(TEXT("the Anim Blueprint has the bound variables"), FBlueprintEditorUtils::FindNewVariableIndex(Abp, TEXT("LookAtTarget")) != INDEX_NONE);
			// controlRig | call SetRigInput("@ABP@", "", "LookAtWeight", "0.5")      then SetRigInput("@ABP@", "<rig name>", "LookAtWeight", "@LookAtWeight")
			TSharedPtr<FJsonObject> Constant;
			TestTrue(TEXT("constant input"), Run(SpecCall(TEXT("controlRig"), TEXT("SetRigInput"), { SpecLit(AbpPath), SpecLit(TEXT("")), SpecLit(TEXT("LookAtWeight")), SpecLit(TEXT("0.5")) }), &Constant));
			TestTrue(*FString::Printf(TEXT("LookAtWeight = 0.5 (%s)"), *SpecJson(Constant)), SpecJson(Constant).Contains(TEXT("LookAtWeight = 0.5")));
			TestTrue(TEXT("bound again, by rig name"), Run(SpecCall(TEXT("controlRig"), TEXT("SetRigInput"), { SpecLit(AbpPath), SpecLit(FPackageName::GetShortName(RigPath)), SpecLit(TEXT("LookAtWeight")), SpecLit(TEXT("@LookAtWeight")) })));

			// controlRig | call TrackNearestActor("@ABP@", "StaticMeshActor", "LookAtTarget", "LookAtWeight", 600, 4)
			TSharedPtr<FJsonObject> Tracked;
			TestTrue(TEXT("tracking added"), Run(SpecCall(TEXT("controlRig"), TEXT("TrackNearestActor"), { SpecLit(AbpPath), SpecLit(TEXT("StaticMeshActor")), SpecLit(TEXT("LookAtTarget")), SpecLit(TEXT("LookAtWeight")), SpecNumber(600), SpecNumber(4) }), &Tracked));
			TestTrue(*FString::Printf(TEXT("says what it added (%s)"), *SpecJson(Tracked).Left(600)), SpecJson(Tracked).Contains(TEXT("follows the nearest StaticMeshActor within 600 cm")));
			TSharedPtr<FJsonObject> Again;
			TestFalse(TEXT("a second tracking of the same variable is refused"), Run(SpecCall(TEXT("controlRig"), TEXT("TrackNearestActor"), { SpecLit(AbpPath), SpecLit(TEXT("StaticMeshActor")) }), &Again));
			TestTrue(*FString::Printf(TEXT("saying it already sets it (%s)"), *SpecJson(Again).Left(400)), SpecJson(Again).Contains(TEXT("already sets LookAtTarget")));

			// unreal.asset("@ABP@") | call Compile()
			TSharedPtr<FJsonObject> Compiled;
			TestTrue(TEXT("anim blueprint compile ran"), Run(SpecCall(AbpPath, TEXT("Compile"), {}), &Compiled));
			TestTrue(*FString::Printf(TEXT("the Anim Blueprint compiles (%s)"), *SpecJson(Compiled).Left(1500)), SpecJson(Compiled).Contains(TEXT("Succeeded\":true")));

			// controlRig | call SetRigInput("@ABP@", "CR_Nope", "LookAtWeight", "1")      an unknown rig node names the ones there are
			TSharedPtr<FJsonObject> NoNode;
			TestFalse(TEXT("an unknown rig node"), Run(SpecCall(TEXT("controlRig"), TEXT("SetRigInput"), { SpecLit(AbpPath), SpecLit(TEXT("CR_Nope")), SpecLit(TEXT("LookAtWeight")), SpecLit(TEXT("1")) }), &NoNode));
			TestTrue(*FString::Printf(TEXT("naming the rig nodes (%s)"), *SpecJson(NoNode).Left(400)), SpecJson(NoNode).Contains(TEXT("Control Rig nodes:")));

			// editor | call DeleteAsset("@ABP@", true)      DeleteAsset("@RIG@", true): the test leaves no unsaved asset behind
			TestTrue(TEXT("anim blueprint deleted"), Run(SpecCall(TEXT("editor"), TEXT("DeleteAsset"), { SpecLit(AbpPath), TEXT(R"({"lit":true})") })));
			TestTrue(TEXT("rig deleted"), Run(SpecCall(TEXT("editor"), TEXT("DeleteAsset"), { SpecLit(RigPath), TEXT(R"({"lit":true})") })));
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
			TestTrue(TEXT("lists name:Nodes"), Schema.Contains(TEXT("\"name\":\"Nodes\"")));
			TestTrue(TEXT("lists name:Variables"), Schema.Contains(TEXT("\"name\":\"Variables\"")));
			bool bAnimOk = false;
			const FString AnimSchema = UnrealMcpRqlTools::Schema(TEXT("AnimBlueprint"), bAnimOk);
			TestTrue(TEXT("AnimBlueprint lists name:ControlRigs"), bAnimOk && AnimSchema.Contains(TEXT("\"name\":\"ControlRigs\"")));
		});

		It("describes ControlRigComponent with its BoundRig member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("ControlRigComponent"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:BoundRig"), Schema.Contains(TEXT("\"name\":\"BoundRig\"")));
		});

		It("matches a placed ControlRigComponent with `where $ is ControlRigComponent`", [this]()
		{
			// The product path of the extension's e2e count (`actors | get Components | where $ is ControlRigComponent | count`): with
			// a ControlRigComponent in the level the filter matches it -- an empty level answers 0 with RQL's no-match warning.
			if (!TestNotNull(TEXT("GEngine"), GEngine))
				return;
			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::Editor)
				{
					World = Context.World();
					break;
				}
			}
			if (!TestNotNull(TEXT("the editor world"), World))
				return;
			AActor* Host = World->SpawnActor<AActor>();
			if (!TestNotNull(TEXT("a host actor"), Host))
				return;
			const FString Label = TEXT("RqlSpecControlRigComponentHost_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Host->SetActorLabel(Label);
			UControlRigComponent* Component = NewObject<UControlRigComponent>(Host, TEXT("RqlSpecControlRigComponent"));
			if (USceneComponent* Scene = Cast<USceneComponent>(Component))
				Host->SetRootComponent(Scene);
			Host->AddInstanceComponent(Component);
			Component->RegisterComponent();

			// actors | where Label == "@LABEL@" | get Components | where $ is ControlRigComponent | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(FString(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"@LABEL@"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"ControlRigComponent"}]}},{"op":"count"}]})RQL")).Replace(TEXT("@LABEL@"), *Label), false, &Result);
			FString Text;
			if (Result.IsValid())
				FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
			TestTrue(*FString::Printf(TEXT("succeeds (%s)"), *Text.Left(400)), bResultOk);
			TestEqual(*FString::Printf(TEXT("counts the placed component (%s)"), *Text.Left(400)),
				Result.IsValid() ? static_cast<int32>(Result->GetNumberField(TEXT("value"))) : -1, 1);
			TestFalse(*FString::Printf(TEXT("no no-match warning (%s)"), *Text.Left(400)), Text.Contains(TEXT("no-match")));
			World->DestroyActor(Host);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
