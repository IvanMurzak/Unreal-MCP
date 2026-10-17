// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIEnhancedInputRqlApi.h"

#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"

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
			TEXT("Enhanced Input operations: call CreateAction(path, valueType) / CreateContext(path) / AddMapping(context, action, key) / RemoveMapping(context, action, key)"),
			UUnrealAIEnhancedInputRqlApi::StaticClass(), TEXT("EnhancedInputOperations"));
		Context.AddJsonMember(UInputMappingContext::StaticClass(), TEXT("KeyMappings"), TEXT("the context's key mappings: Action, Key"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Mappings;
				if (const UInputMappingContext* MappingContext = Cast<UInputMappingContext>(Object))
				{
					for (const FEnhancedActionKeyMapping& Mapping : MappingContext->GetMappings())
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Action"), Mapping.Action ? Mapping.Action->GetName() : FString());
						Entry->SetStringField(TEXT("Key"), Mapping.Key.GetFName().ToString());
						Mappings.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				return MakeShared<FJsonValueArray>(Mappings);
			});
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
unreal.asset("/Game/Input/IA_Move") | set ValueType = "Axis2D"
```

Key names are Unreal FKey names (SpaceBar, W, LeftMouseButton, Gamepad_LeftX, Gamepad_FaceButton_Bottom). Created assets
are unsaved: `editor | call SaveAll()`.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIEnhancedInputRqlModule>, UnrealAIEnhancedInput)
