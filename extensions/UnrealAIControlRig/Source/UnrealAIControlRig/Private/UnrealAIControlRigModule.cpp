// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIControlRigAnim.h"
#include "UnrealAIControlRigGraph.h"
#include "UnrealAIControlRigRqlApi.h"
#include "UnrealAIControlRigUnits.h"

#include "Animation/AnimBlueprint.h"
#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"          // UControlRigBlueprint (ControlRigDeveloper) — authoring asset + GetHierarchy()
#include "ControlRigComponent.h"                 // UControlRigComponent (ControlRig)
#include "Rigs/RigHierarchy.h"                   // URigHierarchy + element-key getters
#include "Rigs/RigHierarchyDefines.h"            // FRigElementKey / ERigElementType / ERigControlType
#include "Rigs/RigHierarchyElements.h"           // FRigBaseElement / FRigControlElement / FRigControlSettings

namespace UnrealAIControlRigRqlLocal
{
	template <typename TEnum>
	FString EnumName(TEnum Value)
	{
		const UEnum* Enum = StaticEnum<TEnum>();
		return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Value)) : FString(TEXT("Unknown"));
	}

	URigHierarchy* HierarchyOf(UObject* Object)
	{
		UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Object);
		return Blueprint ? Blueprint->GetHierarchy() : nullptr;
	}
}

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Control Rig is queried and authored with rql-query /
 * rql-mutate instead of dedicated tools. Reflection exposes the Control Rig blueprint and component properties; the module
 * adds the `controlRigs` asset source, the `controlRigUnits` catalog, a rig blueprint's element hierarchy, graph nodes and
 * variables (not reflected properties), an Anim Blueprint's Control Rig nodes, a component's bound rig, the `controlRig`
 * operations (UUnrealAIControlRigRqlApi: rig authoring and Anim Blueprint wiring) and the agent guide rendered into the
 * unreal-rql skill.
 *
 * The version is the .uplugin VersionName.
 */
class FUnrealAIControlRigRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("ControlRig"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-control-rig"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAIControlRig")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		using namespace UnrealAIControlRigRqlLocal;
		Context.AddAssetSource(TEXT("controlRigs"), TEXT("Control Rig blueprint assets (ControlRigBlueprint) of the project, at most 200"),
			UControlRigBlueprint::StaticClass());
		Context.AddObjectListSource(TEXT("controlRigUnits"),
			TEXT("the rig units a Control Rig can run (Name, Struct, DisplayName, Category, Keywords, Description, Execute, Pins): Aim, Basic IK, Full Body IK, traces, springs, math; add one with AddUnit"),
			UUnrealAIControlRigUnit::StaticClass(), []() { return UnrealAIControlRigUnits::List(); });
		Context.AddApiSource(TEXT("controlRig"),
			TEXT("Control Rig authoring and Anim Blueprint wiring (look-at, aim, IK, traces, springs without C++): call CreateRig / AddVariable / AddUnit / SetPin / Connect / RemoveNode / AddLookAt / AddTwoBoneIK / AddBone / AddControl / AddToAnimBlueprint / SetRigInput / TrackNearestActor"),
			UUnrealAIControlRigRqlApi::StaticClass(), TEXT("ControlRigOperations"));
		Context.AddJsonMember(UControlRigBlueprint::StaticClass(), TEXT("Nodes"),
			TEXT("the rig graph's nodes: Name, Title, Unit, Next (the nodes its execution runs next), Inputs (pin: value, or @Node.Pin / @Variable for a link)"),
			[](UObject* Object) { return UnrealAIControlRigGraph::NodesJson(Object); });
		Context.AddJsonMember(UControlRigBlueprint::StaticClass(), TEXT("Variables"),
			TEXT("the rig's variables: Name, Type, Public (an input of the rig's Control Rig node in Anim Blueprints), Default"),
			[](UObject* Object) { return UnrealAIControlRigGraph::VariablesJson(Object); });
		Context.AddJsonMember(UAnimBlueprint::StaticClass(), TEXT("ControlRigs"),
			TEXT("the Control Rig nodes of the Anim Blueprint: Node, Rig, Graph, After / Before (the pose nodes around it), Inputs (pin: value, or @Variable when bound)"),
			[](UObject* Object) { return UnrealAIControlRigAnim::ControlRigsJson(Object); });
		Context.AddJsonMember(UControlRigBlueprint::StaticClass(), TEXT("Elements"),
			TEXT("rig element counts: Bones, Nulls, Controls, Curves"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				if (URigHierarchy* Hierarchy = HierarchyOf(Object))
				{
					Out->SetNumberField(TEXT("Bones"), Hierarchy->Num(ERigElementType::Bone));
					Out->SetNumberField(TEXT("Nulls"), Hierarchy->Num(ERigElementType::Null));
					Out->SetNumberField(TEXT("Controls"), Hierarchy->Num(ERigElementType::Control));
					Out->SetNumberField(TEXT("Curves"), Hierarchy->Num(ERigElementType::Curve));
				}
				return MakeShared<FJsonValueObject>(Out);
			});
		Context.AddJsonMember(UControlRigBlueprint::StaticClass(), TEXT("Hierarchy"),
			TEXT("every rig element in hierarchy order: Name, Type"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Elements;
				if (URigHierarchy* Hierarchy = HierarchyOf(Object))
				{
					for (const FRigElementKey& Key : Hierarchy->GetAllKeys(/*bTraverse=*/true))
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Name"), Key.Name.ToString());
						Entry->SetStringField(TEXT("Type"), EnumName(Key.Type));
						Elements.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				return MakeShared<FJsonValueArray>(Elements);
			});
		Context.AddJsonMember(UControlRigBlueprint::StaticClass(), TEXT("Controls"),
			TEXT("the rig's controls: Name, ControlType (Float, Transform, ...)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Controls;
				if (URigHierarchy* Hierarchy = HierarchyOf(Object))
				{
					for (const FRigControlElement* Control : Hierarchy->GetControls())
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Name"), Control->GetName());
						Entry->SetStringField(TEXT("ControlType"), EnumName(Control->Settings.ControlType));
						Controls.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				return MakeShared<FJsonValueArray>(Controls);
			});
		Context.AddJsonMember(UControlRigBlueprint::StaticClass(), TEXT("Bones"),
			TEXT("the rig's bones: Name, Parent (empty for a root)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Bones;
				if (URigHierarchy* Hierarchy = HierarchyOf(Object))
				{
					for (const FRigBoneElement* Bone : Hierarchy->GetBones())
					{
						const FRigElementKey Parent = Hierarchy->GetFirstParent(Bone->GetKey());
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Name"), Bone->GetName());
						Entry->SetStringField(TEXT("Parent"), Parent.IsValid() ? Parent.Name.ToString() : FString());
						Bones.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				return MakeShared<FJsonValueArray>(Bones);
			});
		// The rig the component already runs, read from its reflected (transient) property: GetControlRig() would
		// instantiate one during a read.
		Context.AddObjectMember(UControlRigComponent::StaticClass(), TEXT("BoundRig"),
			TEXT("the Control Rig the component runs (null when none is instanced)"), UControlRig::StaticClass(),
			[](UObject* Object) -> UObject*
			{
				static const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(UControlRigComponent::StaticClass(), TEXT("ControlRig"));
				return Property && Object ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
			});
		Context.AddGuide(TEXT("Control Rig"), TEXT(R"MD(Procedural animation (a head or eyes that look at something, aim, foot IK on stairs and ramps, reaching, springy
motion) is a Control Rig: a rig graph that runs in the character's Anim Blueprint after its animation and moves bones.
Author rigs and wire them into the Anim Blueprint with the `controlRig` operations (rql-mutate): no C++ components, no
pose copying, no Python. Rigs are assets (`controlRigs`) with `Nodes`, `Variables`, `Bones`, `Controls`; an Anim
Blueprint lists its rigs in `ControlRigs`; `controlRigUnits` lists every node a rig can run.

Look first: the character's Anim Blueprint (the mesh component's `AnimClass`) may already run a rig (the UE5 Third Person
template runs CR_Mannequin_FootIK, foot IK with pelvis offset, before Output Pose):

```
blueprints | where $type == "AnimBlueprint" | select Name Path ControlRigs
unreal.asset("/Game/Characters/Mannequins/Rigs/CR_Mannequin_FootIK") | select Variables Nodes
```

Head looks at the nearest orb, turning smoothly (a new rig after the existing ones, its inputs set every frame):

```
controlRig | call CreateRig("/Game/Rigs/CR_LookAt", "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple")   bones of the character's mesh
controlRig | call AddVariable("/Game/Rigs/CR_LookAt", "LookAtTarget", "Vector")      public: an input of the rig in Anim Blueprints
controlRig | call AddVariable("/Game/Rigs/CR_LookAt", "LookAtWeight", "float", "0")
controlRig | call AddLookAt("/Game/Rigs/CR_LookAt", "head", "@LookAtTarget", "@LookAtWeight")   nodes LookAt_head_Target (world -> rig space), LookAt_head (Aim)
controlRig | call AddToAnimBlueprint("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed", "/Game/Rigs/CR_LookAt")
controlRig | call TrackNearestActor("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed", "BP_Orb", "LookAtTarget", "LookAtWeight", 600, 4)
unreal.asset("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed") | call Compile()      {Succeeded, Errors, Messages}
editor | call SaveAsset("/Game/Rigs/CR_LookAt")
editor | call SaveAsset("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed")
```

- `AddToAnimBlueprint(abp, rig, after = "", bindVariables = true)` puts a Control Rig node right before Output Pose
  (after every pose node, so after a foot IK rig), or after the node `after` (node name, title or rig name); each public
  rig variable becomes an input bound to an Anim Blueprint variable of the same name (added when missing).
  `SetRigInput(abp, node, input, "@Var" | "0.5")` rebinds one or sets a constant.
- `TrackNearestActor(abp, actorClass, locationVar, weightVar = "", radius = 1000, speed = 5)` adds, on Event Blueprint
  Update Animation, the nearest actor of the class within radius (cm) of the owner: its location (smoothed at speed,
  0 = snap) into locationVar, weightVar blending to 1 in range and to 0 out of it. Anything else the rig needs from the
  game is an Anim Blueprint variable: set it in the event graph (Blueprint graph calls) or from the character.
- `AddLookAt(rig, bone, target, weight = "1", axis = "", after = "")`: target `"@Var"` (a world location), a rig element
  (`"LookAt_Ctrl"`) or `"(x, y, z)"` in rig space; axis `""` is the bone's axis facing the character's forward (+Y of a
  UE mesh); spread a look over the neck with a second call on `neck_01` at weight `"0.4"`.

Any rig unit (Forwards Solve is an execution chain: executing units run in the order added, at its end or after `after`;
pure units such as From World, math and interpolation run when their outputs are linked):

```
controlRigUnits | where DisplayName contains "Trace" or Keywords contains "spring" | select Name DisplayName Execute Pins
controlRig | call AddUnit("/Game/Rigs/CR_LookAt", "SpringInterpVectorV2", "Target=@LookAtTarget, Strength=8", "", "Smooth")
controlRig | call SetPin("/Game/Rigs/CR_LookAt", "LookAt_head_Target.Value", "@Smooth.Result")
controlRig | call Connect("/Game/Rigs/CR_LookAt", "Smooth.Result", "LookAt_head_Target.Value")      the same link
controlRig | call SetPin("/Game/Rigs/CR_LookAt", "LookAt_head.Secondary.Weight", "0.5")
controlRig | call RemoveNode("/Game/Rigs/CR_LookAt", "Smooth")
unreal.asset("/Game/Rigs/CR_LookAt") | call Compile()
```

- Unit names: `Name` (`AimItem`, `TwoBoneIKSimplePerItem`, `SphereTraceWorld`, `RigUnit_ToRigSpace_Location`), or a
  `DisplayName` one unit has. Pins: `Pins` of the unit row (a struct pin's members are `Primary.Target`) or the rig's
  `Nodes`; names and display names, case and spaces ignored.
- Values (settings `"Pin=Value, Sub.Pin=Value"`, SetPin): `"@Variable"` reads a rig variable, `"@Node.Pin"` links an
  output; an item pin takes an element name (`"head"`, `"Control:Target"`); vectors `"(0, 1, 0)"`, rotators
  `"(pitch, yaw, roll)"`, colours `"(r, g, b, a)"`; enums by name (`"Direction"`); otherwise the pin's text form as the
  `Nodes` member prints it.
- Spaces: units work in rig (component) space. A world location from the game goes through
  `RigUnit_ToRigSpace_Location` (From World); AddLookAt does that for `"@Var"` targets.
- Foot IK: copy and extend the template's rig (`editor | call CopyAsset(".../CR_Mannequin_FootIK", "/Game/Rigs/CR_FootIK")`)
  or build one: per foot `SphereTraceWorld` down from the foot, `TwoBoneIKSimplePerItem` (thigh, calf, foot) to the hit,
  the pelvis lowered by the larger offset (`OffsetTransformForItem`). Full Body IK (`PBIK`) is in the engine plugin
  FullBodyIK, which projects enable themselves.
- A bare rig (no mesh): `CreateRig(path)`, `AddBone(rig, name, parent, [x, y, z])` (relative to the parent, cm),
  `AddControl(rig, name, [x, y, z], parent = "")`, `AddTwoBoneIK(rig, boneA, boneB, effector, targetControl)`.
- Each mutation is one undo step; assets stay unsaved until `editor | call SaveAsset(path)`. Placed rig components:
  `actors | get Components | where $ is ControlRigComponent | select Owner.Label BoundRig`.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIControlRigRqlModule>, UnrealAIControlRig)
