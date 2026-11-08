# EnhancedInput RQL module

RQL module for Unreal's Enhanced Input system: author full control schemes (on foot, vehicle, menu): input actions, mapping contexts with priorities, keyboard / mouse / gamepad mappings, triggers (hold, tap, pulse, chords, combos, toggles) and modifiers (dead zones, negate, swizzle, scale), player-rebindable keys with Enhanced Input user settings, and the Blueprint wiring that swaps contexts on possession and binds actions to functions; query everything back.

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.26.0 or later (the RQL runtime and operations these extensions use), installed in the same project.
- Engine plugins: `EnhancedInput`. The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-enhanced-input "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAIEnhancedInput/`. Connect Unreal MCP to your AI client to use the RQL module below.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`, [docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill.

| Kind | Names |
| --- | --- |
| Sources | `inputActions`, `inputContexts`, `inputMappings` (every key mapping with triggers, modifiers and rebind settings), `inputTriggerTypes`, `inputModifierTypes`, `enhancedInput` |
| Assets and mappings (`call`) | `CreateAction`, `CreateContext`, `AddMapping`, `RemoveMapping`, `SetActionTriggers`, `SetActionModifiers`, `SetMappingTriggers`, `SetMappingModifiers` |
| Rebindable keys (`call`) | `SetPlayerMappable`, `SetMappingPlayerMappable`, `EnableUserSettings` |
| Blueprint wiring (`call`) | `AddMappingContextOnPossess`, `AddMappingContextOnEvent`, `RemoveMappingContextOnEvent`, `AddMappingContextOnBeginPlay`, `BindAction`, `BindActionToggle`, `AddActionEvent` |
| Play test (`call`) | `InjectInput` (with the core `editor | call StartPlay / AdvancePlay`) |
| Members | `InputMappingContext.KeyMappings`, `InputAction.TriggersText` / `ModifiersText` / `PlayerMappable`, `Blueprint.InputSetup` |

Triggers and modifiers are text: `"Hold(0.5)"`, `"Pressed"`, `"Tap(0.2)"`, `"ChordAction(/Game/Input/IA_Modifier)"`, `"DeadZone(LowerThreshold=0.2, Type=Axial)"`, `"Swizzle, Negate"`, `"Negate(Y)"`, `"Scalar(2)"`. The same text reads back from `TriggersText`, `ModifiersText`, `inputMappings` and `KeyMappings`; a wrong name or setting fails listing what exists.

A whole on-foot + vehicle scheme, with rebindable keys:

```
enhancedInput | call CreateAction("/Game/Input/IA_Move", "Axis2D")
enhancedInput | call CreateAction("/Game/Input/IA_Crouch", "Boolean", "Pressed")
enhancedInput | call CreateContext("/Game/Input/IMC_OnFoot")
[["W", "Swizzle"], ["S", "Swizzle, Negate"], ["A", "Negate"], ["D", ""], ["Gamepad_Left2D", "DeadZone(0.2)"]] | call enhancedInput.AddMapping("/Game/Input/IMC_OnFoot", "/Game/Input/IA_Move", $[0], $[1])
enhancedInput | call SetMappingTriggers("/Game/Input/IMC_Vehicle", "/Game/Input/IA_Interact", "E", "Hold(0.3)")
enhancedInput | call SetPlayerMappable("/Game/Input/IA_Jump", "Jump", "On Foot")
enhancedInput | call EnableUserSettings()
enhancedInput | call AddMappingContextOnPossess("/Game/BP_Hero", "/Game/Input/IMC_OnFoot")
enhancedInput | call AddMappingContextOnPossess("/Game/BP_Car", "/Game/Input/IMC_Vehicle")
enhancedInput | call BindAction("/Game/BP_Hero", "/Game/Input/IA_Jump", "Started", "Jump")
enhancedInput | call BindActionToggle("/Game/BP_Hero", "/Game/Input/IA_Crouch", "Crouch", "UnCrouch")
inputMappings | where Context == "IMC_Vehicle" | select Action Key Triggers Modifiers PlayerMappable
unreal.asset("/Game/BP_Hero") | select InputSetup
```

`AddMappingContextOnPossess` makes a pawn add its context to the player controlling it and remove it when the player leaves (On Controller Changed), so possessing a vehicle swaps the on-foot scheme for the vehicle one. Contexts are added with `bNotifyUserSettings`, so their player-mappable keys appear in the Enhanced Input user settings (rebinding) and stay listed after removal. `BindAction` calls a Blueprint function, custom event or inherited function (Jump, Crouch, AddMovementInput) on a trigger event (Started, Triggered, Ongoing, Canceled, Completed), passing an axis action's value; `BindActionToggle` alternates two functions with a FlipFlop. The Blueprint nodes come from the EnhancedInput plugin's `InputBlueprintNodes` module, a dependency of this extension only.

`InjectInput(action, value, seconds, frames, triggers)` play-tests the controls: it injects the action into player 0 of
the running Play In Editor session (`editor | call StartPlay()`) for exact 1/60 s frames simulated inside the call, then
returns the core play state (pawn, location, velocity, movement mode) with the action's trigger events frame by frame
in `Notes` (`IA_Move: Triggered x90, Completed x1 (90 frame(s) held + 1 released)`, then its value on the last held
frame). Every play state also lists the mapping contexts applied to player 0 (`MappingContexts`, read by the core).

```
editor | call StartPlay()
enhancedInput | call InjectInput("/Game/Input/IA_Move", [0, 1, 0], 1.5)
enhancedInput | call InjectInput("/Game/Input/IA_Jump")
```

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the RQL module API.

Licensed under [Apache License 2.0](LICENSE).
