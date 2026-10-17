// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"

#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"          // UControlRigBlueprint (ControlRigDeveloper) — authoring asset + GetHierarchy()
#include "ControlRigComponent.h"                 // UControlRigComponent (ControlRig)
#include "Rigs/RigHierarchy.h"                   // URigHierarchy + element-key getters
#include "Rigs/RigHierarchyDefines.h"            // FRigElementKey / ERigElementType / ERigControlType
#include "Rigs/RigHierarchyElements.h"           // FRigBaseElement / FRigControlElement / FRigControlSettings

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIControlRig, Log, All);

namespace UnrealAIControlRigRqlLocal
{
	FString ElementTypeName(ERigElementType Type)
	{
		switch (Type)
		{
			case ERigElementType::Bone:      return TEXT("Bone");
			case ERigElementType::Null:      return TEXT("Null");
			case ERigElementType::Control:   return TEXT("Control");
			case ERigElementType::Curve:     return TEXT("Curve");
			case ERigElementType::Reference: return TEXT("Reference");
			case ERigElementType::Connector: return TEXT("Connector");
			case ERigElementType::Socket:    return TEXT("Socket");
			default:                         return TEXT("Unknown");
		}
	}

	FString ControlTypeName(ERigControlType Type)
	{
		const UEnum* Enum = StaticEnum<ERigControlType>();
		return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Type)) : FString(TEXT("Unknown"));
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
 * properties), a component's bound rig, and the agent guide rendered into the unreal-rql skill.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName (scripts/extension_release.py bump updates both).
 */
class FUnrealAIControlRigRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("ControlRig"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-control-rig"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.3"); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		using namespace UnrealAIControlRigRqlLocal;
		Context.AddAssetSource(TEXT("controlRigs"), TEXT("Control Rig blueprint assets (ControlRigBlueprint) of the project, at most 200"),
			UControlRigBlueprint::StaticClass());
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
						Entry->SetStringField(TEXT("Type"), ElementTypeName(Key.Type));
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
					for (const FRigElementKey& Key : Hierarchy->GetControlKeys(/*bTraverse=*/true))
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Name"), Key.Name.ToString());
						const FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key);
						Entry->SetStringField(TEXT("ControlType"), Control ? ControlTypeName(Control->Settings.ControlType) : FString());
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
					for (const FRigElementKey& Key : Hierarchy->GetBoneKeys(/*bTraverse=*/true))
					{
						const FRigElementKey Parent = Hierarchy->GetFirstParent(Key);
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Name"), Key.Name.ToString());
						Entry->SetStringField(TEXT("Parent"), Parent.IsValid() ? Parent.Name.ToString() : FString());
						Bones.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				return MakeShared<FJsonValueArray>(Bones);
			});
		Context.AddJsonMember(UControlRigComponent::StaticClass(), TEXT("BoundRig"),
			TEXT("the class of the Control Rig the component runs (null when none is instanced)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				UControlRigComponent* Component = Cast<UControlRigComponent>(Object);
				UControlRig* Rig = Component ? Component->GetControlRig() : nullptr;
				return Rig ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Rig->GetClass()->GetName()))
					: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
			});
		Context.AddGuide(TEXT("Control Rig"), TEXT(R"MD(Control Rig blueprints are assets: `controlRigs`. Their element hierarchy (bones, nulls, controls, curves) is in
`Elements` (counts), `Hierarchy`, `Controls` and `Bones`; Control Rig components are components:
`actors | get Components | where $ is ControlRigComponent` (`BoundRig` is the class of the running rig).

```
controlRigs | select $ref Name Elements
controlRigs | where Name == "CR_Mannequin" | select Controls
controlRigs | where Name == "CR_Mannequin" | get Bones | where Parent == "" | select Name
actors | get Components | where $ is ControlRigComponent | select Owner.Label BoundRig
```

The hierarchy is read-only through RQL; author rigs in the Control Rig editor.)MD"));
	}
};

/** Editor module that owns the RQL module and registers it as a modular feature, so Unreal-MCP discovers it. */
class FUnrealAIControlRigModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RqlModule = MakeUnique<FUnrealAIControlRigRqlModule>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
		UE_LOG(LogUnrealAIControlRig, Log, TEXT("[UnrealAIControlRig] registered RQL module '%s'."), *RqlModule->GetRqlModuleName());
	}

	virtual void ShutdownModule() override
	{
		if (RqlModule.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
			RqlModule.Reset();
		}
	}

private:
	TUniquePtr<FUnrealAIControlRigRqlModule> RqlModule;
};

IMPLEMENT_MODULE(FUnrealAIControlRigModule, UnrealAIControlRig)
