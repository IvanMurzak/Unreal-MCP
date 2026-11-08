// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIEnhancedInputGraph.h"
#include "UnrealAIEnhancedInputRows.h"
#include "UnrealMcpRqlPlay.h"
#include "UnrealAIEnhancedInputRqlApi.h"
#include "UnrealAIEnhancedInputText.h"

#include "EnhancedActionKeyMapping.h"
#include "Engine/Blueprint.h"
#include "InputAction.h"
#include "InputMappingContext.h"

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Enhanced Input is queried and authored with
 * rql-query / rql-mutate instead of dedicated tools. Reflection exposes UInputAction (ValueType, Triggers, Modifiers,
 * ...) and UInputMappingContext; the module adds the asset and discovery sources, the `enhancedInput` authoring
 * operations (assets, mappings, triggers and modifiers as text, player-mappable keys, Blueprint wiring), the members
 * reflection does not show and the agent guide rendered into the unreal-rql skill.
 *
 * The version is the .uplugin VersionName.
 */
class FUnrealAIEnhancedInputRqlModule : public IUnrealMcpRqlModule
{
	/** A context's live key mappings (GetMappings: the 5.7+ DefaultKeyMappings) as [{Action, Key, Triggers?, Modifiers?, PlayerMappable?}]. */
	static TSharedPtr<FJsonValue> KeyMappingsJson(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> Mappings;
		if (const UInputMappingContext* MappingContext = Cast<UInputMappingContext>(Object))
		{
			for (const FEnhancedActionKeyMapping& Mapping : MappingContext->GetMappings())
				Mappings.Add(UnrealAIEnhancedInputRows::MappingJson(Mapping));
		}
		return MakeShared<FJsonValueArray>(Mappings);
	}

public:
	FUnrealAIEnhancedInputRqlModule()
	{
		// Every play state (editor | call StartPlay / AdvancePlay, editor.PlayState, InjectInput) lists player 0's mapping contexts.
		// The core fills them itself after 0.27.1; this fills them on an older core (minCoreVersion) and can go with it.
		PlayStateHandle = UnrealMcpRql::OnReadPlayState().AddLambda([](APlayerController* Controller, FUnrealMcpPlayState& State)
		{
			if (State.MappingContexts.IsEmpty())
				State.MappingContexts = UUnrealAIEnhancedInputRqlApi::AppliedContexts(Controller);
		});
	}

	virtual ~FUnrealAIEnhancedInputRqlModule() override
	{
		UnrealMcpRql::OnReadPlayState().Remove(PlayStateHandle);
	}

	virtual FString GetRqlModuleName() const override { return TEXT("EnhancedInput"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-enhanced-input"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAIEnhancedInput")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddAssetSource(TEXT("inputActions"), TEXT("Input Action assets (InputAction) of the project, at most 200"),
			UInputAction::StaticClass());
		Context.AddAssetSource(TEXT("inputContexts"), TEXT("Input Mapping Context assets (InputMappingContext) of the project, at most 200"),
			UInputMappingContext::StaticClass());
		Context.AddObjectListSource(TEXT("inputMappings"),
			TEXT("every key mapping of the project's mapping contexts: Context, Action, Key, Triggers, Modifiers, PlayerMappable, Settings, MappingName, DisplayName, DisplayCategory"),
			UUnrealAIEnhancedInputMappingRow::StaticClass(), []() { return UnrealAIEnhancedInputRows::Mappings(); });
		Context.AddObjectListSource(TEXT("inputTriggerTypes"),
			TEXT("the input triggers the operations take by name (Name, Class, Settings with defaults, Description): Pressed, Released, Hold, HoldAndRelease, Tap, Pulse, ChordAction, Combo, ..."),
			UUnrealAIEnhancedInputTypeRow::StaticClass(), []() { return UnrealAIEnhancedInputRows::Types(/*bTriggers*/ true); });
		Context.AddObjectListSource(TEXT("inputModifierTypes"),
			TEXT("the input modifiers the operations take by name (Name, Class, Settings with defaults, Description): Negate, SwizzleAxis, DeadZone, Scalar, ToWorldSpace, Smooth, ..."),
			UUnrealAIEnhancedInputTypeRow::StaticClass(), []() { return UnrealAIEnhancedInputRows::Types(/*bTriggers*/ false); });
		Context.AddApiSource(TEXT("enhancedInput"),
			TEXT("Enhanced Input authoring: call CreateAction / CreateContext / AddMapping / RemoveMapping / SetActionTriggers / SetActionModifiers / "
				"SetMappingTriggers / SetMappingModifiers / SetPlayerMappable / SetMappingPlayerMappable / EnableUserSettings / AddActionEvent / BindAction / "
				"BindActionToggle / AddMappingContextOnBeginPlay / AddMappingContextOnPossess / AddMappingContextOnEvent / RemoveMappingContextOnEvent / InjectInput (play test: simulate an input action inside the call)"),
			UUnrealAIEnhancedInputRqlApi::StaticClass(), TEXT("EnhancedInputOperations"));

		Context.AddJsonMember(UInputMappingContext::StaticClass(), TEXT("KeyMappings"),
			TEXT("the context's key mappings: Action, Key, Triggers / Modifiers (text, when the key has its own), PlayerMappable {Name, DisplayName, DisplayCategory, From} when rebindable"),
			&KeyMappingsJson);
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
		// Since UE 5.7 the reflected `Mappings` array is deprecated and stays empty (the mappings moved to DefaultKeyMappings):
		// an agent reading it saw [] beside a working mapping. It reads the live mappings too.
		Context.AddJsonMember(UInputMappingContext::StaticClass(), TEXT("Mappings"), TEXT("the context's key mappings (same as KeyMappings)"),
			&KeyMappingsJson);
#endif
		Context.AddJsonMember(UInputAction::StaticClass(), TEXT("TriggersText"),
			TEXT("the action's triggers as the text SetActionTriggers takes, e.g. \"Hold(HoldTimeThreshold=0.5)\" (\"\" = none: fires while actuated)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UInputAction* Action = Cast<UInputAction>(Object);
				return MakeShared<FJsonValueString>(Action ? UnrealAIEnhancedInputText::DescribeList(Action->Triggers) : FString());
			});
		Context.AddJsonMember(UInputAction::StaticClass(), TEXT("ModifiersText"),
			TEXT("the action's modifiers as the text SetActionModifiers takes, e.g. \"DeadZone(LowerThreshold=0.25)\""),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UInputAction* Action = Cast<UInputAction>(Object);
				return MakeShared<FJsonValueString>(Action ? UnrealAIEnhancedInputText::DescribeList(Action->Modifiers) : FString());
			});
		Context.AddJsonMember(UInputAction::StaticClass(), TEXT("PlayerMappable"),
			TEXT("the action's player-mappable key settings {Name, DisplayName, DisplayCategory}, or null when players cannot rebind it"),
			[](UObject* Object) { return UnrealAIEnhancedInputRows::PlayerMappableJson(Cast<UInputAction>(Object)); });
		Context.AddJsonMember(UBlueprint::StaticClass(), TEXT("InputSetup"),
			TEXT("the Blueprint's Enhanced Input wiring: Contexts [{Event, Op (Add / Remove), Context, Priority}], Actions [{Action, Started / Triggered / ... : what each pin runs, ActionValue}]"),
			[](UObject* Object) { return UnrealAIEnhancedInputGraph::InputSetupJson(Cast<UBlueprint>(Object)); });

		Context.AddGuide(TEXT("Enhanced Input"), TEXT(R"MD(Author whole control schemes with the `enhancedInput` operations, not C++ or Python: actions, one mapping
context per scheme (on foot, vehicle, menu), triggers and modifiers, rebindable keys, and the Blueprint wiring that
swaps schemes. Actions and contexts are assets (`inputActions`, `inputContexts`, `unreal.asset(path)`); change mappings
with these operations, not the context's own MapKey / UnmapKey or its DefaultKeyMappings struct.

```
inputMappings | where Context == "IMC_Default" | select Action Key Triggers Modifiers PlayerMappable     what exists
inputTriggerTypes | select Name Settings                inputModifierTypes | select Name Settings
enhancedInput | call CreateAction("/Game/Input/IA_Move", "Axis2D")                    Boolean | Axis1D | Axis2D | Axis3D
enhancedInput | call CreateAction("/Game/Input/IA_Sprint", "Boolean")                 no trigger: Started on press, Completed on release
enhancedInput | call CreateAction("/Game/Input/IA_Crouch", "Boolean", "Pressed")
enhancedInput | call CreateAction("/Game/Input/IA_Throttle", "Axis1D")
enhancedInput | call CreateContext("/Game/Input/IMC_OnFoot")
[["W", "Swizzle"], ["S", "Swizzle, Negate"], ["A", "Negate"], ["D", ""], ["Gamepad_Left2D", "DeadZone(0.2)"]] | call enhancedInput.AddMapping("/Game/Input/IMC_OnFoot", "/Game/Input/IA_Move", $[0], $[1])
["SpaceBar", "Gamepad_FaceButton_Bottom"] | call enhancedInput.AddMapping("/Game/Input/IMC_OnFoot", "/Game/Input/IA_Jump", $)
["LeftShift", "Gamepad_LeftThumbstick"] | call enhancedInput.AddMapping("/Game/Input/IMC_OnFoot", "/Game/Input/IA_Sprint", $)
[["W", ""], ["S", "Negate"], ["Gamepad_RightTriggerAxis", ""], ["Gamepad_LeftTriggerAxis", "Negate"]] | call enhancedInput.AddMapping("/Game/Input/IMC_Vehicle", "/Game/Input/IA_Throttle", $[0], $[1])
enhancedInput | call SetActionTriggers("/Game/Input/IA_Interact", "Hold(0.5)")       every key of the action
enhancedInput | call SetMappingTriggers("/Game/Input/IMC_OnFoot", "/Game/Input/IA_Interact", "Gamepad_FaceButton_Left", "Tap")      one key only
enhancedInput | call SetActionModifiers("/Game/Input/IA_Look", "Negate(Y)")
```

- **Triggers / modifiers are text**: a comma list of `Name` or `Name(value)` (the main setting) or `Name(Setting=value, ...)`.
  Triggers: `Down`, `Pressed`, `Released`, `Hold(seconds)`, `HoldAndRelease(seconds)`, `Tap(seconds)`, `Pulse(interval)`,
  `RepeatedTap`, `ChordAction(/Game/Input/IA_Modifier)` (only while that action fires: Shift+key combos),
  `Combo(/Game/Input/IA_A, /Game/Input/IA_B)`. Modifiers (applied in order, a mapping's before its action's): `Negate`,
  `Negate(Y)`, `Swizzle` (X and Y swapped: W/S into an Axis2D's Y), `DeadZone(0.2)`, `Scalar(2)` / `Scalar(1, 0.5, 1)`,
  `ToWorldSpace`, `Smooth`, `SmoothDelta`, `ResponseCurveExponential(2)`, `FOVScaling`, `ScaleByDeltaTime`. A wrong name or
  setting fails listing what exists. Read back as the same text: `TriggersText` / `ModifiersText` of an action,
  `Triggers` / `Modifiers` of `inputMappings` rows and `KeyMappings`.
- **Hold vs while held**: "sprint while Shift is held" needs no trigger (Started on press, Completed on release); `Hold(t)`
  fires only after t seconds; a toggle (crouch on C) is `BindActionToggle`. An action with no trigger fires Triggered
  every frame while actuated (axes: movement).
- **Keys**: FKey names: `W`, `SpaceBar`, `LeftShift`, `LeftControl`, `C`, `E`, `Escape`, `Gamepad_Left2D` (stick as one 2D
  key), `Gamepad_LeftX`, `Gamepad_RightX`, `Gamepad_FaceButton_Bottom` (A / Cross), `_Right` (B), `_Left` (X), `_Top` (Y),
  `Gamepad_LeftShoulder`, `Gamepad_RightTriggerAxis`, `Gamepad_LeftThumbstick` (L3), `Mouse2D`, `MouseX`, `LeftMouseButton`.

**Rebindable keys**: mark actions (or single mappings) player mappable and turn the user settings on; contexts added by
the operations below register their keys (`bNotifyUserSettings`), and removing a context keeps them listed.

```
enhancedInput | call SetPlayerMappable("/Game/Input/IA_Jump", "Jump", "On Foot")             DisplayName, DisplayCategory[, Name]
enhancedInput | call SetMappingPlayerMappable("/Game/Input/IMC_Vehicle", "/Game/Input/IA_Throttle", "Gamepad_RightTriggerAxis", "Ignore")
enhancedInput | call EnableUserSettings()
inputActions | where PlayerMappable != null | select Name PlayerMappable
inputMappings | where PlayerMappable == true | select Context Action Key MappingName DisplayCategory
```

A key-binding menu (UMG) reads them at runtime: `EnhancedInputLocalPlayerSubsystem.GetUserSettings()` ->
`GetActiveKeyProfile().GetPlayerMappingRows()`, rebinds with `MapPlayerKey` (MappingName, slot, key) and keeps them with
`SaveSettings`.

**Blueprint wiring** (Pawn, Character, vehicle Pawn, PlayerController Blueprints). A pawn owns its scheme:
`AddMappingContextOnPossess` adds the context to whoever controls it and removes it when that player leaves, so
possessing a car swaps on-foot for vehicle controls and the on-foot actions stop firing. Then bind each action to a
function (inherited ones such as Jump, StopJumping, Crouch, UnCrouch, or the Blueprint's own functions / custom events):

```
enhancedInput | call AddMappingContextOnPossess("/Game/BP_Hero", "/Game/Input/IMC_OnFoot", 0)
enhancedInput | call AddMappingContextOnPossess("/Game/BP_Car", "/Game/Input/IMC_Vehicle", 0)
enhancedInput | call BindAction("/Game/BP_Hero", "/Game/Input/IA_Jump", "Started", "Jump")
enhancedInput | call BindAction("/Game/BP_Hero", "/Game/Input/IA_Jump", "Completed", "StopJumping")
enhancedInput | call BindAction("/Game/BP_Hero", "/Game/Input/IA_Sprint", "Started", "StartSprint")      your functions / custom events
enhancedInput | call BindAction("/Game/BP_Hero", "/Game/Input/IA_Sprint", "Completed", "StopSprint")
enhancedInput | call BindActionToggle("/Game/BP_Hero", "/Game/Input/IA_Crouch", "Crouch", "UnCrouch")
enhancedInput | call BindAction("/Game/BP_Car", "/Game/Input/IA_Throttle", "Triggered", "SetThrottle")     ActionValue -> its float parameter
unreal.asset("/Game/BP_Hero") | select InputSetup                       Contexts (event, add / remove, priority) and what each action pin runs
unreal.asset("/Game/BP_Hero") | call Compile()
```

- `AddMappingContextOnEvent(bp, ctx, event, priority)` / `RemoveMappingContextOnEvent(bp, ctx, event)`: `BeginPlay`,
  `Possessed` / `Unpossessed` / `ControllerChanged` (pawns; Possessed / Unpossessed run on the server only, so prefer
  `AddMappingContextOnPossess`), `OnPossess` / `OnUnpossess` (PlayerControllers, server only), or a custom event (created
  when missing: call it from your enter / exit logic, e.g. a menu context). Only local players get a context. When two
  active contexts map the same key, the higher priority one wins while its action consumes input (bConsumeInput, the
  default); a context shared by both schemes (IMC_Common: Interact on E) is added on BeginPlay of the PlayerController.
  `AddMappingContextOnBeginPlay(bp, ctx)` is the one-context shortcut (on a pawn or another actor it adds to player 0).
- `BindAction(bp, action, event, function, value)`: events `Started`, `Triggered`, `Ongoing`, `Canceled`, `Completed`; a second
  bind on the same pin runs after the first; axis actions pass the ActionValue into the function's first parameter of
  that type (`value` names another, `"none"` skips it). `AddActionEvent` places the bare event node to wire by hand
  (`ref(node) | call Connect("Triggered", ref(other), "execute")`).
- Possession: the GameMode's Default Pawn Class (or Auto Possess Player on a placed pawn) is the first pawn; entering a
  vehicle is `PlayerController.Possess(car)` from the interact logic, leaving is `Possess(hero)`. Created assets are
  unsaved: `editor | call SaveAll()`. The legacy key events (`EventGraph | call AddInputKey("SpaceBar")`) need none of this.

)MD") TEXT(R"MD(Play test the controls (simulated inside the call, deterministic; never wait between calls for the editor to tick):

```
editor | call StartPlay()                                                       Play In Editor now, paused between calls; returns PlayState
enhancedInput | call InjectInput("/Game/Input/IA_Jump")                         one frame pressed (Value 1), then released
enhancedInput | call InjectInput("/Game/Input/IA_Move", [0, 1, 0], 1.5)        2D Move forward for 1.5 s
enhancedInput | call InjectInput("/Game/Input/IA_Sprint", [1, 0, 0], 0, 30, "Hold") 30 frames with an extra Hold trigger
editor | call AdvancePlay(1, 0, "W, LeftShift")                                 real keys through the mapping contexts
editor | call StopPlay()
```

Each returns the play state (PawnClass, Location, Velocity, Speed, MovementMode, Crouching, MaxWalkSpeed, MappingContexts =
the contexts applied to player 0); InjectInput adds Notes with the action's trigger events frame by frame ("IA_Jump:
Triggered x1, Completed x1") and its value: no event means its triggers did not fire for that Value. An event that fired with no
effect on the pawn means the pawn does not handle it (no action event in the possessed pawn or controller). Started fires with the first Triggered / Ongoing frame.)MD"));
	}

private:
	FDelegateHandle PlayStateHandle;
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIEnhancedInputRqlModule>, UnrealAIEnhancedInput)
