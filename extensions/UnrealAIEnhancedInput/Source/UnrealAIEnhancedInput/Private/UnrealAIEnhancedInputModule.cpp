// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIEnhancedInputRqlApi.h"

#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Enhanced Input is queried and changed with
 * rql-query / rql-mutate instead of dedicated tools. Reflection exposes UInputAction (ValueType, Triggers, Modifiers,
 * ...) and UInputMappingContext; the module adds the asset sources, the `enhancedInput` authoring operations, a
 * context's key mappings (not a reflected property) and the agent guide rendered into the unreal-rql skill.
 *
 * The version is the .uplugin VersionName.
 */
class FUnrealAIEnhancedInputRqlModule : public IUnrealMcpRqlModule
{
	/** A context's live key mappings (GetMappings: the 5.7+ DefaultKeyMappings, the Mappings array before) as [{Action, Key, Modifiers?}]. */
	static TSharedPtr<FJsonValue> KeyMappingsJson(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> Mappings;
		if (const UInputMappingContext* MappingContext = Cast<UInputMappingContext>(Object))
		{
			for (const FEnhancedActionKeyMapping& Mapping : MappingContext->GetMappings())
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("Action"), Mapping.Action ? Mapping.Action->GetName() : FString());
				Entry->SetStringField(TEXT("Key"), Mapping.Key.GetFName().ToString());
				TArray<TSharedPtr<FJsonValue>> Modifiers;
				for (const UInputModifier* Modifier : Mapping.Modifiers)
				{
					if (Modifier)
						Modifiers.Add(MakeShared<FJsonValueString>(UUnrealAIEnhancedInputRqlApi::ModifierDisplayName(Modifier->GetClass())));
				}
				if (Modifiers.Num() > 0)
					Entry->SetArrayField(TEXT("Modifiers"), Modifiers);
				Mappings.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		return MakeShared<FJsonValueArray>(Mappings);
	}

public:
	virtual FString GetRqlModuleName() const override { return TEXT("EnhancedInput"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-enhanced-input"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAIEnhancedInput")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddAssetSource(TEXT("inputActions"), TEXT("Input Action assets (InputAction) of the project, at most 200"),
			UInputAction::StaticClass());
		Context.AddAssetSource(TEXT("inputContexts"), TEXT("Input Mapping Context assets (InputMappingContext) of the project, at most 200"),
			UInputMappingContext::StaticClass());
		Context.AddApiSource(TEXT("enhancedInput"),
			TEXT("Enhanced Input operations: call CreateAction(path, valueType) / CreateContext(path) / AddMapping(context, action, key, modifiers) / RemoveMapping(context, action, key) / AddActionEvent(blueprint, action) / AddMappingContextOnBeginPlay(blueprint, context, priority)"),
			UUnrealAIEnhancedInputRqlApi::StaticClass(), TEXT("EnhancedInputOperations"));
		Context.AddJsonMember(UInputMappingContext::StaticClass(), TEXT("KeyMappings"), TEXT("the context's key mappings: Action, Key, Modifiers (when the key has any)"), &KeyMappingsJson);
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
		// Since UE 5.7 the reflected `Mappings` array is deprecated and stays empty (the mappings moved to DefaultKeyMappings):
		// an agent reading it saw [] beside a working mapping. It reads the live mappings too. (Before 5.7 it IS the live,
		// editable array: left as reflected.)
		Context.AddJsonMember(UInputMappingContext::StaticClass(), TEXT("Mappings"), TEXT("the context's key mappings: Action, Key, Modifiers (same as KeyMappings)"),
			&KeyMappingsJson);
#endif
		Context.AddGuide(TEXT("Enhanced Input"), TEXT(R"MD(Input Actions and Mapping Contexts are assets: `inputActions`, `inputContexts` (or `unreal.asset("/Game/Input/IA_Jump")`).
An action's members are its reflected properties (ValueType, Triggers, Modifiers, bConsumeInput, ...); a context lists its
bindings in `KeyMappings`.

```
inputActions | select $ref Name ValueType
inputContexts | select Name KeyMappings
enhancedInput | call CreateAction("/Game/Input/IA_Jump", "Boolean")            Boolean | Axis1D | Axis2D | Axis3D
enhancedInput | call CreateContext("/Game/Input/IMC_Default")
enhancedInput | call AddMapping("/Game/Input/IMC_Default", "/Game/Input/IA_Jump", "SpaceBar")
enhancedInput | call RemoveMapping("/Game/Input/IMC_Default", "/Game/Input/IA_Jump", "SpaceBar")
[["W", "Swizzle"], ["S", "Swizzle, Negate"], ["A", "Negate"], ["D", ""], ["Gamepad_Left2D", ""]] | call enhancedInput.AddMapping("/Game/Input/IMC_Default", "/Game/Input/IA_Move", $[0], $[1])
unreal.asset("/Game/Input/IA_Move") | set ValueType = "Axis2D"
```

Key names are Unreal FKey names (SpaceBar, W, LeftMouseButton, Gamepad_LeftX, Gamepad_FaceButton_Bottom). A key mapped to
an Axis2D action feeds its X: the 4th AddMapping argument lists modifiers (Negate, Swizzle = X and Y swapped, DeadZone,
Scalar, ...) so WASD drive a 2D Move (W Swizzle, S Swizzle + Negate, A Negate, D none; ActionValue X = right, Y = forward).
Created assets are unsaved: `editor | call SaveAll()`.

Blueprint input (a Pawn / Character / PlayerController Blueprint): the context must be added at BeginPlay, then each action
gets one event node whose exec pins (Triggered, Started, Ongoing, Canceled, Completed) and ActionValue are wired with the
core graph calls. The jump example:

```
enhancedInput | call AddMappingContextOnBeginPlay("/Game/BP_Hero", "/Game/Input/IMC_Default")      once per Blueprint (Priority optional)
enhancedInput | call AddActionEvent("/Game/BP_Hero", "/Game/Input/IA_Jump")                       returns K2Node_EnhancedInputAction#N
unreal.asset("/Game/BP_Hero").EventGraph | call AddCallFunction("Jump")                             returns K2Node_CallFunction#M
ref("K2Node_EnhancedInputAction#N") | call Connect("Triggered", ref("K2Node_CallFunction#M"), "execute")
unreal.asset("/Game/BP_Hero") | call Compile()
```

The pawn must be the one the player controls (the GameMode's Default Pawn Class, or Auto Possess Player = Player 0 on a
placed one). The legacy key events (`EventGraph | call AddInputKey("SpaceBar")`) need none of this.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIEnhancedInputRqlModule>, UnrealAIEnhancedInput)
