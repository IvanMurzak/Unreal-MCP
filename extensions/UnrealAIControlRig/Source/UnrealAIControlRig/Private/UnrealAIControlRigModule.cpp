// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIControlRigRqlApi.h"

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
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Control Rig is queried with rql-query instead of
 * dedicated tools. Reflection exposes the Control Rig blueprint and component properties; the module adds the
 * `controlRigs` asset source, a rig blueprint's element hierarchy (controls, bones, element counts — not reflected
 * properties), a component's bound rig, the `controlRig` authoring operations (UUnrealAIControlRigRqlApi) and the agent guide rendered into the unreal-rql skill.
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
		Context.AddApiSource(TEXT("controlRig"),
			TEXT("Control Rig authoring: call CreateRig(path) / AddBone(rig, name, parent, location) / AddControl(rig, name, location, parent) / AddTwoBoneIK(rig, boneA, boneB, effector, target)"),
			UUnrealAIControlRigRqlApi::StaticClass(), TEXT("ControlRigOperations"));
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
		Context.AddGuide(TEXT("Control Rig"), TEXT(R"MD(Control Rig blueprints are assets: `controlRigs`. Their element hierarchy (bones, nulls, controls, curves) is in
`Elements` (counts), `Hierarchy`, `Controls` and `Bones`; Control Rig components are components:
`actors | get Components | where $ is ControlRigComponent` (`BoundRig` is the running rig instance).

```
controlRigs | select $ref Name Elements
controlRigs | where Name == "CR_Mannequin" | select Controls
controlRigs | where Name == "CR_Mannequin" | get Bones | where Parent == "" | select Name
actors | get Components | where $ is ControlRigComponent | select Owner.Label BoundRig
```

Author rigs with `controlRig` (rql-mutate): create the asset, add bones (Location is relative to the parent bone, cm),
add a control for the animator, then make a two-bone chain follow it with a Basic IK solver on Forwards Solve. Pass the
rig as `unreal.asset("/Game/...")`; each call returns the new element or node name.

```
controlRig | call CreateRig("/Game/Rigs/CR_Arm")
controlRig | call AddBone(unreal.asset("/Game/Rigs/CR_Arm"), "Shoulder", "", [0, 0, 150])
controlRig | call AddBone(unreal.asset("/Game/Rigs/CR_Arm"), "Elbow", "Shoulder", [30, 0, 0])
controlRig | call AddBone(unreal.asset("/Game/Rigs/CR_Arm"), "Hand", "Elbow", [30, 0, 0])
controlRig | call AddControl(unreal.asset("/Game/Rigs/CR_Arm"), "IK_Target", [60, 0, 150])
controlRig | call AddTwoBoneIK(unreal.asset("/Game/Rigs/CR_Arm"), "Shoulder", "Elbow", "Hand", "IK_Target")
```

Other rig graph nodes are authored in the Control Rig editor.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIControlRigRqlModule>, UnrealAIControlRig)
